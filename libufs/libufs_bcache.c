#include "libufs_internel.h"

static int _bcache_sync_jornal(ufs_bcache_t* bcache) {
    int i, ec;
    if(bcache->jornal_num == 0) return 0;
    ec = ufs_jornal_do(bcache->fd, bcache->jornal_ops, bcache->jornal_num);
    if(ul_unlikely(ec)) return ec;
    for(i = 0; i < bcache->jornal_num; ++i)
        if(bcache->jornal_flag[i] != UFS_BCACHE_ADD_REF) {
            ufs_free(ufs_const_cast(void*, bcache->jornal_ops[i].buf));
        }
    bcache->jornal_num = 0;
    return 0;
}

typedef struct _bcache_node_t {
    ulrb_node_t base;
    uint64_t bnum;
    void* buf;
} _bcache_node_t;

static int _bcache_node_comp(void* opaque, const void* lp, const void* rp) {
    const uint64_t lv = *ul_reinterpret_cast(const uint64_t*, lp);
    const uint64_t rv = *ul_reinterpret_cast(const uint64_t*, rp);
    (void)opaque; return lv < rv ? -1 : lv > rv;
}

static void _bcache_node_destroy(void* opaque, ulrb_node_t* _x) {
    _bcache_node_t* x = ul_reinterpret_cast(_bcache_node_t*, _x);
    (void)opaque;
    ufs_free(x->buf);
    ufs_free(x);
}

static int _bcache_sync_nojornal(ufs_bcache_t* bcache) {
    _bcache_node_t* node;
    ulrb_iter_t iter;
    int ec = 0;
    ulrb_iter_init(&iter, bcache->nojornal_root);
    for(;;) {
        node = ul_reinterpret_cast(_bcache_node_t*, ulrb_iter_next(&iter));
        if(node == NULL) break;
        ec = ufs_fd_pwrite_check(bcache->fd, node->buf, UFS_BLOCK_SIZE, ufs_fd_offset(node->bnum));
        if(ul_unlikely(ec)) return ec;
    }
    ulrb_destroy(&bcache->nojornal_root, _bcache_node_destroy, NULL);
    bcache->nojornal_num = 0;
    return 0;
}

static int _bcache_sync(ufs_bcache_t* bcache) {
    int ec = _bcache_sync_jornal(bcache);
    if(ul_unlikely(ec)) return ec;
    ec = _bcache_sync_nojornal(bcache);
    return ec;
}

static int _bcache_add_nojornal(ufs_bcache_t* bcache, const void* buf, uint64_t bnum, int flag) {
    _bcache_node_t* node;
    int ec = 0;
    node = ul_reinterpret_cast(_bcache_node_t*, ulrb_find(bcache->nojornal_root, &bnum, _bcache_node_comp, NULL));
    if(node != NULL) {
        memcpy(node->buf, buf, UFS_BLOCK_SIZE);
        goto do_return;
    }

    if(bcache->nojornal_num == UFS_BCACHE_NOJORNAL_LIMIT) {
        ec = _bcache_sync_nojornal(bcache);
        if(ul_unlikely(ec)) goto do_return;
    }

    node = ul_reinterpret_cast(_bcache_node_t*, ufs_malloc(sizeof(_bcache_node_t)));
    if(ul_unlikely(node == NULL)) { ec = ENOMEM; goto do_return; }
    if(flag == UFS_BCACHE_ADD_MOVE) {
        node->buf = ufs_const_cast(void*, buf);
        buf = NULL;
    } else {
        node->buf = ufs_malloc(UFS_BLOCK_SIZE);
        if(ul_unlikely(node->buf == NULL)) { ufs_free(node); ec = ENOMEM; goto do_return; }
    }
    node->bnum = bnum;
    
    ulrb_insert_unsafe(&bcache->nojornal_root, &node->base, _bcache_node_comp, NULL);
    ++bcache->nojornal_num;

do_return:
    if(ul_unlikely(UFS_BCACHE_ADD_MOVE)) ufs_free(ufs_const_cast(void*, buf));
    return ec;
}
static int _bcache_add_jornal(ufs_bcache_t* bcache, const void* buf, uint64_t bnum, int flag) {
    int ec, i;

    for(i = 0; i < bcache->jornal_num; ++i)
        if(ul_unlikely(bcache->jornal_ops[i].bnum == bnum)) {
            switch(flag) {
            case UFS_BCACHE_ADD_REF:
                if(bcache->jornal_flag[i] != UFS_BCACHE_ADD_REF)
                    ufs_free(ufs_const_cast(void*, bcache->jornal_ops[i].buf));
                bcache->jornal_ops[i].buf = buf;
                break;
            case UFS_BCACHE_ADD_COPY:
                if(bcache->jornal_flag[i] == UFS_BCACHE_ADD_REF) {
                    if((bcache->jornal_ops[i].buf = ufs_malloc(UFS_BLOCK_SIZE)) == NULL) return ENOMEM;
                }
                memcpy(ufs_const_cast(void*, bcache->jornal_ops[i].buf), buf, UFS_BLOCK_SIZE);
                break;
            case UFS_BCACHE_ADD_MOVE:
                if(bcache->jornal_flag[i] != UFS_BCACHE_ADD_REF)
                    ufs_free(ufs_const_cast(void*, bcache->jornal_ops[i].buf));
                bcache->jornal_ops[i].buf = buf;
                break;
            }
            bcache->jornal_flag[i] = flag;
            return 0;
        }


    if(bcache->jornal_num == UFS_JORNAL_NUM) {
        ec = _bcache_sync(bcache);
        if(ul_unlikely(ec)) {
            if(flag == UFS_BCACHE_ADD_MOVE) ufs_free(ufs_const_cast(void*, buf));
            return ec;
        }
    }
    switch(flag) {
    case UFS_BCACHE_ADD_REF:
        bcache->jornal_ops[bcache->jornal_num].buf = buf;
        break;
    case UFS_BCACHE_ADD_COPY:
        bcache->jornal_ops[bcache->jornal_num].buf = ufs_malloc(UFS_BLOCK_SIZE);
        if(bcache->jornal_ops[bcache->jornal_num].buf == NULL)
            return ENOMEM;
        memcpy(ufs_const_cast(void*, bcache->jornal_ops[bcache->jornal_num].buf), buf, UFS_BLOCK_SIZE);
        break;
    case UFS_BCACHE_ADD_MOVE:
        bcache->jornal_ops[bcache->jornal_num].buf = buf;
        buf = NULL;
        break;
    default:
        return EINVAL;
    }
    bcache->jornal_flag[bcache->jornal_num] = flag;
    bcache->jornal_ops[bcache->jornal_num].bnum = bnum;
    ++bcache->jornal_num;
    return 0;
}

static int _bcache_read_block(ufs_bcache_t* bcache, void* buf, uint64_t bnum) {
    for(int i = 0; i < bcache->jornal_num; ++i)
        if(bcache->jornal_ops[i].bnum == bnum) {
            memcpy(buf, bcache->jornal_ops[i].buf, UFS_BLOCK_SIZE);
            return 0;
        }
    
    do {
        _bcache_node_t* node = ul_reinterpret_cast(_bcache_node_t*,
            ulrb_find(bcache->nojornal_root, &bnum, _bcache_node_comp, NULL));
        if(node) {
            memcpy(buf, node->buf, UFS_BLOCK_SIZE);
            return 0;
        }
    } while(0);

    return ufs_fd_pread_check(bcache->fd, buf, UFS_BLOCK_SIZE, ufs_fd_offset(bnum));
}

UFS_HIDDEN int ufs_bcache_init(ufs_bcache_t* bcache, ufs_fd_t* fd) {
    bcache->fd = fd;
    bcache->jornal_num = 0;

    bcache->nojornal_root = NULL;
    bcache->nojornal_num = 0;

    return ulmtx_init(&bcache->lock);
}
UFS_HIDDEN int ufs_bcache_sync(ufs_bcache_t* bcache) {
    int ec;
    ulmtx_lock(&bcache->lock);
    ec = _bcache_sync(bcache);
    ulmtx_unlock(&bcache->lock);
    return ec;
}
UFS_HIDDEN void ufs_bcache_deinit(ufs_bcache_t* bcache) {
    ulmtx_destroy(&bcache->lock);
}

UFS_HIDDEN int ufs_bcache_add_block(ufs_bcache_t* bcache, const void* buf, uint64_t bnum, int flag) {
    int ec = 0;
    ulmtx_lock(&bcache->lock);
    if(flag & UFS_BCACHE_ADD_JORNAL) {
        ec = _bcache_add_jornal(bcache, buf, bnum, flag & 0xF);
    } else {
        ec = _bcache_add_nojornal(bcache, buf, bnum, flag & 0xF);
    }
    ulmtx_unlock(&bcache->lock);
    return ec;
}
UFS_HIDDEN int ufs_bcache_add(ufs_bcache_t* bcache, const void* buf, uint64_t bnum, size_t off, size_t len, int flag) {
    int ec = 0;
    char* tmp;
    ulmtx_lock(&bcache->lock);

    tmp = ul_reinterpret_cast(char*, ufs_malloc(UFS_BLOCK_SIZE));
    if(ul_unlikely(tmp == NULL)) { ec = ENOMEM; goto do_return; }
    ec = _bcache_read_block(bcache, tmp, bnum);
    memcpy(tmp + off, buf, len);

    if(flag & UFS_BCACHE_ADD_JORNAL) {
        ec = _bcache_add_jornal(bcache, buf, bnum, flag & 0xF);
    } else {
        ec = _bcache_add_nojornal(bcache, buf, bnum, flag & 0xF);
    }

do_return:
    ulmtx_unlock(&bcache->lock);
    return ec;

}

UFS_HIDDEN int ufs_bcache_read_block(ufs_bcache_t* bcache, void* buf, uint64_t bnum) {
    int ec;
    ulmtx_lock(&bcache->lock);
    ec = _bcache_read_block(bcache, buf, bnum);
    ulmtx_unlock(&bcache->lock);
    return ec;
}
UFS_HIDDEN int ufs_bcache_read(ufs_bcache_t* bcache, void* buf, uint64_t bnum, size_t off, size_t len) {
    int ec = 0;
    ulmtx_lock(&bcache->lock);
    
    for(int i = 0; i < bcache->jornal_num; ++i)
        if(bcache->jornal_ops[i].bnum == bnum) {
            memcpy(buf, ul_reinterpret_cast(const char*, bcache->jornal_ops[i].buf) + off, len);
            goto do_return;
        }
    
    {
        _bcache_node_t* node = ul_reinterpret_cast(_bcache_node_t*,
            ulrb_find(bcache->nojornal_root, &bnum, _bcache_node_comp, NULL));
        if(node) {
            memcpy(buf, ul_reinterpret_cast(char*, node->buf) + off, len);
            goto do_return;
        }
    }

    ec = ufs_fd_pread_check(bcache->fd, buf, len, ufs_fd_offset2(bnum, off));

do_return:
    ulmtx_unlock(&bcache->lock);
    return ec;
}
