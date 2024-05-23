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
    ec = ufs_fd_sync(fd);
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
    ec = ufs_fd_sync(fd);
    if(ul_unlikely(ec)) return ec;
    return 0;
}

UFS_HIDDEN int ufs_do_jornal(ufs_fd_t* ufs_restrict fd, const ufs_jornal_op_t* ufs_restrict ops, int num) {
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
    ec = ufs_fd_sync(fd);
    if(ul_unlikely(ec)) return ec;

    // 3. 写入区块
    for(i = 0; i < num; ++i) {
        ec = ufs_fd_pwrite_check(fd, ops[i].buf, UFS_BLOCK_SIZE, ufs_fd_offset(ops[i].bnum));
        if(ul_unlikely(ec)) return ec;
    }
    ec = ufs_fd_sync(fd);
    if(ul_unlikely(ec)) return ec;

    // 4. 擦除标记1
    ec = _remove_flag1(fd);
    if(ul_unlikely(ec)) return ec;

    // 5. 擦除标记0
    ec = _remove_flag2(fd);
    if(ul_unlikely(ec)) return ec;

    return 0;
}

UFS_HIDDEN int ufs_fix_jornal(ufs_fd_t* ufs_restrict fd, ufs_sb_t* ufs_restrict sb) {
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

UFS_HIDDEN int ufs_jmanager_init(ufs_jmanager_t* ufs_restrict jmanager, ufs_fd_t* ufs_restrict fd) {
    jmanager->fd = fd;
    jmanager->num = 0;
    ulatomic_spinlock_init(&jmanager->lock);
    return 0;
}
UFS_HIDDEN void ufs_jmanager_deinit(ufs_jmanager_t* jmanager) {
    int i;
    for(i = jmanager->num - 1; i >= 0; --i)
        if(jmanager->flag[i] & _UFS_JORNAL_ADD_ALLOC) {
            ufs_free(ufs_const_cast(void*, jmanager->ops[i].buf));
        }
    jmanager->num = 0;
}

UFS_HIDDEN void ufs_jmanager_merge_nolock(ufs_jmanager_t* jmanager) {
    int ec, i, j;
    const int n = jmanager->num;
    for(i = n - 1; i >= 0; --i)
        for(j = i - 1; j >= 0; --j)
            if(jmanager->ops[i].bnum == jmanager->ops[j].bnum) {

                break;
            }
    for(i = 0, j = 0; i < n; ++i) {
        if(jmanager->ops[i].buf != NULL) {
            jmanager->ops[j] = jmanager->ops[i];
            jmanager->flag[j] = jmanager->flag[i];
            ++j;
        }
    }
    jmanager->num = j;
}
UFS_HIDDEN int ufs_jmanager_sync_nolock(ufs_jmanager_t* jmanager) {
    int ec, i;
    ufs_jmanager_merge_nolock(jmanager);
    ec = ufs_do_jornal(jmanager->fd, jmanager->ops, jmanager->num);
    if(ul_unlikely(ec)) return ec;
    for(i = jmanager->num - 1; i >= 0; --i)
        if(jmanager->flag[i] & _UFS_JORNAL_ADD_ALLOC) {
            ufs_free(ufs_const_cast(void*, jmanager->ops[i].buf));
        }
    jmanager->num = 0;
    return 0;
}
UFS_HIDDEN int ufs_jmanager_read_block_nolock(ufs_jmanager_t* ufs_restrict jmanager, void* ufs_restrict buf, uint64_t bnum) {
    int i;
    for(i = jmanager->num - 1; i >= 0; --i)
        if(jmanager->ops[i].bnum == bnum) {
            memcpy(buf, jmanager->ops[i].buf, UFS_BLOCK_SIZE);
            return 0;
        }
    return ufs_fd_pread_check(jmanager->fd, buf, UFS_BLOCK_SIZE, ufs_fd_offset(bnum));
}
UFS_HIDDEN int ufs_jmanager_read_nolock(ufs_jmanager_t* ufs_restrict jmanager, void* ufs_restrict buf, uint64_t bnum, size_t off, size_t len) {
    int i;
    for(i = jmanager->num - 1; i >= 0; --i)
        if(jmanager->ops[i].bnum == bnum) {
            memcpy(buf, ul_reinterpret_cast(const char*, jmanager->ops[i].buf) + off, len);
            return 0;
        }
    return ufs_fd_pread_check(jmanager->fd, buf, len, ufs_fd_offset2(bnum, off));
}
UFS_HIDDEN int ufs_jmanager_add_nolock(ufs_jmanager_t* jmanager, const void* ufs_restrict buf, uint64_t bnum, int flag) {
    if(ul_unlikely(jmanager->num == UFS_JORNAL_NUM)) {
        int ec = ufs_jmanager_sync_nolock(jmanager);
        if(ul_unlikely(ec)) {
            if(flag == UFS_JORNAL_ADD_REF) ufs_free(ufs_const_cast(void*, buf));
            return ec;
        }
    }
    switch(flag) {
    case UFS_JORNAL_ADD_REF:
        jmanager->ops[jmanager->num].buf = buf;
        break;
    case UFS_JORNAL_ADD_COPY:
        jmanager->ops[jmanager->num].buf = ufs_malloc(UFS_BLOCK_SIZE);
        if(ul_unlikely(jmanager->ops[jmanager->num].buf == NULL)) return ENOMEM;
        memcpy(ufs_const_cast(void*, jmanager->ops[jmanager->num].buf), buf, UFS_BLOCK_SIZE);
        break;
    case UFS_JORNAL_ADD_MOVE:
        jmanager->ops[jmanager->num].buf = buf;
        buf = NULL;
        break;
    default:
        return EINVAL;
    }
    jmanager->flag[jmanager->num] = flag & 3;
    jmanager->ops[jmanager->num].bnum = bnum;
    ++jmanager->num;
    return 0;
}

UFS_HIDDEN void ufs_jmanager_merge(ufs_jmanager_t* jmanager) {
    ufs_jmanager_lock(jmanager);
    ufs_jmanager_merge_nolock(jmanager);
    ufs_jmanager_unlock(jmanager);
}
UFS_HIDDEN int ufs_jmanager_read_block(ufs_jmanager_t* ufs_restrict jmanager, void* ufs_restrict buf, uint64_t bnum) {
    int ec;
    ufs_jmanager_lock(jmanager);
    ec = ufs_jmanager_read_block_nolock(jmanager, buf, bnum);
    ufs_jmanager_unlock(jmanager);
    return ec;
}
UFS_HIDDEN int ufs_jmanager_read(ufs_jmanager_t* ufs_restrict jmanager, void* ufs_restrict buf, uint64_t bnum, size_t off, size_t len) {
    int ec;
    ufs_jmanager_lock(jmanager);
    ec = ufs_jmanager_read_nolock(jmanager, buf, bnum, off, len);
    ufs_jmanager_unlock(jmanager);
    return ec;
}
UFS_HIDDEN int ufs_jmanager_sync(ufs_jmanager_t* jmanager) {
    int ec;
    ufs_jmanager_lock(jmanager);
    ec = ufs_jmanager_sync_nolock(jmanager);
    ufs_jmanager_unlock(jmanager);
    return ec;
}
UFS_HIDDEN int ufs_jmanager_add(ufs_jmanager_t* jmanager, const void* ufs_restrict buf, uint64_t bnum, int flag) {
    int ec;
    ufs_jmanager_lock(jmanager);
    ec = ufs_jmanager_add_nolock(jmanager, buf, bnum, flag);
    ufs_jmanager_unlock(jmanager);
    return ec;
}


UFS_HIDDEN int ufs_jornal_init(ufs_jornal_t* ufs_restrict jornal, ufs_jmanager_t* ufs_restrict jmanager) {
    jornal->jmanager = jmanager;
    jornal->num = 0;
    return 0;
}
UFS_HIDDEN void ufs_jornal_deinit(ufs_jornal_t* jornal) {
    ufs_jornal_settop(jornal, 0);
}
UFS_HIDDEN int ufs_jornal_add(ufs_jornal_t* ufs_restrict jornal, const void* ufs_restrict buf, uint64_t bnum, size_t off, size_t len, int flag) {
    int ec;
    char* tmp;
    tmp = ul_reinterpret_cast(char*, ufs_malloc(UFS_BLOCK_SIZE));
    if(ul_unlikely(tmp == NULL)) { ec = ENOMEM; goto do_return; }
    ec = ufs_jornal_read_block(jornal, tmp, bnum);
    if(ul_unlikely(ec)) { ufs_free(tmp); goto do_return; }
    memcpy(tmp + off, buf, len);
    ec = ufs_jornal_add_block(jornal, tmp, bnum, UFS_JORNAL_ADD_MOVE);
do_return:
    if(flag == UFS_JORNAL_ADD_REF) ufs_free(ufs_const_cast(void*, buf));
    return ec;
}
UFS_HIDDEN int ufs_jornal_add_block(ufs_jornal_t* ufs_restrict jornal, const void* ufs_restrict buf, uint64_t bnum, int flag) {
    if(ul_unlikely(jornal->num == UFS_JORNAL_NUM)) {
        if(flag == UFS_JORNAL_ADD_REF) ufs_free(ufs_const_cast(void*, buf));
        return ERANGE;
    }
    switch(flag) {
    case UFS_JORNAL_ADD_REF:
        jornal->ops[jornal->num].buf = buf;
        break;
    case UFS_JORNAL_ADD_COPY:
        jornal->ops[jornal->num].buf = ufs_malloc(UFS_BLOCK_SIZE);
        if(ul_unlikely(jornal->ops[jornal->num].buf == NULL)) return ENOMEM;
        memcpy(ufs_const_cast(void*, jornal->ops[jornal->num].buf), buf, UFS_BLOCK_SIZE);
        break;
    case UFS_JORNAL_ADD_MOVE:
        jornal->ops[jornal->num].buf = buf;
        buf = NULL;
        break;
    default:
        return EINVAL;
    }
    jornal->flag[jornal->num] = flag & 3;
    jornal->ops[jornal->num].bnum = bnum;
    ++jornal->num;
    return 0;
}
UFS_HIDDEN int ufs_jornal_add_zero_block(ufs_jornal_t* jornal, uint64_t bnum) {
    char* tmp = ul_reinterpret_cast(char*, ufs_malloc(UFS_BLOCK_SIZE));
    if(ul_unlikely(tmp)) return ENOMEM;
    memset(tmp, 0, UFS_BLOCK_SIZE);
    return ufs_jornal_add_block(jornal, tmp, bnum, UFS_JORNAL_ADD_MOVE);
}
UFS_HIDDEN int ufs_jornal_add_zero(ufs_jornal_t* jornal, uint64_t bnum, size_t off, size_t len) {
    int ec;
    char* tmp;
    tmp = ul_reinterpret_cast(char*, ufs_malloc(UFS_BLOCK_SIZE));
    if(ul_unlikely(tmp == NULL)) return ENOMEM;
    ec = ufs_jornal_read_block(jornal, tmp, bnum);
    if(ul_unlikely(ec)) { ufs_free(tmp); return ec; }
    memset(tmp + off, 0, len);
    return ufs_jornal_add_block(jornal, tmp, bnum, UFS_JORNAL_ADD_MOVE);
}
UFS_HIDDEN int ufs_jornal_read_block(ufs_jornal_t* ufs_restrict jornal, void* ufs_restrict buf, uint64_t bnum) {
    return ufs_jmanager_read_block(jornal->jmanager, buf, bnum);
}
UFS_HIDDEN int ufs_jornal_read(ufs_jornal_t* ufs_restrict jornal, void* ufs_restrict buf, uint64_t bnum, size_t off, size_t len) {
    return ufs_jmanager_read(jornal->jmanager, buf, bnum, off, len);
}
UFS_HIDDEN int ufs_jornal_nolock_commit(ufs_jornal_t* jornal, int num) {
    if(jornal->jmanager->num + num > UFS_JORNAL_NUM) {
        int ec = ufs_jmanager_sync_nolock(jornal->jmanager);
        if(ul_unlikely(ec)) return ec;
    }
    memcpy(jornal->jmanager->ops + jornal->jmanager->num, jornal->ops, ul_static_cast(size_t, jornal->num) * sizeof(jornal->ops[0]));
    memcpy(jornal->jmanager->flag + jornal->jmanager->num, jornal->flag, ul_static_cast(size_t, jornal->num) * sizeof(jornal->flag[0]));
    jornal->jmanager->num += jornal->num;
    ufs_jmanager_merge_nolock(jornal->jmanager);
    return 0;
}
UFS_HIDDEN int ufs_jornal_nolock_commit_all(ufs_jornal_t* jornal) {
    return ufs_jornal_nolock_commit(jornal, jornal->num);
}
UFS_HIDDEN int ufs_jornal_commit(ufs_jornal_t* jornal, int num) {
    int ec;
    ufs_jmanager_lock(jornal->jmanager);
    ec = ufs_jornal_nolock_commit(jornal, num);
    ufs_jmanager_unlock(jornal->jmanager);
    return ec;
}
UFS_HIDDEN int ufs_jornal_commit_all(ufs_jornal_t* jornal) {
    return ufs_jornal_commit(jornal, jornal->num);
}
UFS_HIDDEN void ufs_jornal_settop(ufs_jornal_t* jornal, int top) {
    int i;
    ufs_assert(top <= jornal->num);
    for(i = top; i < jornal->num; ++i)
        if(jornal->flag[i] & _UFS_JORNAL_ADD_ALLOC) {
            ufs_free(ufs_const_cast(void*, jornal->ops[i].buf));
        }
    jornal->num = top;
}
