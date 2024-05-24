#include "libufs_internel.h"

static int _istrue(uint8_t v) {
    return ufs_popcount(v) > 1;
}

typedef struct _sb_transcation_t {
    uint32_t _jd0;
    uint16_t _jd1;
    uint8_t jornal_start0;
    uint8_t jornal_start1;
    uint64_t jornal[UFS_JORNAL_NUM];
    uint8_t jornal_last1;
    uint8_t jornal_last0;
    uint16_t _jd2;
    uint32_t _jd3;
} _sb_transcation_t;
#define _sb_jornal_start(p) (&(p)->jornal_start0)
#define _sb_transcation_size (4 + 8 * UFS_JORNAL_NUM)

static int _remove_flag1(ufs_fd_t* fd) {
    int ec;
    uint8_t c = 0;
    ec = ufs_fd_pwrite_check(fd, &c, 1, ufs_fd_offset(UFS_BNUM_SB) + offsetof(struct ufs_sb_t, jornal_start1));
    if(ul_unlikely(ec)) return ec;
    ec = ufs_fd_pwrite_check(fd, &c, 1, ufs_fd_offset(UFS_BNUM_SB) + offsetof(struct ufs_sb_t, jornal_last1));
    if(ul_unlikely(ec)) return ec;
    ec = ufs_fd_sync(fd);
    if(ul_unlikely(ec)) return ec;
    return 0;
}
static int _remove_flag2(ufs_fd_t* fd) {
    int ec;
    uint8_t c = 0;
    ec = ufs_fd_pwrite_check(fd, &c, 1, ufs_fd_offset(UFS_BNUM_SB) + offsetof(struct ufs_sb_t, jornal_start0));
    if(ul_unlikely(ec)) return ec;
    ec = ufs_fd_pwrite_check(fd, &c, 1, ufs_fd_offset(UFS_BNUM_SB) + offsetof(struct ufs_sb_t, jornal_last0));
    if(ul_unlikely(ec)) return ec;
    ec = ufs_fd_sync(fd);
    if(ul_unlikely(ec)) return ec;
    return 0;
}

UFS_HIDDEN int ufs_do_jornal(ufs_fd_t* ufs_restrict fd, const ufs_jornal_op_t* ufs_restrict ops, int num) {
    int ec;
    int i;
    _sb_transcation_t disk;

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
    ec = ufs_fd_pwrite_check(fd, _sb_jornal_start(&disk), _sb_transcation_size, ufs_fd_offset(UFS_BNUM_SB) + UFS_JORNAL_OFFSET);
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
    _sb_transcation_t disk;
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
        return ufs_fd_pwrite_check(fd, _sb_jornal_start(&disk), _sb_transcation_size, ufs_fd_offset(UFS_BNUM_SB) + UFS_JORNAL_OFFSET);

    case 0xF:
        // 写入区块未完成/标记擦除未开始
        for(i = 0; i < UFS_JORNAL_NUM; ++i)
            if(sb->jornal[i])
                ec = ufs_fd_copy(fd, ufs_fd_offset(UFS_BNUM_JORNAL + i), ufs_fd_offset(ul_trans_u64_le(sb->jornal[i])), UFS_BLOCK_SIZE);
        disk.jornal_start0 = 0;
        disk.jornal_last0 = 0;
        disk.jornal_start1 = 0;
        disk.jornal_last1 = 0;
        return ufs_fd_pwrite_check(fd, _sb_jornal_start(&disk), _sb_transcation_size, ufs_fd_offset(UFS_BNUM_SB) + UFS_JORNAL_OFFSET);

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

UFS_HIDDEN int ufs_jornal_init(ufs_jornal_t* ufs_restrict jornal, ufs_fd_t* ufs_restrict fd) {
    jornal->fd = fd;
    jornal->num = 0;
    ulatomic_spinlock_init(&jornal->lock);
    return 0;
}
UFS_HIDDEN void ufs_jornal_deinit(ufs_jornal_t* jornal) {
    int i;
    for(i = jornal->num - 1; i >= 0; --i)
        if(jornal->flag[i] & _UFS_JORNAL_ADD_ALLOC) {
            ufs_free(ufs_const_cast(void*, jornal->ops[i].buf));
        }
    jornal->num = 0;
}

UFS_HIDDEN void ufs_jornal_merge_nolock(ufs_jornal_t* jornal) {
    int i, j;
    const int n = jornal->num;
    for(i = n - 1; i >= 0; --i)
        for(j = i - 1; j >= 0; --j)
            if(jornal->ops[i].bnum == jornal->ops[j].bnum) {
                if(jornal->flag[j] & _UFS_JORNAL_ADD_ALLOC) ufs_free(ufs_const_cast(void*, jornal->ops[j].buf));
                jornal->ops[j].buf = jornal->ops[i].buf;
                jornal->flag[j] = jornal->flag[i];
                jornal->ops[i].buf = NULL;
                break;
            }
    for(i = 0, j = 0; i < n; ++i) {
        if(jornal->ops[i].buf != NULL) {
            jornal->ops[j] = jornal->ops[i];
            jornal->flag[j] = jornal->flag[i];
            ++j;
        }
    }
    jornal->num = j;
}
UFS_HIDDEN int ufs_jornal_sync_nolock(ufs_jornal_t* jornal) {
    int ec, i;
    ufs_jornal_merge_nolock(jornal);
    ec = ufs_do_jornal(jornal->fd, jornal->ops, jornal->num);
    if(ul_unlikely(ec)) return ec;
    for(i = jornal->num - 1; i >= 0; --i) 
        if(jornal->flag[i] & _UFS_JORNAL_ADD_ALLOC) {
            ufs_free(ufs_const_cast(void*, jornal->ops[i].buf));
        }
    jornal->num = 0;
    return 0;
}
UFS_HIDDEN int ufs_jornal_read_block_nolock(ufs_jornal_t* ufs_restrict jornal, void* ufs_restrict buf, uint64_t bnum) {
    int i;
    for(i = jornal->num - 1; i >= 0; --i)
        if(jornal->ops[i].bnum == bnum) {
            memcpy(buf, jornal->ops[i].buf, UFS_BLOCK_SIZE);
            return 0;
        }
    return ufs_fd_pread_check(jornal->fd, buf, UFS_BLOCK_SIZE, ufs_fd_offset(bnum));
}
UFS_HIDDEN int ufs_jornal_read_nolock(ufs_jornal_t* ufs_restrict jornal, void* ufs_restrict buf, uint64_t bnum, size_t off, size_t len) {
    int i;
    for(i = jornal->num - 1; i >= 0; --i)
        if(jornal->ops[i].bnum == bnum) {
            memcpy(buf, ul_reinterpret_cast(const char*, jornal->ops[i].buf) + off, len);
            return 0;
        }
    return ufs_fd_pread_check(jornal->fd, buf, len, ufs_fd_offset2(bnum, off));
}
UFS_HIDDEN int ufs_jornal_add_nolock(ufs_jornal_t* jornal, const void* ufs_restrict buf, uint64_t bnum, int flag) {
    if(ul_unlikely(jornal->num == UFS_JORNAL_NUM)) {
        int ec = ufs_jornal_sync_nolock(jornal);
        if(ul_unlikely(ec)) {
            if(flag == UFS_JORNAL_ADD_MOVE) ufs_free(ufs_const_cast(void*, buf));
            return ec;
        }
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
        break;
    default:
        return EINVAL;
    }
    jornal->flag[jornal->num] = flag;
    jornal->ops[jornal->num].bnum = bnum;
    ++jornal->num;
    return 0;
}

UFS_HIDDEN void ufs_jornal_merge(ufs_jornal_t* jornal) {
    ufs_jornal_lock(jornal);
    ufs_jornal_merge_nolock(jornal);
    ufs_jornal_unlock(jornal);
}
UFS_HIDDEN int ufs_jornal_read_block(ufs_jornal_t* ufs_restrict jornal, void* ufs_restrict buf, uint64_t bnum) {
    int ec;
    ufs_jornal_lock(jornal);
    ec = ufs_jornal_read_block_nolock(jornal, buf, bnum);
    ufs_jornal_unlock(jornal);
    return ec;
}
UFS_HIDDEN int ufs_jornal_read(ufs_jornal_t* ufs_restrict jornal, void* ufs_restrict buf, uint64_t bnum, size_t off, size_t len) {
    int ec;
    ufs_jornal_lock(jornal);
    ec = ufs_jornal_read_nolock(jornal, buf, bnum, off, len);
    ufs_jornal_unlock(jornal);
    return ec;
}
UFS_HIDDEN int ufs_jornal_sync(ufs_jornal_t* jornal) {
    int ec;
    ufs_jornal_lock(jornal);
    ec = ufs_jornal_sync_nolock(jornal);
    ufs_jornal_unlock(jornal);
    return ec;
}
UFS_HIDDEN int ufs_jornal_add(ufs_jornal_t* jornal, const void* ufs_restrict buf, uint64_t bnum, int flag) {
    int ec;
    ufs_jornal_lock(jornal);
    ec = ufs_jornal_add_nolock(jornal, buf, bnum, flag);
    ufs_jornal_unlock(jornal);
    return ec;
}


UFS_HIDDEN int ufs_transcation_init(ufs_transcation_t* ufs_restrict transcation, ufs_jornal_t* ufs_restrict jornal) {
    transcation->jornal = jornal;
    transcation->num = 0;
    return 0;
}
UFS_HIDDEN void ufs_transcation_deinit(ufs_transcation_t* transcation) {
    ufs_transcation_settop(transcation, 0);
}

UFS_HIDDEN int ufs_transcation_nolock_add(ufs_transcation_t* ufs_restrict transcation, const void* ufs_restrict buf, uint64_t bnum, size_t off, size_t len, int flag) {
    int ec;
    char* tmp;
    tmp = ul_reinterpret_cast(char*, ufs_malloc(UFS_BLOCK_SIZE));
    if(ul_unlikely(tmp == NULL)) { ec = ENOMEM; goto do_return; }
    ec = ufs_transcation_nolock_read_block(transcation, tmp, bnum);
    if(ul_unlikely(ec)) { ufs_free(tmp); goto do_return; }
    memcpy(tmp + off, buf, len);
    ec = ufs_transcation_nolock_add_block(transcation, tmp, bnum, UFS_JORNAL_ADD_MOVE);
do_return:
    if(flag == UFS_JORNAL_ADD_REF) ufs_free(ufs_const_cast(void*, buf));
    return ec;
}
UFS_HIDDEN int ufs_transcation_nolock_add_block(ufs_transcation_t* ufs_restrict transcation, const void* ufs_restrict buf, uint64_t bnum, int flag) {
    if(ul_unlikely(transcation->num == UFS_JORNAL_NUM)) {
        if(flag == UFS_JORNAL_ADD_MOVE) ufs_free(ufs_const_cast(void*, buf));
        return ERANGE;
    }
    switch(flag) {
    case UFS_JORNAL_ADD_REF:
        transcation->ops[transcation->num].buf = buf;
        break;
    case UFS_JORNAL_ADD_COPY:
        transcation->ops[transcation->num].buf = ufs_malloc(UFS_BLOCK_SIZE);
        if(ul_unlikely(transcation->ops[transcation->num].buf == NULL)) return ENOMEM;
        memcpy(ufs_const_cast(void*, transcation->ops[transcation->num].buf), buf, UFS_BLOCK_SIZE);
        break;
    case UFS_JORNAL_ADD_MOVE:
        transcation->ops[transcation->num].buf = buf;
        break;
    default:
        return EINVAL;
    }
    transcation->flag[transcation->num] = flag;
    transcation->ops[transcation->num].bnum = bnum;
    ++transcation->num;
    return 0;
}
UFS_HIDDEN int ufs_transcation_nolock_add_zero_block(ufs_transcation_t* transcation, uint64_t bnum) {
    char* tmp = ul_reinterpret_cast(char*, ufs_malloc(UFS_BLOCK_SIZE));
    if(ul_unlikely(tmp == NULL)) return ENOMEM;
    memset(tmp, 0, UFS_BLOCK_SIZE);
    return ufs_transcation_nolock_add_block(transcation, tmp, bnum, UFS_JORNAL_ADD_MOVE);
}
UFS_HIDDEN int ufs_transcation_nolock_add_zero(ufs_transcation_t* transcation, uint64_t bnum, size_t off, size_t len) {
    int ec;
    char* tmp;
    tmp = ul_reinterpret_cast(char*, ufs_malloc(UFS_BLOCK_SIZE));
    if(ul_unlikely(tmp == NULL)) return ENOMEM;
    ec = ufs_transcation_nolock_read_block(transcation, tmp, bnum);
    if(ul_unlikely(ec)) { ufs_free(tmp); return ec; }
    memset(tmp + off, 0, len);
    return ufs_transcation_nolock_add_block(transcation, tmp, bnum, UFS_JORNAL_ADD_MOVE);
}
UFS_HIDDEN int ufs_transcation_nolock_read_block(ufs_transcation_t* ufs_restrict transcation, void* ufs_restrict buf, uint64_t bnum) {
    return ufs_jornal_read_block_nolock(transcation->jornal, buf, bnum);
}
UFS_HIDDEN int ufs_transcation_nolock_read(ufs_transcation_t* ufs_restrict transcation, void* ufs_restrict buf, uint64_t bnum, size_t off, size_t len) {
    return ufs_jornal_read_nolock(transcation->jornal, buf, bnum, off, len);
}
UFS_HIDDEN int ufs_transcation_nolock_commit(ufs_transcation_t* transcation, int num) {
    if(transcation->jornal->num + num > UFS_JORNAL_NUM) {
        int ec = ufs_jornal_sync_nolock(transcation->jornal);
        if(ul_unlikely(ec)) return ec;
    }
    memcpy(transcation->jornal->ops + transcation->jornal->num, transcation->ops, ul_static_cast(size_t, transcation->num) * sizeof(transcation->ops[0]));
    memcpy(transcation->jornal->flag + transcation->jornal->num, transcation->flag, ul_static_cast(size_t, transcation->num) * sizeof(transcation->flag[0]));
    transcation->jornal->num += transcation->num;
    ufs_jornal_merge_nolock(transcation->jornal);
    transcation->num = 0;
    return 0;
}
UFS_HIDDEN int ufs_transcation_nolock_commit_all(ufs_transcation_t* transcation) {
    return ufs_transcation_nolock_commit(transcation, transcation->num);
}

UFS_HIDDEN int ufs_transcation_add(ufs_transcation_t* ufs_restrict transcation, const void* ufs_restrict buf, uint64_t bnum, size_t off, size_t len, int flag) {
    int ec;
    char* tmp;
    tmp = ul_reinterpret_cast(char*, ufs_malloc(UFS_BLOCK_SIZE));
    if(ul_unlikely(tmp == NULL)) { ec = ENOMEM; goto do_return; }
    ec = ufs_transcation_read_block(transcation, tmp, bnum);
    if(ul_unlikely(ec)) { ufs_free(tmp); goto do_return; }
    memcpy(tmp + off, buf, len);
    ec = ufs_transcation_add_block(transcation, tmp, bnum, UFS_JORNAL_ADD_MOVE);
do_return:
    if(flag == UFS_JORNAL_ADD_REF) ufs_free(ufs_const_cast(void*, buf));
    return ec;
}
UFS_HIDDEN int ufs_transcation_add_block(ufs_transcation_t* ufs_restrict transcation, const void* ufs_restrict buf, uint64_t bnum, int flag) {
    return ufs_transcation_nolock_add_block(transcation, buf, bnum, flag);
}
UFS_HIDDEN int ufs_transcation_add_zero_block(ufs_transcation_t* transcation, uint64_t bnum) {
    return ufs_transcation_nolock_add_zero_block(transcation, bnum);
}
UFS_HIDDEN int ufs_transcation_add_zero(ufs_transcation_t* transcation, uint64_t bnum, size_t off, size_t len) {
    int ec;
    char* tmp;
    tmp = ul_reinterpret_cast(char*, ufs_malloc(UFS_BLOCK_SIZE));
    if(ul_unlikely(tmp == NULL)) return ENOMEM;
    ec = ufs_transcation_read_block(transcation, tmp, bnum);
    if(ul_unlikely(ec)) { ufs_free(tmp); return ec; }
    memset(tmp + off, 0, len);
    return ufs_transcation_add_block(transcation, tmp, bnum, UFS_JORNAL_ADD_MOVE);
}
UFS_HIDDEN int ufs_transcation_read_block(ufs_transcation_t* ufs_restrict transcation, void* ufs_restrict buf, uint64_t bnum) {
    return ufs_jornal_read_block(transcation->jornal, buf, bnum);
}
UFS_HIDDEN int ufs_transcation_read(ufs_transcation_t* ufs_restrict transcation, void* ufs_restrict buf, uint64_t bnum, size_t off, size_t len) {
    return ufs_jornal_read(transcation->jornal, buf, bnum, off, len);
}
UFS_HIDDEN int ufs_transcation_commit(ufs_transcation_t* transcation, int num) {
    int ec;
    ufs_jornal_lock(transcation->jornal);
    ec = ufs_transcation_nolock_commit(transcation, num);
    ufs_jornal_unlock(transcation->jornal);
    return ec;
}
UFS_HIDDEN int ufs_transcation_commit_all(ufs_transcation_t* transcation) {
    return ufs_transcation_commit(transcation, transcation->num);
}
UFS_HIDDEN void ufs_transcation_settop(ufs_transcation_t* transcation, int top) {
    int i;
    ufs_assert(top <= transcation->num);
    for(i = top; i < transcation->num; ++i)
        if(transcation->flag[i] & _UFS_JORNAL_ADD_ALLOC) {
            ufs_free(ufs_const_cast(void*, transcation->ops[i].buf));
        }
    transcation->num = top;
}
