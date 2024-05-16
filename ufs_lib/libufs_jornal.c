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

static int _do_jornal(ufs_fd_t* fd, const ufs_jornal_op_t* ops, int num) {
    int ec;
    int i;
    _sb_jornal_t disk;

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

    ec = ufs_fd_pwrite_check(fd, _sb_jornal_start(&disk), _sb_jornal_size, ufs_fd_offset(UFS_BNUM_SB) + UFS_JORNAL_OFFSET);
    if(ul_unlikely(ec)) return ec;
    ec = fd->sync(fd);
    if(ul_unlikely(ec)) return ec;

    for(i = 0; i < UFS_JORNAL_NUM; ++i) {
        ec = ufs_fd_pwrite_check(fd, ops[i].buf, UFS_BLOCK_SIZE, ufs_fd_offset(ops[i].bnum));
        if(ul_unlikely(ec)) return ec;
    }
    ec = fd->sync(fd);
    if(ul_unlikely(ec)) return ec;

    ec = _remove_flag1(fd);
    if(ul_unlikely(ec)) return ec;

    ec = _remove_flag2(fd);
    if(ul_unlikely(ec)) return ec;

    return 0;
}

static int _fix_jornal(ufs_fd_t* fd, const ufs_sb_t* sb) {
    int ec;
    int i;
    _sb_jornal_t disk;
    memset(&disk, 0, sizeof(disk));

    switch(
        (__ufs_jornal_istrue(sb->jornal_start0) << 3) | (__ufs_jornal_istrue(sb->jornal_start1) << 2) |
        (__ufs_jornal_istrue(sb->jornal_last1) << 1) | (__ufs_jornal_istrue(sb->jornal_last1) << 0)
    ) {
    case 0x0:
        // 正常状态
        return 0;

    case 0x8: case 0x4: case 0x2: case 0x1:
    case 0xC: case 0x3:

    case 0xE: case 0x7: case 0x6:
        disk.jornal_start0 = 0;
        disk.jornal_last0 = 0;
        disk.jornal_start1 = 0;
        disk.jornal_last1 = 0;
        return ufs_fd_pwrite_check(fd, _sb_jornal_start(&disk), _sb_jornal_size, ufs_fd_offset(UFS_BNUM_SB) + UFS_JORNAL_OFFSET);

    case 0xF:
        for(i = 0; i < UFS_JORNAL_NUM; ++i)
            if(sb->jornal[i])
                ec = ufs_fd_copy(fd, ufs_fd_offset(UFS_BNUM_JORNAL + i), ufs_fd_offset(ul_trans_u64_le(sb->jornal[i])), UFS_BLOCK_SIZE);
        disk.jornal_start0 = 0;
        disk.jornal_last0 = 0;
        disk.jornal_start1 = 0;
        disk.jornal_last1 = 0;
        return ufs_fd_pwrite_check(fd, _sb_jornal_start(&disk), _sb_jornal_size, ufs_fd_offset(UFS_BNUM_SB) + UFS_JORNAL_OFFSET);

    case 0xB: case 0xD:
        ec = __ufs_jornal_remove_flag1(fd);
        if(ul_unlikely(ec)) return ec;
        return __ufs_jornal_remove_flag2(fd);

    case 0x9:
        return __ufs_jornal_remove_flag2(fd);

    case 0xA: case 0x5:
        return UFS_ERROR_BROKEN_DISK;
    }
}


// TODO: 更完善的互斥锁错误信息

UFS_HIDDEN int ufs_init_jornal(ufs_jornal_t* jornal) {
    return ulmtx_init(&jornal->mtx) ? EINVAL : 0;
}
UFS_HIDDEN void ufs_deinit_jornal(ufs_jornal_t* jornal) {
    ulmtx_destroy(&jornal->mtx);
}
UFS_HIDDEN int ufs_fix_jornal(ufs_jornal_t* jornal, ufs_fd_t* fd, ufs_sb_t* sb) {
    int ec;
    if(ulmtx_lock(&jornal->mtx)) return UFS_ERROR_BROKEN_MUTEX;
    ec = _fix_jornal(fd, sb);
    if(ul_likely(!ec)) memset(ul_reinterpret_cast(char*, sb) + UFS_JORNAL_OFFSET, 0, _sb_jornal_size);
    ulmtx_unlock(&jornal->mtx);
    return ec;
}
UFS_HIDDEN int ufs_do_jornal(
    ufs_jornal_t* jornal, ufs_fd_t* fd, const ufs_sb_t* sb, int wait,
    ufs_jornal_op_t* ops, int n
) {
    int ec;
    if(wait)
        if(ulmtx_lock(&jornal->mtx)) return UFS_ERROR_BROKEN_MUTEX;
    else {
        ec = ulmtx_trylock(&jornal->mtx);
        if(ec == 1) return EBUSY;
        if(ec) return UFS_ERROR_BROKEN_MUTEX;
    }

    ec = _do_jornal(fd, ops, n);
    ulmtx_unlock(&jornal->mtx);
    return ec;
}
