#include "libufs_internel.h"

static int _istrue(uint8_t v) {
    return ufs_popcount(v) > 1;
}

typedef struct _sb_jornal_t {
    uint32_t _jd0;
    uint16_t _jd1;
    uint8_t jornal_start0;
    uint8_t jornal_start1;
    uint64_t jornal[UFS_JORNAL_NUM];
    uint8_t jornal_last1;
    uint8_t jornal_last0;
    uint16_t _jd2;
    uint32_t _jd3;
} _sb_jornal_t;
#define _sb_jornal_start(p) (&(p)->jornal_start0)
#define _sb_jornal_size (4 + 8 * UFS_JORNAL_NUM)

static int _remove_flag1(ufs_fd_t* fd) {
    int ec;
    uint8_t c = 0;
    ec = ufs_fd_pwrite_check(fd, &c, 1, ufs_fd_offset(UFS_BNUM_SB) + offsetof(ufs_sb_t, jornal_start1));
    if(ul_unlikely(ec)) return ec;
    ec = ufs_fd_pwrite_check(fd, &c, 1, ufs_fd_offset(UFS_BNUM_SB) + offsetof(ufs_sb_t, jornal_last1));
    if(ul_unlikely(ec)) return ec;
    ec = fd->sync(fd);
    if(ul_unlikely(ec)) return ec;
    return 0;
}
static int _remove_flag2(ufs_fd_t* fd) {
    int ec;
    uint8_t c = 0;
    ec = ufs_fd_pwrite_check(fd, &c, 1, ufs_fd_offset(UFS_BNUM_SB) + offsetof(ufs_sb_t, jornal_start0));
    if(ul_unlikely(ec)) return ec;
    ec = ufs_fd_pwrite_check(fd, &c, 1, ufs_fd_offset(UFS_BNUM_SB) + offsetof(ufs_sb_t, jornal_last0));
    if(ul_unlikely(ec)) return ec;
    ec = fd->sync(fd);
    if(ul_unlikely(ec)) return ec;
    return 0;
}

UFS_HIDDEN int ufs_jornal_do(ufs_fd_t* ufs_restrict fd, const ufs_jornal_op_t* ufs_restrict ops, int num) {
    int ec;
    int i;
    _sb_jornal_t disk;

    // 1. 备份区块
    disk.jornal_start0 = 0xFF;
    disk.jornal_start1 = 0xFF;
    for(i = 0; i < num; ++i) {
        disk.jornal[i] = ul_trans_u64_le(ops[i].bnum);
        ec = ufs_fd_copy(fd,
            ufs_fd_offset(ops[i].bnum), ufs_fd_offset(UFS_BNUM_JORNAL + i), UFS_BLOCK_SIZE);
        if(ul_unlikely(ec)) return ec;
    }
    for(; i < UFS_JORNAL_NUM; ++i)
        disk.jornal[i] = 0;
    disk.jornal_last0 = 0xFF;
    disk.jornal_last1 = 0xFF;

    // 2. 将四个标记和日志偏移量写入
    ec = ufs_fd_pwrite_check(fd, _sb_jornal_start(&disk), _sb_jornal_size, ufs_fd_offset(UFS_BNUM_SB) + UFS_JORNAL_OFFSET);
    if(ul_unlikely(ec)) return ec;
    ec = fd->sync(fd);
    if(ul_unlikely(ec)) return ec;

    // 3. 写入区块
    for(i = 0; i < num; ++i) {
        ec = ufs_fd_pwrite_check(fd, ops[i].buf, UFS_BLOCK_SIZE, ufs_fd_offset(ops[i].bnum));
        if(ul_unlikely(ec)) return ec;
    }
    ec = fd->sync(fd);
    if(ul_unlikely(ec)) return ec;

    // 4. 擦除标记1
    ec = _remove_flag1(fd);
    if(ul_unlikely(ec)) return ec;

    // 5. 擦除标记0
    ec = _remove_flag2(fd);
    if(ul_unlikely(ec)) return ec;

    return 0;
}

UFS_HIDDEN int ufs_jornal_fix(ufs_fd_t* ufs_restrict fd, ufs_sb_t* ufs_restrict sb) {
    int ec;
    int i;
    _sb_jornal_t disk;
    memset(&disk, 0, sizeof(disk));

    switch(
        (_istrue(sb->jornal_start0) << 3) | (_istrue(sb->jornal_start1) << 2) |
        (_istrue(sb->jornal_last1) << 1) | (_istrue(sb->jornal_last1) << 0)
    ) {
    case 0x0:
        // 初始状态/标记写入未开始
        return 0;

    case 0x8: case 0x4: case 0x2: case 0x1:
    case 0xC: case 0x3:
        // 标记写入未完成但偏移量未写入/标记0擦除未完成
    case 0xE: case 0x7: case 0x6:
        // 标记写入未完成但偏移量已写入
        disk.jornal_start0 = 0;
        disk.jornal_last0 = 0;
        disk.jornal_start1 = 0;
        disk.jornal_last1 = 0;
        return ufs_fd_pwrite_check(fd, _sb_jornal_start(&disk), _sb_jornal_size, ufs_fd_offset(UFS_BNUM_SB) + UFS_JORNAL_OFFSET);

    case 0xF:
        // 写入区块未完成/标记擦除未开始
        for(i = 0; i < UFS_JORNAL_NUM; ++i)
            if(sb->jornal[i])
                ec = ufs_fd_copy(fd, ufs_fd_offset(UFS_BNUM_JORNAL + i), ufs_fd_offset(ul_trans_u64_le(sb->jornal[i])), UFS_BLOCK_SIZE);
        disk.jornal_start0 = 0;
        disk.jornal_last0 = 0;
        disk.jornal_start1 = 0;
        disk.jornal_last1 = 0;
        return ufs_fd_pwrite_check(fd, _sb_jornal_start(&disk), _sb_jornal_size, ufs_fd_offset(UFS_BNUM_SB) + UFS_JORNAL_OFFSET);

    case 0xB: case 0xD:
        // 标记1擦除未完成
        ec = _remove_flag1(fd);
        if(ul_unlikely(ec)) return ec;
        return _remove_flag2(fd);

    case 0x9:
        // 标记0擦除未开始
        return _remove_flag2(fd);

    case 0xA: case 0x5:
        // 非法中间状态
        return UFS_ERROR_BROKEN_DISK;
    }
    return UFS_ERROR_BROKEN_DISK;
}
