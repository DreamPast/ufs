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

static int _remove_flag1(ufs_vfs_t* vfs) {
    int ec;
    uint8_t c = 0;
    ec = ufs_vfs_pwrite_check(vfs, &c, 1, ufs_vfs_offset(UFS_BNUM_SB) + offsetof(struct ufs_sb_t, jornal_start1));
    if(ufs_unlikely(ec)) return ec;
    ec = ufs_vfs_pwrite_check(vfs, &c, 1, ufs_vfs_offset(UFS_BNUM_SB) + offsetof(struct ufs_sb_t, jornal_last1));
    if(ufs_unlikely(ec)) return ec;
    ec = ufs_vfs_sync(vfs);
    if(ufs_unlikely(ec)) return ec;
    return 0;
}
static int _remove_flag2(ufs_vfs_t* vfs) {
    int ec;
    uint8_t c = 0;
    ec = ufs_vfs_pwrite_check(vfs, &c, 1, ufs_vfs_offset(UFS_BNUM_SB) + offsetof(struct ufs_sb_t, jornal_start0));
    if(ufs_unlikely(ec)) return ec;
    ec = ufs_vfs_pwrite_check(vfs, &c, 1, ufs_vfs_offset(UFS_BNUM_SB) + offsetof(struct ufs_sb_t, jornal_last0));
    if(ufs_unlikely(ec)) return ec;
    ec = ufs_vfs_sync(vfs);
    if(ufs_unlikely(ec)) return ec;
    return 0;
}

UFS_HIDDEN int ufs_do_jornal(ufs_vfs_t* ufs_restrict vfs, const ufs_jornal_op_t* ufs_restrict ops, int num) {
    int ec;
    int i;
    _sb_transcation_t disk;

    // 1. 备份区块
    disk.jornal_start0 = 0xFF;
    disk.jornal_start1 = 0xFF;
    for(i = 0; i < num; ++i) {
        disk.jornal[i] = ul_trans_u64_le(ops[i].bnum);
        ec = ufs_vfs_copy(vfs,
            ufs_vfs_offset(ops[i].bnum), ufs_vfs_offset(UFS_BNUM_JORNAL + i), UFS_BLOCK_SIZE);
        if(ufs_unlikely(ec)) return ec;
    }
    for(; i < UFS_JORNAL_NUM; ++i)
        disk.jornal[i] = 0;
    disk.jornal_last0 = 0xFF;
    disk.jornal_last1 = 0xFF;

    // 2. 将四个标记和日志偏移量写入
    ec = ufs_vfs_pwrite_check(vfs, _sb_jornal_start(&disk), _sb_transcation_size, ufs_vfs_offset(UFS_BNUM_SB) + UFS_JORNAL_OFFSET);
    if(ufs_unlikely(ec)) return ec;
    ec = ufs_vfs_sync(vfs);
    if(ufs_unlikely(ec)) return ec;

    // 3. 写入区块
    for(i = 0; i < num; ++i) {
        ec = ufs_vfs_pwrite_check(vfs, ops[i].buf, UFS_BLOCK_SIZE, ufs_vfs_offset(ops[i].bnum));
        if(ufs_unlikely(ec)) return ec;
    }
    ec = ufs_vfs_sync(vfs);
    if(ufs_unlikely(ec)) return ec;

    // 4. 擦除标记1
    ec = _remove_flag1(vfs);
    if(ufs_unlikely(ec)) return ec;

    // 5. 擦除标记0
    ec = _remove_flag2(vfs);
    if(ufs_unlikely(ec)) return ec;

    return 0;
}

UFS_HIDDEN int ufs_fix_jornal(ufs_vfs_t* ufs_restrict vfs, ufs_sb_t* ufs_restrict sb) {
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
        return ufs_vfs_pwrite_check(vfs, _sb_jornal_start(&disk), _sb_transcation_size, ufs_vfs_offset(UFS_BNUM_SB) + UFS_JORNAL_OFFSET);

    case 0xF:
        // 写入区块未完成/标记擦除未开始
        for(i = 0; i < UFS_JORNAL_NUM; ++i)
            if(sb->jornal[i])
                ec = ufs_vfs_copy(vfs, ufs_vfs_offset(UFS_BNUM_JORNAL + i), ufs_vfs_offset(ul_trans_u64_le(sb->jornal[i])), UFS_BLOCK_SIZE);
        disk.jornal_start0 = 0;
        disk.jornal_last0 = 0;
        disk.jornal_start1 = 0;
        disk.jornal_last1 = 0;
        return ufs_vfs_pwrite_check(vfs, _sb_jornal_start(&disk), _sb_transcation_size, ufs_vfs_offset(UFS_BNUM_SB) + UFS_JORNAL_OFFSET);

    case 0xB: case 0xD:
        // 标记1擦除未完成
        ec = _remove_flag1(vfs);
        if(ufs_unlikely(ec)) return ec;
        return _remove_flag2(vfs);

    case 0x9:
        // 标记0擦除未开始
        return _remove_flag2(vfs);

    case 0xA: case 0x5:
        // 非法中间状态
        return -1;
    }
    return -1;
}

UFS_HIDDEN int ufs_jornal_init(ufs_jornal_t* ufs_restrict jornal, ufs_vfs_t* ufs_restrict vfs) {
    jornal->vfs = vfs;
    jornal->num = 0;
    ulatomic_spinlock_init(&jornal->lock);
    return 0;
}
UFS_HIDDEN void ufs_jornal_deinit(ufs_jornal_t* jornal) {
    int i;
    for(i = jornal->num - 1; i >= 0; --i)
        ufs_free(ufs_const_cast(void*, jornal->ops[i].buf));
    jornal->num = 0;
}

static void _jornal_merge(ufs_jornal_t* jornal, int x) {
    int i, j;
    const int n = jornal->num;
    for(i = n - 1; i >= x; --i)
        for(j = i - 1; j >= 0; --j)
            if(jornal->ops[i].bnum == jornal->ops[j].bnum) {
                ufs_free(ufs_const_cast(void*, jornal->ops[j].buf));
                jornal->ops[j].buf = jornal->ops[i].buf;
                jornal->ops[i].buf = NULL;
                break;
            }
    for(i = x, j = x; i < n; ++i)
        if(jornal->ops[i].buf != NULL)
            jornal->ops[j++] = jornal->ops[i];
    jornal->num = j;
}
static void ufs_jornal_merge_nolock(ufs_jornal_t* jornal) {
    _jornal_merge(jornal, 0);
}
static int ufs_jornal_sync_nolock(ufs_jornal_t* jornal) {
    int ec, i;
    ufs_jornal_merge_nolock(jornal);
    ec = ufs_do_jornal(jornal->vfs, jornal->ops, jornal->num);
    if(ufs_unlikely(ec)) return ec;
    for(i = jornal->num - 1; i >= 0; --i)
        ufs_free(ufs_const_cast(void*, jornal->ops[i].buf));
    jornal->num = 0;
    return 0;
}
static int ufs_jornal_read_block_nolock(ufs_jornal_t* ufs_restrict jornal, void* ufs_restrict buf, uint64_t bnum) {
    int i;
    for(i = jornal->num - 1; i >= 0; --i)
        if(jornal->ops[i].bnum == bnum) {
            memcpy(buf, jornal->ops[i].buf, UFS_BLOCK_SIZE);
            return 0;
        }
    return ufs_vfs_pread_check(jornal->vfs, buf, UFS_BLOCK_SIZE, ufs_vfs_offset(bnum));
}
static int ufs_jornal_add_block_nolock(ufs_jornal_t* ufs_restrict jornal, const void* ufs_restrict buf, uint64_t bnum, int flag) {
    if(ufs_unlikely(jornal->num == UFS_JORNAL_NUM)) {
        int ec = ufs_jornal_sync_nolock(jornal);
        if(ufs_unlikely(ec)) {
            if(flag == UFS_JORNAL_ADD_MOVE) ufs_free(ufs_const_cast(void*, buf));
            return ec;
        }
    }
    switch(flag) {
    case UFS_JORNAL_ADD_COPY:
        jornal->ops[jornal->num].buf = ufs_malloc(UFS_BLOCK_SIZE);
        if(ufs_unlikely(jornal->ops[jornal->num].buf == NULL)) return UFS_ENOMEM;
        memcpy(ufs_const_cast(void*, jornal->ops[jornal->num].buf), buf, UFS_BLOCK_SIZE);
        break;
    case UFS_JORNAL_ADD_MOVE:
        jornal->ops[jornal->num].buf = buf;
        break;
    default:
        return UFS_EINVAL;
    }
    jornal->ops[jornal->num].bnum = bnum;
    ++jornal->num;
    return 0;
}
static int ufs_jornal_add_nolock(ufs_jornal_t* ufs_restrict jornal, const void* ufs_restrict buf, uint64_t bnum, size_t off, size_t len, int flag) {
    int ec;
    char* tmp;
    tmp = ul_reinterpret_cast(char*, ufs_malloc(UFS_BLOCK_SIZE));
    if(ufs_unlikely(tmp == NULL)) { ec = UFS_ENOMEM; goto do_return; }
    ec = ufs_jornal_read_block_nolock(jornal, tmp, bnum);
    if(ufs_unlikely(ec)) { ufs_free(tmp); goto do_return; }
    memcpy(tmp + off, buf, len);
    ec = ufs_jornal_add_block_nolock(jornal, tmp, bnum, UFS_JORNAL_ADD_MOVE);
do_return:
    if(flag == UFS_JORNAL_ADD_MOVE) ufs_free(ufs_const_cast(void*, buf));
    return ec;
}
static int ufs_jornal_append_nolock(ufs_jornal_t* ufs_restrict jornal, ufs_jornal_op_t* ufs_restrict ops, int num) {
    if(jornal->num + num > UFS_JORNAL_NUM) {
        int ec = ufs_jornal_sync_nolock(jornal);
        if(ufs_unlikely(ec)) return ec;
    }
    memcpy(jornal->ops + jornal->num, ops, ul_static_cast(size_t, num) * sizeof(ops[0]));
    jornal->num += num;
    _jornal_merge(jornal, jornal->num - num);
    return 0;
}

static void ufs_jornal_lock(ufs_jornal_t* jornal) { ulatomic_spinlock_lock(&jornal->lock); }
static void ufs_jornal_unlock(ufs_jornal_t* jornal) { ulatomic_spinlock_unlock(&jornal->lock); }

UFS_HIDDEN void ufs_jornal_merge(ufs_jornal_t* jornal) {
    ufs_jornal_lock(jornal);
    ufs_jornal_merge_nolock(jornal);
    ufs_jornal_unlock(jornal);
}
UFS_HIDDEN int ufs_jornal_read_block(ufs_jornal_t* ufs_restrict jornal, void* ufs_restrict buf, uint64_t bnum) {
    int i;
    ulatomic_spinlock_lock(&jornal->lock);
    for(i = jornal->num - 1; i >= 0; --i)
        if(jornal->ops[i].bnum == bnum) {
            memcpy(buf, jornal->ops[i].buf, UFS_BLOCK_SIZE);
            ulatomic_spinlock_unlock(&jornal->lock);
            return 0;
        }
    ulatomic_spinlock_unlock(&jornal->lock);
    return ufs_vfs_pread_check(jornal->vfs, buf, UFS_BLOCK_SIZE, ufs_vfs_offset(bnum));
}
UFS_HIDDEN int ufs_jornal_read(ufs_jornal_t* ufs_restrict jornal, void* ufs_restrict buf, uint64_t bnum, size_t off, size_t len) {
    int i;
    ulatomic_spinlock_lock(&jornal->lock);
    for(i = jornal->num - 1; i >= 0; --i)
        if(jornal->ops[i].bnum == bnum) {
            memcpy(buf, ul_reinterpret_cast(const char*, jornal->ops[i].buf) + off, len);
            ulatomic_spinlock_unlock(&jornal->lock);
            return 0;
        }
    ulatomic_spinlock_unlock(&jornal->lock);
    return ufs_vfs_pread_check(jornal->vfs, buf, len, ufs_vfs_offset2(bnum, off));
}
UFS_HIDDEN int ufs_jornal_sync(ufs_jornal_t* jornal) {
    int ec;
    ufs_jornal_lock(jornal);
    ec = ufs_jornal_sync_nolock(jornal);
    ufs_jornal_unlock(jornal);
    return ec;
}
UFS_HIDDEN int ufs_jornal_add(ufs_jornal_t* ufs_restrict jornal, const void* ufs_restrict buf, uint64_t bnum, size_t off, size_t len, int flag) {
    int ec;
    ufs_jornal_lock(jornal);
    ec = ufs_jornal_add_nolock(jornal, buf, bnum, off, len, flag);
    ufs_jornal_unlock(jornal);
    return ec;
}
UFS_HIDDEN int ufs_jornal_add_block(ufs_jornal_t* ufs_restrict jornal, const void* ufs_restrict buf, uint64_t bnum, int flag) {
    int ec;
    ufs_jornal_lock(jornal);
    ec = ufs_jornal_add_block_nolock(jornal, buf, bnum, flag);
    ufs_jornal_unlock(jornal);
    return ec;
}
UFS_HIDDEN int ufs_jornal_append(ufs_jornal_t* ufs_restrict jornal, ufs_jornal_op_t* ufs_restrict ops, int num) {
    int ec;
    ufs_jornal_lock(jornal);
    ec = ufs_jornal_append_nolock(jornal, ops, num);
    ufs_jornal_unlock(jornal);
    return ec;
}
