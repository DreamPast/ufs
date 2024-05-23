#include "libufs_internel.h"

// 对inode进行大小端转化
static void _trans_inode(ufs_inode_t* dest, const ufs_inode_t* src) {
    dest->nlink = ul_trans_u32_le(src->nlink);
    dest->mode = ul_trans_u16_le(src->mode);
    dest->size = ul_trans_u64_le(src->size);
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
    ec = ufs_jmanager_read(&ufs->jmanager, inode, inum / UFS_INODE_PER_BLOCK,
        (inum % UFS_INODE_PER_BLOCK) * UFS_INODE_DISK_SIZE, UFS_INODE_MEMORY_SIZE);
    if(ul_unlikely(ec)) goto do_return;

    _trans_inode(inode, inode);

do_return:
    return ec;
}
static int _write_inode(ufs_jornal_t* jornal, ufs_inode_t* inode, uint64_t inum) {
    ufs_inode_t* d = ul_reinterpret_cast(ufs_inode_t*, ufs_malloc(UFS_INODE_DISK_SIZE));
    memset(ul_reinterpret_cast(char*, d) + UFS_INODE_MEMORY_SIZE, 0, UFS_INODE_DISK_SIZE - UFS_INODE_MEMORY_SIZE);
    _trans_inode(d, inode);
    return ufs_jornal_add(jornal, d, inum / UFS_INODE_PER_BLOCK, (inum % UFS_INODE_PER_BLOCK) * UFS_INODE_DISK_SIZE, UFS_INODE_DISK_SIZE, UFS_JORNAL_ADD_MOVE);
}

// 从inode中定位块号
static int _seek_inode(ufs_minode_t* inode, uint64_t block, uint64_t* pznum) {
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
        ec = ufs_jmanager_read(&inode->ufs->jmanager, &bnum, bnum, block * 8, sizeof(bnum));
        if(ul_unlikely(ec)) return ec;
        *pznum = ul_trans_u64_le(bnum);
        return 0;
    }
    block -= UFS_ZNUM_PER_BLOCK * 2;

    if(block < UFS_ZNUM_PER_BLOCK * UFS_ZNUM_PER_BLOCK) {
        uint64_t bnum = inode->inode.zones[14];

        if(bnum == 0) { *pznum = 0; return 0; }
        ec = ufs_jmanager_read(&inode->ufs->jmanager, &bnum, bnum, (block / UFS_ZNUM_PER_BLOCK) * 8, sizeof(bnum));
        if(ul_unlikely(ec)) return ec;
        bnum = ul_trans_u64_le(bnum);

        if(bnum == 0) { *pznum = 0; return 0; }
        ec = ufs_jmanager_read(&inode->ufs->jmanager, &bnum, bnum, (block % UFS_ZNUM_PER_BLOCK) * 8, sizeof(bnum));
        if(ul_unlikely(ec)) return ec;
        bnum = ul_trans_u64_le(bnum);

        *pznum = ul_trans_u64_le(bnum);
        return 0;
    }
    block -= UFS_ZNUM_PER_BLOCK * UFS_ZNUM_PER_BLOCK;

    if(block < UFS_ZNUM_PER_BLOCK * UFS_ZNUM_PER_BLOCK * UFS_ZNUM_PER_BLOCK) {
        uint64_t bnum = inode->inode.zones[15];

        if(bnum == 0) { *pznum = 0; return 0; }
        ec = ufs_jmanager_read(&inode->ufs->jmanager, &bnum, bnum, (block / (UFS_ZNUM_PER_BLOCK * UFS_ZNUM_PER_BLOCK)) * 8, sizeof(bnum));
        if(ul_unlikely(ec)) return ec;
        block %= UFS_ZNUM_PER_BLOCK;
        bnum = ul_trans_u64_le(bnum);

        if(bnum == 0) { *pznum = 0; return 0; }
        ec = ufs_jmanager_read(&inode->ufs->jmanager, &bnum, bnum, (block / UFS_ZNUM_PER_BLOCK) * 8, sizeof(bnum));
        if(ul_unlikely(ec)) return ec;
        bnum = ul_trans_u64_le(bnum);

        if(bnum == 0) { *pznum = 0; return 0; }
        ec = ufs_jmanager_read(&inode->ufs->jmanager, &bnum, bnum, (block % UFS_ZNUM_PER_BLOCK) * 8, sizeof(bnum));
        if(ul_unlikely(ec)) return ec;
        bnum = ul_trans_u64_le(bnum);

        *pznum = ul_trans_u64_le(bnum);
        return 0;
    }

    return ERANGE;
}

static int __alloc_preaccess_inode1(ufs_minode_t* ufs_restrict inode, ufs_t* ufs_restrict ufs, int zk, uint64_t* ufs_restrict pblock) {
    int ec;
    ufs_jornal_t jornal;
    if(inode->inode.zones[zk] != 0) return 0;

    ufs_jornal_init(&jornal, &ufs->jmanager);
    ufs_zlist_lock(&ufs->zlist, &jornal);

    ec = ufs_zlist_pop(&ufs->zlist, &inode->inode.zones[zk]);
    if(ul_unlikely(ec)) goto do_return;
    ec = ufs_jornal_add_zero_block(&jornal, inode->inode.zones[zk]);
    if(ul_unlikely(ec)) goto fail_to_alloc;
    ec = ufs_zlist_sync(&ufs->zlist);
    if(ul_unlikely(ec)) goto fail_to_alloc;
    ec = ufs_jornal_nolock_commit_all(&jornal);
    if(ul_unlikely(ec)) goto fail_to_alloc;
    ++inode->inode.blocks;
    goto do_return;

fail_to_alloc:
    if(ul_unlikely(ufs_zlist_push(&ufs->zlist, inode->inode.zones[zk])))
        ufs_errabort("libufs_minode.c", "_alloc_preaccess_inode", "the zlist in memory is broken");
    inode->inode.zones[zk] = 0;

do_return:
    *pblock = inode->inode.zones[zk];
    ufs_zlist_unlock(&ufs->zlist);
    ufs_jornal_deinit(&jornal);
    return ec;
}
static int __alloc_preaccess_inode2(ufs_minode_t* ufs_restrict inode, ufs_t* ufs_restrict ufs, uint64_t k, uint64_t* ufs_restrict pblock) {
    int ec;
    ufs_jornal_t jornal;
    uint64_t old_zone;
    uint64_t oz, nz;

    ufs_jornal_init(&jornal, &ufs->jmanager);
    ufs_zlist_lock(&ufs->zlist, &jornal);
    old_zone = inode->inode.zones[14];
    if(old_zone == 0) {
        ec = ufs_zlist_pop(&ufs->zlist, &inode->inode.zones[14]);
        if(ul_unlikely(ec)) goto do_return;
        ++inode->inode.blocks;
        ec = ufs_jornal_add_zero_block(&jornal, inode->inode.zones[14]);
        if(ul_unlikely(ec)) goto fail_to_alloc0;
        oz = 0;
    } else {
        ec = ufs_jornal_read(&jornal, &oz, inode->inode.zones[14], k * UFS_ZNUM_PER_BLOCK, 8);
        if(ul_unlikely(ec)) goto fail_to_alloc0;
        oz = ul_trans_u64_le(oz);
    }

    if(oz == 0) {
        ec = ufs_zlist_pop(&ufs->zlist, &nz);
        if(ul_unlikely(ec)) goto fail_to_alloc0;
        ++inode->inode.blocks;
        nz = ul_trans_u64_le(nz);
        ec = ufs_jornal_add(&jornal, &nz, inode->inode.zones[14], k * UFS_ZNUM_PER_BLOCK, 8, UFS_JORNAL_ADD_COPY);
        nz = ul_trans_u64_le(nz);
        if(ul_unlikely(ec)) goto fail_to_alloc1;
        ec = ufs_jornal_add_zero_block(&jornal, nz);
        if(ul_unlikely(ec)) goto fail_to_alloc1;
    }
    ec = ufs_zlist_sync(&ufs->zlist);
    if(ul_unlikely(ec)) goto fail_to_alloc1;
    ec = ufs_jornal_commit_all(&jornal);
    if(ul_unlikely(ec)) goto fail_to_alloc1;
    goto do_return;

fail_to_alloc1:
    if(oz == 0) {
        if(ul_unlikely(ufs_zlist_push(&ufs->zlist, nz)))
            ufs_errabort("libufs_minode.c", "_alloc_preaccess_inode", "the zlist is broken");
        --inode->inode.blocks;
    }

fail_to_alloc0:
    if(old_zone == 0) {
        if(ul_unlikely(ufs_zlist_push(&ufs->zlist, inode->inode.zones[14])))
            ufs_errabort("libufs_minode.c", "_alloc_preaccess_inode", "the zlist is broken");
        inode->inode.zones[14] = 0;
        --inode->inode.blocks;
    }

do_return:
    if(ul_likely(ec == 0)) *pblock = nz;
    ufs_zlist_unlock(&ufs->zlist);
    ufs_jornal_deinit(&jornal);
    return ec;
}
static int __alloc_preaccess_inode3(ufs_minode_t* ufs_restrict inode, ufs_t* ufs_restrict ufs, uint64_t k, uint64_t* ufs_restrict pblock) {
    int ec;
    ufs_jornal_t jornal;
    uint64_t oz[3], nz[3];

    ufs_jornal_init(&jornal, &ufs->jmanager);
    ufs_zlist_lock(&ufs->zlist, &jornal);

    oz[0] = inode->inode.zones[15];
    if(oz[0] == 0) {
        ec = ufs_zlist_pop(&ufs->zlist, nz + 0);
        if(ul_unlikely(ec)) goto do_return;
        ++inode->inode.blocks;
        ec = ufs_jornal_add_zero_block(&jornal, nz[0]);
        if(ul_unlikely(ec)) goto fail_to_alloc0;
        oz[1] = 0;
    } else {
        nz[0] = oz[0];
        ec = ufs_jornal_read(&jornal, oz + 1, nz[0], (k / UFS_ZNUM_PER_BLOCK) * UFS_ZNUM_PER_BLOCK, 8);
        if(ul_unlikely(ec)) goto fail_to_alloc0;
        oz[1] = ul_trans_u64_le(oz[1]);
    }

    if(oz[1] == 0) {
        ec = ufs_zlist_pop(&ufs->zlist, nz + 1);
        if(ul_unlikely(ec)) goto fail_to_alloc0;
        ++inode->inode.blocks;
        nz[1] = ul_trans_u64_le(nz[1]);
        ec = ufs_jornal_add(&jornal, nz + 1, nz[0], (k / UFS_ZNUM_PER_BLOCK) * UFS_ZNUM_PER_BLOCK, 8, UFS_JORNAL_ADD_COPY);
        nz[1] = ul_trans_u64_le(nz[1]);
        if(ul_unlikely(ec)) goto fail_to_alloc1;
        ec = ufs_jornal_add_zero_block(&jornal, nz[1]);
        if(ul_unlikely(ec)) goto fail_to_alloc1;
        oz[2] = 0;
    } else {
        nz[1] = oz[1];
        ec = ufs_jornal_read(&jornal, oz + 2, nz[0], (k % UFS_ZNUM_PER_BLOCK) * UFS_ZNUM_PER_BLOCK, 8);
        if(ul_unlikely(ec)) goto fail_to_alloc1;
    }

    if(oz[2] == 0) {
        ec = ufs_zlist_pop(&ufs->zlist, nz + 2);
        if(ul_unlikely(ec)) goto fail_to_alloc1;
        ++inode->inode.blocks;
        nz[2] = ul_trans_u64_le(nz[2]);
        ec = ufs_jornal_add(&jornal, nz + 2, nz[1], (k % UFS_ZNUM_PER_BLOCK), 8, UFS_JORNAL_ADD_COPY);
        nz[2] = ul_trans_u64_le(nz[2]);
        if(ul_unlikely(ec)) goto fail_to_alloc2;
        ec = ufs_jornal_add_zero_block(&jornal, nz[2]);
        if(ul_unlikely(ec)) goto fail_to_alloc2;
    }
    ec = ufs_zlist_sync(&ufs->zlist);
    if(ul_unlikely(ec)) goto fail_to_alloc2;
    ec = ufs_jornal_commit_all(&jornal);
    if(ul_unlikely(ec)) goto fail_to_alloc2;
    goto do_return;

fail_to_alloc2:
    if(oz[2] == 0) {
        if(ul_unlikely(ufs_zlist_push(&ufs->zlist, nz[2])))
            ufs_errabort("libufs_minode.c", "_alloc_preaccess_inode", "the zlist is broken");
        --inode->inode.blocks;
    }

fail_to_alloc1:
    if(oz[1] == 0) {
        if(ul_unlikely(ufs_zlist_push(&ufs->zlist, nz[1])))
            ufs_errabort("libufs_minode.c", "_alloc_preaccess_inode", "the zlist is broken");
        --inode->inode.blocks;
    }

fail_to_alloc0:
    if(oz[0] == 0) {
        if(ul_unlikely(ufs_zlist_push(&ufs->zlist, nz[0])))
            ufs_errabort("libufs_minode.c", "_alloc_preaccess_inode", "the zlist is broken");
        --inode->inode.blocks;
    }

do_return:
    inode->inode.zones[15] = nz[0];
    if(ul_likely(ec == 0)) { *pblock = nz[2]; }
    ufs_zlist_unlock(&ufs->zlist);
    ufs_jornal_deinit(&jornal);
    return 0;
}
static int _alloc_preaccess_inode(ufs_minode_t* ufs_restrict inode, ufs_t* ufs_restrict ufs, uint64_t block, uint64_t* ufs_restrict pblock) {
    int ec = ERANGE;
    ufs_jmanager_lock(&ufs->jmanager);

    if(block < 12) { ec = 0; *pblock = 0; goto do_return; }
    block -= 12;

    if(block < UFS_ZNUM_PER_BLOCK) {
        ec = __alloc_preaccess_inode1(inode, ufs, 12, pblock);
        goto do_return;
    }
    block -= UFS_ZNUM_PER_BLOCK;

    if(block < UFS_ZNUM_PER_BLOCK) {
        ec = __alloc_preaccess_inode1(inode, ufs, 13, pblock);
        goto do_return;
    }
    block -= UFS_ZNUM_PER_BLOCK;

    if(block < UFS_ZNUM_PER_BLOCK * UFS_ZNUM_PER_BLOCK) {
        ec = __alloc_preaccess_inode2(inode, ufs, block / UFS_ZNUM_PER_BLOCK, pblock);
        goto do_return;
    }
    block -= UFS_ZNUM_PER_BLOCK * UFS_ZNUM_PER_BLOCK;

    if(block < UFS_ZNUM_PER_BLOCK * UFS_ZNUM_PER_BLOCK * UFS_ZNUM_PER_BLOCK) {
        ec = __alloc_preaccess_inode3(inode, ufs, block / UFS_ZNUM_PER_BLOCK, pblock);
        goto do_return;
    }
    // block -= UFS_ZNUM_PER_BLOCK * UFS_ZNUM_PER_BLOCK * UFS_ZNUM_PER_BLOCK;

do_return:
    ufs_jmanager_unlock(&ufs->jmanager);
    return ec;
}

static int _alloc_access_inode0(ufs_minode_t* ufs_restrict inode, ufs_t* ufs_restrict ufs, uint64_t block, uint64_t* ufs_restrict pznum) {
    ufs_jornal_t jornal;
    uint64_t znum;
    int ec;
    if(inode->inode.zones[block]) { *pznum = inode->inode.zones[block]; return 0; }

    ufs_jornal_init(&jornal, &ufs->jmanager);
    ufs_zlist_lock(&ufs->zlist, &jornal);

    ec = ufs_zlist_pop(&ufs->zlist, &znum);
    if(ul_unlikely(ec)) goto do_return;
    ec = ufs_zlist_sync(&ufs->zlist);
    if(ul_unlikely(ec)) goto fail_to_alloc;
    ec = ufs_jornal_commit_all(&jornal);
    if(ul_unlikely(ec)) goto fail_to_alloc;
    *pznum = znum;
    ++inode->inode.blocks;
    goto do_return;

fail_to_alloc:
    if(ul_unlikely(ufs_zlist_push(&ufs->zlist, znum)))
        ufs_errabort("libufs_minode.c", "_alloc_access_inode", "the zlist is broken");

do_return:
    ufs_zlist_unlock(&ufs->zlist);
    ufs_jornal_deinit(&jornal);
    return ec;
}
static int _alloc_access_inode_g(ufs_minode_t* ufs_restrict inode, ufs_t* ufs_restrict ufs, uint64_t block, uint64_t* ufs_restrict pznum) {
    uint64_t tznum, znum;
    int ec;
    ufs_jornal_t jornal;

    ec = _alloc_preaccess_inode(inode, ufs, block, &tznum);
    if(ul_unlikely(ec)) return ec;

    ec = ufs_jmanager_read(&ufs->jmanager, &znum, tznum, (block % UFS_ZNUM_PER_BLOCK) * 8, 8);
    if(ul_unlikely(ec)) return ec;
    if(znum != 0) { *pznum = ul_trans_u64_le(znum); return 0; }

    ufs_jornal_init(&jornal, &ufs->jmanager);
    ufs_zlist_lock(&ufs->zlist, &jornal);
    ec = ufs_zlist_pop(&ufs->zlist, &znum);
    if(ul_unlikely(ec)) goto do_return;
    *pznum = znum;
    znum = ul_trans_u64_le(znum);
    ec = ufs_jornal_add(&jornal, &znum, tznum, (block % UFS_ZNUM_PER_BLOCK) * 8, 8, UFS_JORNAL_ADD_COPY);
    if(ul_unlikely(ec)) goto fail_to_alloc;
    ec = ufs_zlist_sync(&ufs->zlist);
    if(ul_unlikely(ec)) goto fail_to_alloc;
    ec = ufs_jornal_commit_all(&jornal);
    if(ul_unlikely(ec)) goto fail_to_alloc;
    ++inode->inode.blocks;
    goto do_return;

fail_to_alloc:
    if(ul_unlikely(ufs_zlist_push(&ufs->zlist, ul_trans_u64_le(znum))))
        ufs_errabort("libufs_minode.c", "_alloc_access_inode", "the zlist is broken");

do_return:
    ufs_zlist_unlock(&ufs->zlist);
    ufs_jornal_deinit(&jornal);
    return ec;
}
static int _alloc_access_inode(ufs_minode_t* ufs_restrict inode, ufs_t* ufs_restrict ufs, uint64_t block, uint64_t* ufs_restrict pznum) {
    if(block < 12) return _alloc_access_inode0(inode, ufs, block, pznum);
    else return _alloc_access_inode_g(inode, ufs, block, pznum);
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
    ufs_jornal_t jornal;
    uint64_t inum;
    creat = creat ? creat : &_default_creat;
    ufs_jornal_init(&jornal, &ufs->jmanager);

    ufs_ilist_lock(&ufs->ilist, &jornal);
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
    inode->inode.mtime = inode->inode.mtime;

    inode->inode.uid = creat->uid;
    inode->inode.gid = creat->gid;

    memset(inode->inode.zones, 0, sizeof(inode->inode.zones));
    ec = _write_inode(&jornal, &inode->inode, inum);
    if(ul_unlikely(ec)) goto fail_to_alloc;

    ec = ufs_jornal_commit_all(&jornal);
    if(ul_unlikely(ec)) goto fail_to_alloc;
    inode->ufs = ufs;
    inode->inum = inum;
    goto do_return;

fail_to_alloc:
    if(ul_unlikely(ufs_ilist_push(&ufs->ilist, inum)))
        ufs_errabort("libufs_minode.c", "ufs_minode_create", "the ilist is broken");

do_return:
    ufs_ilist_unlock(&ufs->ilist);
    ufs_jornal_deinit(&jornal);
    return ec;
}

UFS_HIDDEN int ufs_minode_destroy(ufs_minode_t* inode);
UFS_HIDDEN int ufs_minode_pread(ufs_minode_t* inode, char* buf, size_t len, uint64_t off, size_t* pread);
UFS_HIDDEN int ufs_minode_pwrite(ufs_minode_t* inode, const void* buf, size_t len, int64_t off);
UFS_HIDDEN int ufs_minode_sync(ufs_minode_t* inode, int only_data) {
    ufs_jornal_t jornal;
    int ec;
    if(only_data) return ufs_fd_sync(inode->ufs->fd);
    ufs_jornal_init(&jornal, &inode->ufs->jmanager);
    _write_inode(&jornal, &inode->inode, inode->inum);
    ec = ufs_jornal_commit_all(&jornal);
    ufs_jornal_deinit(&jornal);
    return ec;
}

// 预分配块，成功则保证之后的写入绝不会因为磁盘空间不够而失败
UFS_HIDDEN int ufs_minode_fallocate(ufs_minode_t* inode, uint64_t block);
