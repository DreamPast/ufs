#include "libufs_internel.h"


// 对inode进行大小端转化
static void _trans_inode(ufs_inode_t* dest, const ufs_inode_t* src) {
    dest->nlink = ul_trans_u32_le(src->nlink);
    dest->mode = ul_trans_u16_le(src->mode);

    dest->size = ul_trans_u64_le(src->size);
    dest->blocks = ul_trans_u64_le(src->blocks);

    dest->ctime = ul_trans_i64_le(src->ctime);
    dest->mtime = ul_trans_i64_le(src->mtime);
    dest->atime = ul_trans_i64_le(src->atime);

    dest->uid = ul_trans_i32_le(src->uid);
    dest->gid = ul_trans_i32_le(src->gid);

    for(int i = 0; i < 16; ++i)
        dest->zones[i] = ul_trans_u64_le(src->zones[i]);
}


// 从inum中读取inode信息
static int _read_inode(ufs_t* ufs, ufs_inode_t* inode, uint64_t inum) {
    int ec;
    ec = ufs_jornal_read(&ufs->jornal, inode, inum / UFS_INODE_PER_BLOCK,
        (inum % UFS_INODE_PER_BLOCK) * UFS_INODE_DISK_SIZE, UFS_INODE_MEMORY_SIZE);
    if(ul_unlikely(ec)) goto do_return;

    _trans_inode(inode, inode);

do_return:
    return ec;
}
// 将inode提交到事务中
static int _write_inode(ufs_transcation_t* transcation, ufs_inode_t* inode, uint64_t inum) {
    ufs_inode_t* d = ul_reinterpret_cast(ufs_inode_t*, ufs_malloc(UFS_INODE_DISK_SIZE));
    if(ul_unlikely(d == NULL)) return ENOMEM;
    memset(ul_reinterpret_cast(char*, d) + UFS_INODE_MEMORY_SIZE, 0, UFS_INODE_DISK_SIZE - UFS_INODE_MEMORY_SIZE);
    _trans_inode(d, inode);
    return ufs_transcation_add(transcation, d, inum / UFS_INODE_PER_BLOCK,
        (inum % UFS_INODE_PER_BLOCK) * UFS_INODE_DISK_SIZE, UFS_INODE_DISK_SIZE, UFS_JORNAL_ADD_MOVE);
}
// 将inode提交到事务中（无锁版本）
static int _write_inode_nolock(ufs_transcation_t* transcation, ufs_inode_t* inode, uint64_t inum) {
    ufs_inode_t* d = ul_reinterpret_cast(ufs_inode_t*, ufs_malloc(UFS_INODE_DISK_SIZE));
    if(ul_unlikely(d == NULL)) return ENOMEM;
    memset(ul_reinterpret_cast(char*, d) + UFS_INODE_MEMORY_SIZE, 0, UFS_INODE_DISK_SIZE - UFS_INODE_MEMORY_SIZE);
    _trans_inode(d, inode);
    return ufs_transcation_nolock_add(transcation, d, inum / UFS_INODE_PER_BLOCK,
        (inum % UFS_INODE_PER_BLOCK) * UFS_INODE_DISK_SIZE, UFS_INODE_DISK_SIZE, UFS_JORNAL_ADD_MOVE);
}


// 从inode中定位块号
static int _seek_zone(ufs_minode_t* inode, uint64_t block, uint64_t* pznum) {
    int ec;

    if(block < 12) {
        *pznum = inode->inode.zones[block]; return 0;
    }
    block -= 12;

    if(block < UFS_ZNUM_PER_BLOCK * 2) {
        uint64_t bnum;
        if(block < UFS_ZNUM_PER_BLOCK) {
            bnum = inode->inode.zones[12];
        } else {
            bnum = inode->inode.zones[13];
            block -= UFS_ZNUM_PER_BLOCK;
        }
        if(bnum == 0) { *pznum = 0; return 0; }
        ec = ufs_jornal_read(&inode->ufs->jornal, &bnum, bnum, block * 8, sizeof(bnum));
        if(ul_unlikely(ec)) return ec;
        *pznum = ul_trans_u64_le(bnum);
        return 0;
    }
    block -= UFS_ZNUM_PER_BLOCK * 2;

    if(block < UFS_ZNUM_PER_BLOCK * UFS_ZNUM_PER_BLOCK) {
        uint64_t bnum = inode->inode.zones[14];

        if(bnum == 0) { *pznum = 0; return 0; }
        ec = ufs_jornal_read(&inode->ufs->jornal, &bnum, bnum, (block / UFS_ZNUM_PER_BLOCK) * 8, sizeof(bnum));
        if(ul_unlikely(ec)) return ec;
        bnum = ul_trans_u64_le(bnum);

        if(bnum == 0) { *pznum = 0; return 0; }
        ec = ufs_jornal_read(&inode->ufs->jornal, &bnum, bnum, (block % UFS_ZNUM_PER_BLOCK) * 8, sizeof(bnum));
        if(ul_unlikely(ec)) return ec;
        bnum = ul_trans_u64_le(bnum);

        *pznum = ul_trans_u64_le(bnum);
        return 0;
    }
    block -= UFS_ZNUM_PER_BLOCK * UFS_ZNUM_PER_BLOCK;

    if(block < UFS_ZNUM_PER_BLOCK * UFS_ZNUM_PER_BLOCK * UFS_ZNUM_PER_BLOCK) {
        uint64_t bnum = inode->inode.zones[15];

        if(bnum == 0) { *pznum = 0; return 0; }
        ec = ufs_jornal_read(&inode->ufs->jornal, &bnum, bnum, (block / (UFS_ZNUM_PER_BLOCK * UFS_ZNUM_PER_BLOCK)) * 8, sizeof(bnum));
        if(ul_unlikely(ec)) return ec;
        block %= UFS_ZNUM_PER_BLOCK * UFS_ZNUM_PER_BLOCK;
        bnum = ul_trans_u64_le(bnum);

        if(bnum == 0) { *pznum = 0; return 0; }
        ec = ufs_jornal_read(&inode->ufs->jornal, &bnum, bnum, (block / UFS_ZNUM_PER_BLOCK) * 8, sizeof(bnum));
        if(ul_unlikely(ec)) return ec;
        bnum = ul_trans_u64_le(bnum);

        if(bnum == 0) { *pznum = 0; return 0; }
        ec = ufs_jornal_read(&inode->ufs->jornal, &bnum, bnum, (block % UFS_ZNUM_PER_BLOCK) * 8, sizeof(bnum));
        if(ul_unlikely(ec)) return ec;
        bnum = ul_trans_u64_le(bnum);

        *pznum = ul_trans_u64_le(bnum);
        return 0;
    }

    return ERANGE;
}


// 写回zlist和inode，并提交事务
static int __end_zlist(ufs_minode_t* ufs_restrict inode, ufs_transcation_t* ufs_restrict transcation) {
    int ec;
    ec = ufs_zlist_sync(&inode->ufs->zlist);
    if(ul_unlikely(ec)) return ec;
    ec = _write_inode_nolock(transcation, &inode->inode, inode->inum);
    if(ul_unlikely(ec)) return ec;
    ec = ufs_transcation_nolock_commit_all(transcation);
    return ec;
}

static int __prealloc_zone1(ufs_minode_t* ufs_restrict inode, ufs_t* ufs_restrict ufs, int zk, uint64_t* ufs_restrict pblock) {
    int ec;
    ufs_transcation_t transcation;
    if(inode->inode.zones[zk] != 0) return 0;

    ufs_transcation_init(&transcation, &ufs->jornal);
    ufs_zlist_lock(&ufs->zlist, &transcation);

    ec = ufs_zlist_pop(&ufs->zlist, &inode->inode.zones[zk]);
    if(ul_unlikely(ec)) goto do_return;
    ec = ufs_transcation_nolock_add_zero_block(&transcation, inode->inode.zones[zk]);
    if(ul_unlikely(ec)) goto fail_to_alloc;
    ec = __end_zlist(inode, &transcation);
    if(ul_unlikely(ec)) goto fail_to_alloc;
    ++inode->inode.blocks;
    goto do_return;

fail_to_alloc:
    ufs_zlist_rollback(&ufs->zlist);
    inode->inode.zones[zk] = 0;

do_return:
    *pblock = inode->inode.zones[zk];
    ufs_zlist_unlock(&ufs->zlist);
    ufs_transcation_deinit(&transcation);
    return ec;
}
static int __prealloc_zone2(ufs_minode_t* ufs_restrict inode, ufs_t* ufs_restrict ufs, uint64_t k, uint64_t* ufs_restrict pblock) {

    int ec;
    ufs_transcation_t transcation;
    uint64_t oz[2], nz[2] = { 0 };

    ufs_transcation_init(&transcation, &ufs->jornal);
    ufs_zlist_lock(&ufs->zlist, &transcation);

    oz[0] = inode->inode.zones[15];
    if(oz[0] == 0) {
        ec = ufs_zlist_pop(&ufs->zlist, nz + 0);
        if(ul_unlikely(ec)) goto do_return;
        ++inode->inode.blocks;
        ec = ufs_transcation_nolock_add_zero_block(&transcation, nz[0]);
        if(ul_unlikely(ec)) goto fail_to_alloc;
        oz[1] = 0;
    } else {
        nz[0] = oz[0];
        ec = ufs_transcation_nolock_read(&transcation, oz + 1, nz[0], (k / UFS_ZNUM_PER_BLOCK) * 8, 8);
        if(ul_unlikely(ec)) goto fail_to_alloc;
        oz[1] = ul_trans_u64_le(oz[1]);
    }

    if(oz[1] == 0) {
        ec = ufs_zlist_pop(&ufs->zlist, nz + 1);
        if(ul_unlikely(ec)) goto fail_to_alloc;
        ++inode->inode.blocks;
        nz[1] = ul_trans_u64_le(nz[1]);
        ec = ufs_transcation_nolock_add(&transcation, nz + 1, nz[0], (k / UFS_ZNUM_PER_BLOCK) * 8, 8, UFS_JORNAL_ADD_COPY);
        nz[1] = ul_trans_u64_le(nz[1]);
        if(ul_unlikely(ec)) goto fail_to_alloc;
        ec = ufs_transcation_nolock_add_zero_block(&transcation, nz[1]);
        if(ul_unlikely(ec)) goto fail_to_alloc;
    }
    ec = __end_zlist(inode, &transcation);
    if(ul_unlikely(ec)) goto fail_to_alloc;
    goto do_return;

fail_to_alloc:
    ufs_zlist_rollback(&ufs->zlist);
    inode->inode.blocks -= (!oz[0] && nz[0]) + (!oz[1] && nz[1]);
    nz[0] = 0; nz[1] = 0;

do_return:
    inode->inode.zones[15] = nz[0];
    *pblock = nz[1];
    ufs_zlist_unlock(&ufs->zlist);
    ufs_transcation_deinit(&transcation);
    return ec;
}
static int __prealloc_zone3(ufs_minode_t* ufs_restrict inode, ufs_t* ufs_restrict ufs, uint64_t k, uint64_t* ufs_restrict pblock) {
    int ec;
    ufs_transcation_t transcation;
    uint64_t oz[3], nz[3] = { 0 };

    ufs_transcation_init(&transcation, &ufs->jornal);
    ufs_zlist_lock(&ufs->zlist, &transcation);

    oz[0] = inode->inode.zones[15];
    if(oz[0] == 0) {
        ec = ufs_zlist_pop(&ufs->zlist, nz + 0);
        if(ul_unlikely(ec)) goto do_return;
        ++inode->inode.blocks;
        ec = ufs_transcation_nolock_add_zero_block(&transcation, nz[0]);
        if(ul_unlikely(ec)) goto fail_to_alloc;
        oz[1] = 0;
    } else {
        nz[0] = oz[0];
        ec = ufs_transcation_nolock_read(&transcation, oz + 1, nz[0], (k / UFS_ZNUM_PER_BLOCK) * 8, 8);
        if(ul_unlikely(ec)) goto fail_to_alloc;
        oz[1] = ul_trans_u64_le(oz[1]);
    }

    if(oz[1] == 0) {
        ec = ufs_zlist_pop(&ufs->zlist, nz + 1);
        if(ul_unlikely(ec)) goto fail_to_alloc;
        ++inode->inode.blocks;
        nz[1] = ul_trans_u64_le(nz[1]);
        ec = ufs_transcation_nolock_add(&transcation, nz + 1, nz[0], (k / UFS_ZNUM_PER_BLOCK) * 8, 8, UFS_JORNAL_ADD_COPY);
        nz[1] = ul_trans_u64_le(nz[1]);
        if(ul_unlikely(ec)) goto fail_to_alloc;
        ec = ufs_transcation_nolock_add_zero_block(&transcation, nz[1]);
        if(ul_unlikely(ec)) goto fail_to_alloc;
        oz[2] = 0;
    } else {
        nz[1] = oz[1];
        ec = ufs_transcation_nolock_read(&transcation, oz + 2, nz[0], (k % UFS_ZNUM_PER_BLOCK) * 8, 8);
        if(ul_unlikely(ec)) goto fail_to_alloc;
    }

    if(oz[2] == 0) {
        ec = ufs_zlist_pop(&ufs->zlist, nz + 2);
        if(ul_unlikely(ec)) goto fail_to_alloc;
        ++inode->inode.blocks;
        nz[2] = ul_trans_u64_le(nz[2]);
        ec = ufs_transcation_nolock_add(&transcation, nz + 2, nz[1], (k % UFS_ZNUM_PER_BLOCK) * 8, 8, UFS_JORNAL_ADD_COPY);
        nz[2] = ul_trans_u64_le(nz[2]);
        if(ul_unlikely(ec)) goto fail_to_alloc;
        ec = ufs_transcation_nolock_add_zero_block(&transcation, nz[2]);
        if(ul_unlikely(ec)) goto fail_to_alloc;
    }
    ec = __end_zlist(inode, &transcation);
    if(ul_unlikely(ec)) goto fail_to_alloc;
    goto do_return;

fail_to_alloc:
    ufs_zlist_rollback(&ufs->zlist);
    inode->inode.blocks -= (!oz[0] && nz[0]) + (!oz[1] && nz[1]) + (!oz[2] && nz[2]);
    nz[0] = 0; nz[2] = 0;

do_return:
    inode->inode.zones[15] = nz[0];
    *pblock = nz[2];
    ufs_zlist_unlock(&ufs->zlist);
    ufs_transcation_deinit(&transcation);
    return ec;
}
static int _prealloc_zone(ufs_minode_t* ufs_restrict inode, uint64_t block, uint64_t* ufs_restrict pblock) {
    int ec = ERANGE;
    ufs_jornal_lock(&inode->ufs->jornal);
    block -= 12;

    if(block < UFS_ZNUM_PER_BLOCK) {
        ec = __prealloc_zone1(inode, inode->ufs, 12, pblock);
        goto do_return;
    }
    block -= UFS_ZNUM_PER_BLOCK;

    if(block < UFS_ZNUM_PER_BLOCK) {
        ec = __prealloc_zone1(inode, inode->ufs, 13, pblock);
        goto do_return;
    }
    block -= UFS_ZNUM_PER_BLOCK;

    if(block < UFS_ZNUM_PER_BLOCK * UFS_ZNUM_PER_BLOCK) {
        ec = __prealloc_zone2(inode, inode->ufs, block / UFS_ZNUM_PER_BLOCK, pblock);
        goto do_return;
    }
    block -= UFS_ZNUM_PER_BLOCK * UFS_ZNUM_PER_BLOCK;

    if(block < UFS_ZNUM_PER_BLOCK * UFS_ZNUM_PER_BLOCK * UFS_ZNUM_PER_BLOCK) {
        ec = __prealloc_zone3(inode, inode->ufs, block / UFS_ZNUM_PER_BLOCK, pblock);
        goto do_return;
    }

do_return:
    ufs_jornal_unlock(&inode->ufs->jornal);
    return ec;
}

static int __alloc_zone_0(ufs_minode_t* ufs_restrict inode, ufs_t* ufs_restrict ufs, uint64_t block, uint64_t* ufs_restrict pznum) {
    ufs_transcation_t transcation;
    int ec;
    if(inode->inode.zones[block]) { *pznum = inode->inode.zones[block]; return 0; }

    ufs_transcation_init(&transcation, &ufs->jornal);
    ufs_zlist_lock(&ufs->zlist, &transcation);

    ec = ufs_zlist_pop(&ufs->zlist, &inode->inode.zones[block]);
    if(ul_unlikely(ec)) goto do_return;
    ec = __end_zlist(inode, &transcation);
    if(ul_unlikely(ec)) goto fail_to_alloc;
    *pznum = inode->inode.zones[block];
    ++inode->inode.blocks;
    goto do_return;

fail_to_alloc:
    ufs_zlist_rollback(&ufs->zlist);
    inode->inode.zones[block] = 0;

do_return:
    ufs_zlist_unlock(&ufs->zlist);
    ufs_transcation_deinit(&transcation);
    return ec;
}
static int __alloc_zone_g(ufs_minode_t* ufs_restrict inode, ufs_t* ufs_restrict ufs, uint64_t block, uint64_t* ufs_restrict pznum) {
    uint64_t tznum, znum;
    int ec;
    ufs_transcation_t transcation;

    ec = _prealloc_zone(inode, block, &tznum);
    if(ul_unlikely(ec)) return ec;

    block -= 12;
    ec = ufs_jornal_read(&ufs->jornal, &znum, tznum, (block % UFS_ZNUM_PER_BLOCK) * 8, 8);
    if(ul_unlikely(ec)) return ec;
    if(znum != 0) { *pznum = ul_trans_u64_le(znum); return 0; }

    ufs_transcation_init(&transcation, &ufs->jornal);
    ufs_zlist_lock(&ufs->zlist, &transcation);
    ec = ufs_zlist_pop(&ufs->zlist, &znum);
    if(ul_unlikely(ec)) goto do_return;
    *pznum = znum;
    znum = ul_trans_u64_le(znum);
    ec = ufs_transcation_add(&transcation, &znum, tznum, (block % UFS_ZNUM_PER_BLOCK) * 8, 8, UFS_JORNAL_ADD_COPY);
    if(ul_unlikely(ec)) goto fail_to_alloc;
    ec = __end_zlist(inode, &transcation);
    if(ul_unlikely(ec)) goto fail_to_alloc;
    ++inode->inode.blocks;
    goto do_return;

fail_to_alloc:
    ufs_zlist_rollback(&ufs->zlist);

do_return:
    ufs_zlist_unlock(&ufs->zlist);
    ufs_transcation_deinit(&transcation);
    return ec;
}
static int _alloc_zone(ufs_minode_t* ufs_restrict inode, uint64_t block, uint64_t* ufs_restrict pznum) {
    if(block < 12) return __alloc_zone_0(inode, inode->ufs, block, pznum);
    else return __alloc_zone_g(inode, inode->ufs, block, pznum);
}

UFS_HIDDEN int ufs_minode_init(ufs_t* ufs_restrict ufs, ufs_minode_t* ufs_restrict inode, uint64_t inum) {
    int ec;
    ec = _read_inode(ufs, &inode->inode, inum);
    if(ul_unlikely(ec)) return ec;
    inode->ufs = ufs;
    inode->inum = inum;
    return 0;
}
UFS_HIDDEN int ufs_minode_create(ufs_t* ufs_restrict ufs, ufs_minode_t* ufs_restrict inode, const ufs_inode_create_t* ufs_restrict creat) {
    static ufs_inode_create_t _default_creat = { 0, 0, UFS_S_IFREG | 0664 };
    int ec;
    ufs_transcation_t transcation;
    uint64_t inum;
    creat = creat ? creat : &_default_creat;
    ufs_transcation_init(&transcation, &ufs->jornal);

    ufs_ilist_lock(&ufs->ilist, &transcation);
    ec = ufs_ilist_pop(&ufs->ilist, &inum);
    if(ul_unlikely(ec)) { ufs_ilist_unlock(&ufs->ilist); goto do_return; }
    ec = ufs_ilist_sync(&ufs->ilist);
    if(ul_unlikely(ec)) goto fail_to_alloc;

    inode->inode.nlink = 1;
    inode->inode.mode = creat->mode;
    inode->inode.size = 0;
    inode->inode.blocks = 0;

    inode->inode.ctime = ufs_time(0);
    inode->inode.atime = inode->inode.ctime;
    inode->inode.mtime = inode->inode.ctime;

    inode->inode.uid = creat->uid;
    inode->inode.gid = creat->gid;

    memset(inode->inode.zones, 0, sizeof(inode->inode.zones));
    ec = _write_inode(&transcation, &inode->inode, inum);
    if(ul_unlikely(ec)) goto fail_to_alloc;

    ec = ufs_transcation_commit_all(&transcation);
    if(ul_unlikely(ec)) goto fail_to_alloc;
    inode->ufs = ufs;
    inode->inum = inum;
    goto do_return;

fail_to_alloc:
    if(ul_unlikely(ufs_ilist_push(&ufs->ilist, inum)))
        ufs_errabort("libufs_minode.c", "ufs_minode_create", "the ilist is broken");

do_return:
    ufs_ilist_unlock(&ufs->ilist);
    ufs_transcation_deinit(&transcation);
    return ec;
}
UFS_HIDDEN int ufs_minode_deinit(ufs_minode_t* inode) {
    // 默认不自动同步
    (void)inode; return 0;
}


static int _trans_read(
    ufs_minode_t* ufs_restrict inode, ufs_transcation_t* ufs_restrict transcation,
    void* ufs_restrict buf, size_t len, uint64_t bnum, uint64_t off
) {
    return transcation
        ? ufs_transcation_read(transcation, buf, bnum, off, len)
        : ufs_fd_pread_check(inode->ufs->fd, buf, len, ufs_fd_offset2(bnum, off));
}
static int _trans_read_block(
    ufs_minode_t* ufs_restrict inode, ufs_transcation_t* ufs_restrict transcation,
    void* ufs_restrict buf, uint64_t bnum
) {
    return  transcation
        ? ufs_transcation_read_block(transcation, buf, bnum)
        : ufs_fd_pread_check(inode->ufs->fd, buf, UFS_BLOCK_SIZE, ufs_fd_offset(bnum));
}

static int _trans_write(
    ufs_minode_t* ufs_restrict inode, ufs_transcation_t* ufs_restrict transcation,
    const void* ufs_restrict buf, size_t len, uint64_t bnum, uint64_t off
) {
    return transcation
        ? ufs_transcation_add(transcation, buf, bnum, off, len, UFS_JORNAL_ADD_COPY)
        : ufs_fd_pwrite_check(inode->ufs->fd, buf, len, ufs_fd_offset2(bnum, off));
}
static int _trans_write_block(
    ufs_minode_t* ufs_restrict inode, ufs_transcation_t* ufs_restrict transcation,
    const void* ufs_restrict buf, uint64_t bnum
) {
    return transcation
        ? ufs_transcation_add_block(transcation, buf, bnum, UFS_JORNAL_ADD_COPY)
        : ufs_fd_pwrite_check(inode->ufs->fd, buf, UFS_BLOCK_SIZE, ufs_fd_offset(bnum));
}


static int _minode_pread(
    ufs_minode_t* ufs_restrict inode, ufs_transcation_t* ufs_restrict transcation,
    char* ufs_restrict buf, size_t len, uint64_t off, size_t* ufs_restrict pread
) {
    int ec;
    uint64_t znum;
    uint64_t block, boff;
    size_t nread;
    uint64_t rest_block;

    block = off / UFS_BLOCK_SIZE;
    boff = off % UFS_BLOCK_SIZE;
    ec = _seek_zone(inode, block, &znum);
    if(ul_unlikely(ec)) return ec;
    if(boff + len <= UFS_BLOCK_SIZE) {
        ec = _trans_read(inode, transcation, buf, len, znum, boff);
        if(ul_unlikely(ec)) return ec;
        *pread = len; return 0;
    }
    ec = _trans_read(inode, transcation, buf, UFS_BLOCK_SIZE - boff, znum, boff);
    if(ul_unlikely(ec)) return ec;
    nread = UFS_BLOCK_SIZE - boff;
    len -= nread;
    buf += nread;

    rest_block = len / UFS_BLOCK_SIZE;
    while(rest_block--) {
        ec = _seek_zone(inode, block++, &znum);
        if(ul_unlikely(ec)) { *pread = nread; return 0; }
        ec = _trans_read_block(inode, transcation, buf, znum);
        if(ul_unlikely(ec)) return ec;
        nread += UFS_BLOCK_SIZE;
        len -= UFS_BLOCK_SIZE;
        buf += UFS_BLOCK_SIZE;
    }

    if(len) {
        ec = _seek_zone(inode, block, &znum);
        if(ul_unlikely(ec)) { *pread = nread; return 0; }
        ec = _trans_read(inode, transcation, buf, len, znum, 0);
        nread += len;
    }
    *pread = nread; return 0;
}
UFS_HIDDEN int ufs_minode_pread(
    ufs_minode_t* ufs_restrict inode, ufs_transcation_t* ufs_restrict transcation,
    char* ufs_restrict buf, size_t len, uint64_t off, size_t* ufs_restrict pread
) {
    if(off + len > inode->inode.size) len = inode->inode.size - off;
    return _minode_pread(inode, transcation, buf, len, off, pread);
}

static int _minode_pwrite(
    ufs_minode_t* ufs_restrict inode, ufs_transcation_t* ufs_restrict transcation,
    const char* ufs_restrict buf, size_t len, uint64_t off, size_t* ufs_restrict pwriten
) {
    int ec;
    uint64_t znum;
    uint64_t block, boff;
    size_t nwriten;
    uint64_t rest_block;

    block = off / UFS_BLOCK_SIZE;
    boff = off % UFS_BLOCK_SIZE;
    ec = _alloc_zone(inode, block, &znum);
    if(ul_unlikely(ec)) return ec;
    if(boff + len <= UFS_BLOCK_SIZE) {
        ec = _trans_write(inode, transcation, buf, len, znum, boff);
        if(ul_unlikely(ec)) return ec;
        *pwriten = len; return 0;
    }
    ec = _trans_write(inode, transcation, buf, UFS_BLOCK_SIZE - boff, znum, boff);
    if(ul_unlikely(ec)) return ec;
    nwriten = UFS_BLOCK_SIZE - boff;
    len -= nwriten;
    buf += nwriten;

    rest_block = len / UFS_BLOCK_SIZE;
    while(rest_block--) {
        ec = _alloc_zone(inode, block++, &znum);
        if(ul_unlikely(ec)) { *pwriten = nwriten; return 0; }
        ec = _trans_write_block(inode, transcation, buf, znum);
        if(ul_unlikely(ec)) return ec;
        nwriten += UFS_BLOCK_SIZE;
        len -= UFS_BLOCK_SIZE;
        buf += UFS_BLOCK_SIZE;
    }

    if(len) {
        ec = _alloc_zone(inode, block, &znum);
        if(ul_unlikely(ec)) { *pwriten = nwriten; return 0; }
        ec = _trans_write(inode, transcation, buf, len, znum, 0);
        nwriten += len;
    }
    *pwriten = nwriten; return 0;
}
UFS_HIDDEN int ufs_minode_pwrite(
    ufs_minode_t* ufs_restrict inode, ufs_transcation_t* ufs_restrict transcation,
    const char* ufs_restrict buf, size_t len, uint64_t off, size_t* ufs_restrict pwriten
) {
    int ec = _minode_pwrite(inode, transcation, buf, len, off, pwriten);
    if(ul_unlikely(ec)) return ec;
    inode->inode.size = off + len > inode->inode.size ? off + len : inode->inode.size;
    return 0;
}


UFS_HIDDEN int ufs_minode_sync_meta(ufs_minode_t* inode) {
    ufs_transcation_t transcation;
    int ec;
    ufs_transcation_init(&transcation, &inode->ufs->jornal);
    _write_inode(&transcation, &inode->inode, inode->inum);
    ec = ufs_transcation_commit_all(&transcation);
    ufs_transcation_deinit(&transcation);
    if(ul_unlikely(ec)) return ec;
    return ufs_fd_sync(inode->ufs->fd);
}
UFS_HIDDEN int ufs_minode_sync(ufs_minode_t* inode, int only_data) {
    return only_data ? ufs_fd_sync(inode->ufs->fd) : ufs_minode_sync_meta(inode);
}


UFS_HIDDEN int ufs_minode_fallocate(ufs_minode_t* inode, uint64_t block, uint64_t* pblock) {
    int ec;
    uint64_t i;
    uint64_t znum;
    for(i = 0; i < block; ++i) {
        ec = _alloc_zone(inode, block, &znum);
        if(ul_unlikely(ec)) break;
    }
    *pblock = i;
    return ec;
}


// 保留buf的前block个zone，其余全部删除，并将删除的数量、删除后的视图同步到磁盘上
static int __minode_shrink_xr(ufs_minode_t* ufs_restrict inode, uint64_t znum, uint64_t block, uint64_t* ufs_restrict buf) {
    int ec;
    ufs_transcation_t transcation;
    uint64_t i;
    uint64_t oblocks;

    ufs_transcation_init(&transcation, &inode->ufs->jornal);
    ufs_zlist_lock(&inode->ufs->zlist, &transcation);

    oblocks = inode->inode.blocks;
    for(i = UFS_ZNUM_PER_BLOCK; i > block; ++i)
        if(buf[i - 1]) {
            ec = ufs_zlist_push(&inode->ufs->zlist, ul_trans_u64_le(buf[i - 1]));
            if(ul_unlikely(ec)) goto fail_to_shrink;
            --inode->inode.blocks;
            buf[i - 1] = 0;
        }
    ec = ufs_transcation_add_block(&transcation, buf, znum, UFS_JORNAL_ADD_MOVE);
    buf = NULL;
    if(ul_unlikely(ec)) goto fail_to_shrink;
    ec = __end_zlist(inode, &transcation);
    if(ul_unlikely(ec)) goto fail_to_shrink;
    goto do_return;

fail_to_shrink:
    ufs_zlist_rollback(&inode->ufs->zlist);
    inode->inode.blocks = oblocks;

do_return:
    ufs_free(buf);
    ufs_zlist_unlock(&inode->ufs->zlist);
    ufs_transcation_deinit(&transcation);
    return ec;
}
static int __minode_shrink_x1(ufs_minode_t* inode, uint64_t znum, uint64_t block) {
    int ec;
    uint64_t* buf;
    
    ufs_assert(znum != 0);
    buf = ul_reinterpret_cast(uint64_t*, ufs_malloc(UFS_BLOCK_SIZE));
    if(ul_unlikely(buf == NULL)) return ENOMEM;
    ec = ufs_jornal_read_block(&inode->ufs->jornal, buf, znum);
    if(ul_unlikely(ec)) { ufs_free(buf); return ENOMEM; }
    return __minode_shrink_xr(inode, znum, block, buf);
}
static int __minode_shrink_x2(ufs_minode_t* inode, uint64_t znum, uint64_t block) {
    int ec;
    ufs_transcation_t transcation;
    uint64_t i, oblocks, bq, br;
    uint64_t* buf;

    ufs_assert(znum != 0);
    buf = ul_reinterpret_cast(uint64_t*, ufs_malloc(UFS_BLOCK_SIZE));
    if(ul_unlikely(buf == NULL)) return ENOMEM;
    bq = block / UFS_ZNUM_PER_BLOCK;
    br = block % UFS_ZNUM_PER_BLOCK;
    ++bq;

    ec = ufs_jornal_read_block(&inode->ufs->jornal, buf, znum);
    if(ul_unlikely(ec)) { ufs_free(buf); return ec; }
    for(i = UFS_ZNUM_PER_BLOCK; i > bq; --i)
        if(buf[i - 1]) {
            ec = __minode_shrink_x1(inode, ul_trans_u64_le(buf[i - 1]), 0);
            if(ul_unlikely(ec)) { ufs_free(buf); return ec; }
        }
    ec = __minode_shrink_x1(inode, ul_trans_u64_le(buf[bq - 1]), br);
    if(ul_unlikely(ec)) { ufs_free(buf); return ec; }
    return __minode_shrink_xr(inode, znum, bq - !br, buf);
}
static int __minode_shrink_x3(ufs_minode_t* inode, uint64_t znum, uint64_t block) {
    int ec;
    ufs_transcation_t transcation;
    uint64_t i, oblocks, bq, br;
    uint64_t* buf;

    ufs_assert(znum != 0);
    buf = ul_reinterpret_cast(uint64_t*, ufs_malloc(UFS_BLOCK_SIZE));
    if(ul_unlikely(buf == NULL)) return ENOMEM;
    bq = block / (UFS_ZNUM_PER_BLOCK * UFS_ZNUM_PER_BLOCK);
    br = block % (UFS_ZNUM_PER_BLOCK * UFS_ZNUM_PER_BLOCK);
    ++bq;

    ec = ufs_jornal_read_block(&inode->ufs->jornal, buf, znum);
    if(ul_unlikely(ec)) { ufs_free(buf); return ec; }
    for(i = UFS_ZNUM_PER_BLOCK; i > bq; --i)
        if(buf[i - 1]) {
            ec = __minode_shrink_x2(inode, ul_trans_u64_le(buf[i - 1]), 0);
            if(ul_unlikely(ec)) { ufs_free(buf); return ec; }
        }
    ec = __minode_shrink_x2(inode, ul_trans_u64_le(buf[bq - 1]), br);
    if(ul_unlikely(ec)) { ufs_free(buf); return ec; }
    return __minode_shrink_xr(inode, znum, bq - !br, buf);
}
static int __minode_shrink0(ufs_minode_t* inode, uint64_t block) {
    int ec;
    ufs_transcation_t transcation;
    uint64_t oz[12], i;
    uint64_t oblocks;

    memcpy(oz, inode->inode.zones, sizeof(oz));
    oblocks = inode->inode.blocks;
    ufs_transcation_init(&transcation, &inode->ufs->jornal);
    ufs_zlist_lock(&inode->ufs->zlist, &transcation);

    for(i = block; i < 12; ++i)
        if(oz[i] != 0) {
            ec = ufs_zlist_push(&inode->ufs->zlist, oz[i]);
            if(ul_unlikely(ec)) goto fail_to_shrink;
            inode->inode.zones[i] = 0;
            --inode->inode.blocks;
        }
    ec = __end_zlist(inode, &transcation);
    if(ul_unlikely(ec)) goto fail_to_shrink;
    goto do_return;

fail_to_shrink:
    memcpy(inode->inode.zones, oz, sizeof(oz));
    inode->inode.blocks = oblocks;
    ufs_zlist_rollback(&inode->ufs->zlist);

do_return:
    ufs_zlist_unlock(&inode->ufs->zlist);
    ufs_transcation_deinit(&transcation);
    return ec;
}
static int __minode_shrink1(ufs_minode_t* inode, uint64_t block, int zk) {
    int ec;
    uint64_t oz;

    oz = inode->inode.zones[zk];
    if(oz == 0) return 0;
    ec = __minode_shrink_x1(inode, oz, block);
    if(ul_unlikely(ec)) return ec;

    if(block == 0) {
        ufs_transcation_t transcation;
        uint64_t i, oblocks;
        oblocks = inode->inode.blocks;
        ufs_transcation_init(&transcation, &inode->ufs->jornal);
        ufs_zlist_lock(&inode->ufs->zlist, &transcation);
        --inode->inode.blocks;
        inode->inode.zones[zk] = 0;
        ec = ufs_zlist_push(&inode->ufs->zlist, oz);
        if(ul_unlikely(ec)) goto fail_to_shrink;
        ec = __end_zlist(inode, &transcation);
        if(ul_unlikely(ec)) goto fail_to_shrink;
        goto do_return;
        
    fail_to_shrink:
        inode->inode.blocks = oblocks;
        inode->inode.zones[zk] = oz;

    do_return:
        ufs_zlist_unlock(&inode->ufs->zlist);
        ufs_transcation_deinit(&transcation);
        return ec;
    }
    return 0;
}
static int __minode_shrink2(ufs_minode_t* inode, uint64_t block) {
    int ec;
    uint64_t oz;

    oz = inode->inode.zones[14];
    if(oz == 0) return 0;
    ec = __minode_shrink_x2(inode, oz, block);
    if(ul_unlikely(ec)) return ec;

    if(block == 0) {
        ufs_transcation_t transcation;
        uint64_t i, oblocks;
        oblocks = inode->inode.blocks;
        ufs_transcation_init(&transcation, &inode->ufs->jornal);
        ufs_zlist_lock(&inode->ufs->zlist, &transcation);
        --inode->inode.blocks;
        inode->inode.zones[14] = 0;
        ec = ufs_zlist_push(&inode->ufs->zlist, oz);
        if(ul_unlikely(ec)) goto fail_to_shrink;
        ec = __end_zlist(inode, &transcation);
        if(ul_unlikely(ec)) goto fail_to_shrink;
        goto do_return;
        
    fail_to_shrink:
        inode->inode.blocks = oblocks;
        inode->inode.zones[14] = oz;

    do_return:
        ufs_zlist_unlock(&inode->ufs->zlist);
        ufs_transcation_deinit(&transcation);
        return ec;
    }
    return 0;
}
static int __minode_shrink3(ufs_minode_t* inode, uint64_t block) {
    int ec;
    uint64_t oz;

    oz = inode->inode.zones[15];
    if(oz == 0) return 0;
    ec = __minode_shrink_x3(inode, oz, block);
    if(ul_unlikely(ec)) return ec;

    if(block == 0) {
        ufs_transcation_t transcation;
        uint64_t i, oblocks;
        oblocks = inode->inode.blocks;
        ufs_transcation_init(&transcation, &inode->ufs->jornal);
        ufs_zlist_lock(&inode->ufs->zlist, &transcation);
        --inode->inode.blocks;
        inode->inode.zones[15] = 0;
        ec = ufs_zlist_push(&inode->ufs->zlist, oz);
        if(ul_unlikely(ec)) goto fail_to_shrink;
        ec = __end_zlist(inode, &transcation);
        if(ul_unlikely(ec)) goto fail_to_shrink;
        goto do_return;
        
    fail_to_shrink:
        inode->inode.blocks = oblocks;
        inode->inode.zones[15] = oz;

    do_return:
        ufs_zlist_unlock(&inode->ufs->zlist);
        ufs_transcation_deinit(&transcation);
        return ec;
    }
    return 0;
}
UFS_HIDDEN int ufs_minode_shrink(ufs_minode_t* inode, uint64_t block) {
    int ec;
    uint64_t B = 12 + UFS_ZNUM_PER_BLOCK * 2 + UFS_ZNUM_PER_BLOCK * UFS_ZNUM_PER_BLOCK;

    if(block > B + UFS_ZNUM_PER_BLOCK * UFS_ZNUM_PER_BLOCK * UFS_ZNUM_PER_BLOCK) return 0;

    if(block > B) {
        ec = __minode_shrink3(inode, block - B);
        if(ul_unlikely(ec)) return ec;
        block = B;
    }
    B -= UFS_ZNUM_PER_BLOCK * UFS_ZNUM_PER_BLOCK;

    if(block > B) {
        ec = __minode_shrink2(inode, block - B);
        if(ul_unlikely(ec)) return ec;
        block = B;
    }
    B -= UFS_ZNUM_PER_BLOCK;

    if(block > B) {
        ec = __minode_shrink1(inode, block, 13);
        if(ul_unlikely(ec)) return ec;
        block = B;
    }
    B -= UFS_ZNUM_PER_BLOCK;

    if(block > B) {
        ec = __minode_shrink1(inode, block, 12);
        if(ul_unlikely(ec)) return ec;
        block = B;
    }

    ec = __minode_shrink0(inode, block);
    return ec;
}


UFS_HIDDEN int ufs_minode_resize(ufs_minode_t* inode, uint64_t size) {
    int ec;
    uint64_t osize = inode->inode.size;

    if(size >= inode->inode.size) {
        inode->inode.size = size;
    } else {
        uint64_t sz = (inode->inode.size - size) / 2 + size;
        ec = ufs_minode_shrink(inode, (sz + UFS_BLOCK_SIZE - 1) / UFS_BLOCK_SIZE);
        if(ul_unlikely(ec)) return ec;
        inode->inode.size = size;
    }

    ufs_transcation_t transcation;
    ufs_transcation_init(&transcation, &inode->ufs->jornal);
    ec = _write_inode(&transcation, &inode->inode, inode->inum);
    if(ul_unlikely(ec)) goto fail;
    ec = ufs_transcation_commit_all(&transcation);
    if(ul_unlikely(ec)) goto fail;
    goto do_return;

fail:
    inode->inode.size = size;
do_return:
    ufs_transcation_deinit(&transcation);
    return ec;
}


static void _ufs_inode_debug(const ufs_inode_t* inode, FILE* fp, const char* title) {
    fprintf(fp, "%s [%p]\n", title, ufs_const_cast(void*, inode));
    fprintf(fp, "\tlink: %" PRIu32 "\n", inode->nlink);
    fprintf(fp, "\tmode: %" PRIu16 "\n", inode->mode);

    fprintf(fp, "\tsize: %" PRIu64 "\n", inode->size);
    fprintf(fp, "\tblock: %" PRIu64 "\n", inode->blocks);

    fprintf(fp, "\tcreate time: "); ufs_ptime(inode->ctime, NULL, fp); fputc('\n', fp);
    fprintf(fp, "\tmodify time: "); ufs_ptime(inode->mtime, NULL, fp); fputc('\n', fp);
    fprintf(fp, "\taccess time: "); ufs_ptime(inode->atime, NULL, fp); fputc('\n', fp);

    fprintf(fp, "\tuid: %" PRIi32 "\n", inode->uid);
    fprintf(fp, "\tgid: %" PRIi32 "\n", inode->gid);

    do {
        int i;
        fprintf(fp, "\tzones0: ");
        for(i = 0; i < 12; ++i)
            fprintf(fp, "[%" PRIu64 "]", inode->zones[i]);
        fputc('\n', fp);

        fprintf(fp, "\tzones1: ");
        for(i = 12; i < 14; ++i)
            fprintf(fp, "[%" PRIu64 "]", inode->zones[i]);
        fputc('\n', fp);

        fprintf(fp, "\tzones2: [%" PRIu64 "]\n", inode->zones[14]);
        fprintf(fp, "\tzones3: [%" PRIu64 "]\n", inode->zones[15]);
    } while(0);
}

UFS_HIDDEN void ufs_inode_debug(const ufs_inode_t* inode, FILE* fp) {
    _ufs_inode_debug(inode, fp, "inode");
}

UFS_HIDDEN void ufs_minode_debug(const ufs_minode_t* inode, FILE* fp) {
    _ufs_inode_debug(&inode->inode, fp, "minode");
    fprintf(fp, "\tufs: [%p]\n", ufs_const_cast(void*, inode->ufs));
    fprintf(fp, "\tinum: [%" PRIu64 "]\n", inode->inum);
}
