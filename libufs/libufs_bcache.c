#include "libufs_internel.h"

static void _bcache_shl(ufs_bcache_t* bcache, int num) {
    int i;
    ufs_assert(num < bcache->jornal_num);
    for(i = 0; i < num; ++i)
        if(bcache->jornal_flag[i] & _UFS_BCACHE_ADD_ALLOC) {
            ufs_free(ufs_const_cast(void*, bcache->jornal_ops[i].buf));
        }
    num = bcache->jornal_num - num;
    if(num) {
        memmove(bcache->jornal_ops, bcache->jornal_ops + num, ul_static_cast(size_t, num) * sizeof(bcache->jornal_ops[0]));
        memmove(bcache->jornal_flag, bcache->jornal_flag + num, ul_static_cast(size_t, num) * sizeof(bcache->jornal_flag[0]));
    }
    bcache->jornal_num = num;
}

static int _bcache_sync(ufs_bcache_t* bcache, int num) {
    int ec;
    if(ul_unlikely(num == 0)) return 0;
    ec = ufs_jornal_do(bcache->fd, bcache->jornal_ops, num);
    if(ul_unlikely(ec)) return ec;
    _bcache_shl(bcache, num);
    return 0;
}

static int _bcache_merge_item(ufs_bcache_t* ufs_restrict bcache, int k, const void* ufs_restrict buf, int flag) {
    switch(flag & 3) {
    case UFS_BCACHE_ADD_REF:
        if(bcache->jornal_flag[k] & _UFS_BCACHE_ADD_ALLOC)
            ufs_free(ufs_const_cast(void*, bcache->jornal_ops[k].buf));
        bcache->jornal_ops[k].buf = buf;
        break;
    case UFS_BCACHE_ADD_COPY:
        if(!(bcache->jornal_flag[k] & _UFS_BCACHE_ADD_ALLOC)) {
            if((bcache->jornal_ops[k].buf = ufs_malloc(UFS_BLOCK_SIZE)) == NULL) return ENOMEM;
        }
        memcpy(ufs_const_cast(void*, bcache->jornal_ops[k].buf), buf, UFS_BLOCK_SIZE);
        break;
    case UFS_BCACHE_ADD_MOVE:
        if(bcache->jornal_flag[k] & _UFS_BCACHE_ADD_ALLOC)
            ufs_free(ufs_const_cast(void*, bcache->jornal_ops[k].buf));
        bcache->jornal_ops[k].buf = buf;
        break;
    }
    bcache->jornal_flag[k] = flag & 3;
    return 0;
}

static int _bcache_add_jornal(ufs_bcache_t* ufs_restrict bcache, const void* ufs_restrict buf, uint64_t bnum, int flag) {
    int i;

    if(flag & UFS_BCACHE_ADD_MERGE) {
        for(i = 0; i < bcache->jornal_num; ++i)
            if(ul_unlikely(bcache->jornal_ops[i].bnum == bnum)) return _bcache_merge_item(bcache, i, buf, flag);
    }

    if(bcache->jornal_num >= UFS_BCACHE_MAX_LEN) {
        if((flag & 3) == UFS_BCACHE_ADD_MOVE) ufs_free(ufs_const_cast(void*, buf));
        return ERANGE;
    }
    switch(flag & 3) {
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
    bcache->jornal_flag[bcache->jornal_num] = flag & 3;
    bcache->jornal_ops[bcache->jornal_num].bnum = bnum;
    ++bcache->jornal_num;
    return 0;
}

UFS_HIDDEN int ufs_bcache_init(ufs_bcache_t* ufs_restrict bcache, ufs_fd_t* ufs_restrict fd) {
    bcache->fd = fd;
    bcache->jornal_num = 0;
    ulatomic_spinlock_init(&bcache->lock);
    return 0;
}
UFS_HIDDEN void ufs_bcache_deinit(ufs_bcache_t* bcache) { (void)bcache; }

UFS_HIDDEN int ufs_bcache_sync(ufs_bcache_t* bcache) {
    return _bcache_sync(bcache, bcache->jornal_num);
}
UFS_HIDDEN int ufs_bcache_sync_part(ufs_bcache_t* bcache, int num) {
    return _bcache_sync(bcache, num);
}

UFS_HIDDEN int ufs_bcache_add_block(ufs_bcache_t* ufs_restrict bcache, const void* ufs_restrict buf, uint64_t bnum, int flag) {
    return _bcache_add_jornal(bcache, buf, bnum, flag);
}
UFS_HIDDEN int ufs_bcache_add(ufs_bcache_t* ufs_restrict bcache, const void* ufs_restrict buf, uint64_t bnum, size_t off, size_t len, int flag) {
    int ec = 0;
    char* tmp;
    tmp = ul_reinterpret_cast(char*, ufs_malloc(UFS_BLOCK_SIZE));
    if(ul_unlikely(tmp == NULL)) { ec = ENOMEM; goto do_return; }
    ec = ufs_bcache_read_block(bcache, tmp, bnum);
    if(ul_unlikely(ec)) goto do_return;
    memcpy(tmp + off, buf, len);
    ec = _bcache_add_jornal(bcache, buf, bnum, (flag & ~3) | UFS_BCACHE_ADD_MOVE);
do_return:
    if((flag & 3) == UFS_BCACHE_ADD_MOVE) ufs_free(ufs_const_cast(void*, buf));
    return ec;
}

UFS_HIDDEN int ufs_bcache_read_block(ufs_bcache_t* ufs_restrict bcache, void* ufs_restrict buf, uint64_t bnum) {
    for(int i = bcache->jornal_num - 1; i >= 0; --i)
        if(bcache->jornal_ops[i].bnum == bnum) {
            memcpy(buf, bcache->jornal_ops[i].buf, UFS_BLOCK_SIZE);
            return 0;
        }
    return ufs_fd_pread_check(bcache->fd, buf, UFS_BLOCK_SIZE, ufs_fd_offset(bnum));
}
UFS_HIDDEN int ufs_bcache_read(ufs_bcache_t* ufs_restrict bcache, void* ufs_restrict buf, uint64_t bnum, size_t off, size_t len) {
    for(int i = bcache->jornal_num - 1; i >= 0; --i)
        if(bcache->jornal_ops[i].bnum == bnum) {
            memcpy(buf, ul_reinterpret_cast(const char*, bcache->jornal_ops[i].buf) + off, len);
            return 0;
        }
    return ufs_fd_pread_check(bcache->fd, buf, len, ufs_fd_offset2(bnum, off));
}

UFS_HIDDEN int ufs_bcache_copy(ufs_bcache_t* ufs_restrict dest, const ufs_bcache_t* ufs_restrict src) {
    dest->fd = src->fd;
    memcpy(dest->jornal_ops, src->jornal_ops, sizeof(dest->jornal_ops));
    memcpy(dest->jornal_flag, src->jornal_flag, sizeof(dest->jornal_flag));
    dest->jornal_num = src->jornal_num;
    ulatomic_spinlock_init(&dest->lock);
}
UFS_HIDDEN void ufs_bcache_compress(ufs_bcache_t* bcache) {
    int i, j;
    for(i = bcache->jornal_num - 1; i >= 0; --i)
        for(j = i - 1; j >= 0; --j)
            if(bcache->jornal_ops[j].bnum == bcache->jornal_ops[i].bnum) {
                if(!(bcache->jornal_flag[j] & _UFS_BCACHE_ADD_ALLOC))
                    ufs_free(ufs_const_cast(void*, bcache->jornal_ops[j].buf));
                bcache->jornal_flag[j] = bcache->jornal_flag[i];
                bcache->jornal_ops[j] = bcache->jornal_ops[i];
                bcache->jornal_ops[i].buf = NULL;
                break;
            }
    for(i = 0, j = 0; i < bcache->jornal_num; ++i) {
        if(bcache->jornal_ops[i].buf) {
            bcache->jornal_flag[j] = bcache->jornal_flag[i];
            bcache->jornal_ops[j] = bcache->jornal_ops[i];
            ++j;
        }
    }
    bcache->jornal_num = j;
}
UFS_HIDDEN void ufs_bcache_settop(ufs_bcache_t* bcache, int top) {
    int i;
    ufs_assert(top < bcache->jornal_num);
    for(i = top; i < bcache->jornal_num; ++i)
        if(bcache->jornal_flag[i] & _UFS_BCACHE_ADD_ALLOC) {
            ufs_free(ufs_const_cast(void*, bcache->jornal_ops[i].buf));
        }
    bcache->jornal_num = top;
}
