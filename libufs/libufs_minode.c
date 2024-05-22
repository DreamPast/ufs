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

UFS_HIDDEN int ufs_minode_init(ufs_t* ufs, ufs_minode_t* inode, uint64_t inum) {
    int ec;

    ec = _read_inode(ufs, &inode->inode, inum);
    if(ul_unlikely(ec)) return ec;

    inode->ufs = ufs;
    return 0;
}
UFS_HIDDEN int ufs_minode_destroy(ufs_minode_t* inode);
UFS_HIDDEN int ufs_minode_pread(ufs_minode_t* inode, void* buf, size_t len, int64_t off);
UFS_HIDDEN int ufs_minode_pwrite(ufs_minode_t* inode, const void* buf, size_t len, int64_t off);
UFS_HIDDEN int ufs_minode_sync(ufs_minode_t* inode);

// 预分配块，成功则保证之后的写入绝不会因为磁盘空间不够而失败
UFS_HIDDEN int ufs_minode_fallocate(ufs_minode_t* inode, uint64_t block);
