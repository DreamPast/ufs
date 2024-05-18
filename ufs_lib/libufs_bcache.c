#include "libufs_internel.h"

static int _ufs_bcache_sync(ufs_bcache_t* bcache) {
    int i, ec;
    if(bcache->n == 0) return 0;
    ec = ufs_jornal_do(bcache->jornal, bcache->fd, 1, bcache->ops, bcache->n);
    if(ul_unlikely(ec)) return ec;
    for(i = 0; i < bcache->n; ++i)
        if(bcache->flag[i] != UFS_BCACHE_ADD_REF) {
            ufs_free(ul_const_cast(void*, bcache->ops[i].buf));
        }
    bcache->n = 0;
    return 0;
}

UFS_HIDDEN int ufs_bcache_init(ufs_bcache_t* bcache, ufs_jornal_t* jornal, ufs_fd_t* fd) {
    bcache->jornal = jornal;
    bcache->fd = fd;
    bcache->n = 0;
    ulatomic_spinlock_unlock(&bcache->lock);
    return 0;
}
UFS_HIDDEN int ufs_bcache_sync(ufs_bcache_t* bcache) {
    int ec;
    ulatomic_spinlock_lock(&bcache->lock);
    ec = _ufs_bcache_sync(bcache);
    ulatomic_spinlock_unlock(&bcache->lock);
    return ec;
}
UFS_HIDDEN void ufs_bcache_deinit(ufs_bcache_t* bcache) {
    (void)bcache;
}
UFS_HIDDEN int ufs_bcache_add(ufs_bcache_t* bcache, const void* buf, uint64_t bnum, int flag) {
    int ec;
    ulatomic_spinlock_lock(&bcache->lock);
    if(bcache->n == UFS_JORNAL_NUM) {
        ec = _ufs_bcache_sync(bcache);
        if(ul_unlikely(ec)) goto do_return;
    }
    switch(flag) {
    case UFS_BCACHE_ADD_REF:
        bcache->ops[bcache->n].buf = buf;
        break;
    case UFS_BCACHE_ADD_COPY:
        bcache->ops[bcache->n].buf = ufs_malloc(UFS_BLOCK_SIZE);
        if(bcache->ops[bcache->n].buf == NULL) {
            ec = ENOMEM; goto do_return;
        }
        memcpy(ul_const_cast(void*, bcache->ops[bcache->n].buf), buf, UFS_BLOCK_SIZE);
        break;
    case UFS_BCACHE_ADD_MOVE:
        bcache->ops[bcache->n].buf = buf;
        break;
    default:
        ec = EINVAL; goto do_return;
    }
    bcache->flag[bcache->n] = flag;
    bcache->ops[bcache->n].bnum = bnum;
    ++bcache->n;

do_return:
    ulatomic_spinlock_unlock(&bcache->lock);
    return ec;
}
