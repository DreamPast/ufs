#ifndef LIBUFS_INTERNEL_H
#define LIBUFS_INTERNEL_H

#include "libufs_thread.h"
#include "ulendian.h"

ul_hapi int ufs_popcount(unsigned v) {
#ifdef __has_builtin
    #if __has_builtin(__builtin_popcount)
        return __builtin_popcount(v);
    #else
        int c; for(c = 0; v; v >>= 1) c += (v & 1); return c;
    #endif
#else
    int c; for(c = 0; v; v >>= 1) c += (v & 1); return c;
#endif
}
ul_hapi int ufs_clz(unsigned v) {
#ifdef __has_builtin
    #if __has_builtin(__builtin_popcount)
        return __builtin_clz(v);
    #else
        int i; for(i = 0; !(v & (1u << (sizeof(unsigned) - 1))); v <<= 1) ++i; return i;
    #endif
#else
    int i; for(i = 0; !(v & (1u << (sizeof(unsigned) - 1))); v <<= 1) ++i; return i;
#endif
}

typedef struct ufs_sb_t {
    uint8_t magic[2]; // 魔数
    uint8_t jornal_num; // 最大日志数量

#define UFS_JORNAL_OFFSET offsetof(ufs_sb_t, jornal_start0)
    uint8_t jornal_start0; // 日志起始标记0
    uint8_t jornal_start1; // 日志起始标记1
    uint64_t jornal[UFS_JORNAL_NUM]; // 日志
    uint8_t jornal_last1; // 日志终止标记1
    uint8_t jornal_last0; // 日志终止标记0
    uint8_t block_size_log2; // 块的大小（2的指数）
    uint8_t ext_offset; // 扩展标记（仅作向后兼容，当前版本为0）
    uint32_t _jd3;

    uint64_t iblock; // inode块数
    uint64_t zblock; // zone块数
    uint64_t iblock_max; // 最大inode块数
    uint64_t zblock_max; // 最大zone块数
} ufs_sb_t;

typedef struct ufs_inode_t {
    uint32_t nlink; // 链接数
    uint16_t mode; // 模式
    uint16_t _d2;

    uint64_t size; // 文件大小
    uint64_t blocks; // 块数

    int64_t ctime; // 创建时间
    int64_t mtime; // 修改时间
    int64_t atime; // 访问时间

    int32_t uid; // 用户UID
    int32_t gid; // 组GID

    /*
    块

    0 ~ 11  0级
    12 ~ 13 1级
    14      2级
    15      3级

    支持文件大小计算为:
    (12 + 2 * (B / 8) + (B / 8) ** 2 + (B / 8) ** 3)

    使用1KB时，about 2GiB (2^31)
    使用2KB时，about 16GiB (2^34)
    使用4KB时，about 128GiB (2^37)
    使用8KB时，about 1025GiB (2^40)

    */
    uint64_t zones[16];
} ufs_inode_t;
#define UFS_INODE_DISK_SIZE (256)
#define UFS_INODE_MEMORY_SIZE (sizeof(ufs_inode_t))
#define UFS_INODE_PER_BLOCK (UFS_BLOCK_SIZE / UFS_INODE_DISK_SIZE)
#define UFS_ZNUM_PER_BLOCK (UFS_BLOCK_SIZE / 8)
UFS_HIDDEN void ufs_inode_debug(const ufs_inode_t* inode, FILE* fp);

struct ufs_t;
typedef struct ufs_t ufs_t;



/**
 * 文件描述符
*/

ul_hapi void ufs_fd_close(ufs_fd_t* fd) { fd->close(fd); }
ul_hapi int ufs_fd_pread(ufs_fd_t* fd, void* buf, size_t len, int64_t off, size_t* pread) {
    return fd->pread(fd, buf, len, off, pread);
}
ul_hapi int ufs_fd_pwrite(ufs_fd_t* fd, const void* buf, size_t len, int64_t off, size_t* pwriten) {
    return fd->pwrite(fd, buf, len, off, pwriten);
}
ul_hapi int ufs_fd_sync(ufs_fd_t* fd) { return fd->sync(fd); }

#define ufs_fd_offset(bnum) ul_static_cast(int64_t, (bnum) * UFS_BLOCK_SIZE)
#define ufs_fd_offset2(bnum, off) ul_static_cast(int64_t, (bnum) * UFS_BLOCK_SIZE + off)

UFS_HIDDEN int ufs_fd_pread_check(ufs_fd_t* ufs_restrict fd, void* ufs_restrict buf, size_t len, int64_t off);
UFS_HIDDEN int ufs_fd_pwrite_check(ufs_fd_t* ufs_restrict fd, const void* ufs_restrict buf, size_t len, int64_t off);
/* 拷贝文件描述符的两块区域（区域重叠为UB行为） */
UFS_HIDDEN int ufs_fd_copy(ufs_fd_t* fd, int64_t off_in, int64_t off_out, size_t len);
UFS_HIDDEN int ufs_fd_pwrite_zeros(ufs_fd_t* fd, size_t len, int64_t off);



/**
 * 文件系统日志
 *
 * 当我们需要写入元信息或者目录信息时，为了防止突然的断电/硬盘损坏导致的部分数据缺失，我们提供了日志写入功能。
 * 日志写入可以保证写入操作要么完全完成，要么没有发生。
 * 我们的日志写入依赖于以下假设：
 * - 磁盘的写入一定是线性的，即其始终从磁盘的一端向另一端逐字节写入（每次刷盘的顺序可以不一致）
*/
typedef struct ufs_jornal_op_t {
    uint64_t bnum; // 目标写入区块块号
    const void* buf; // 写入内容
} ufs_jornal_op_t;

UFS_HIDDEN int ufs_fix_jornal(ufs_fd_t* ufs_restrict fd, ufs_sb_t* ufs_restrict sb);
UFS_HIDDEN int ufs_do_jornal(ufs_fd_t* ufs_restrict fd, const ufs_jornal_op_t* ufs_restrict ops, int num);



typedef struct ufs_jornal_t {
    ufs_fd_t* fd;
    ufs_jornal_op_t ops[UFS_JORNAL_NUM];
    int flag[UFS_JORNAL_NUM];
    int num;
    ulatomic_spinlock_t lock;
} ufs_jornal_t;

UFS_HIDDEN int ufs_jornal_init(ufs_jornal_t* ufs_restrict jornal, ufs_fd_t* ufs_restrict fd);
UFS_HIDDEN void ufs_jornal_deinit(ufs_jornal_t* jornal);

UFS_HIDDEN void ufs_jornal_merge(ufs_jornal_t* jornal);
UFS_HIDDEN int ufs_jornal_read_block(ufs_jornal_t* ufs_restrict jornal, void* ufs_restrict buf, uint64_t bnum);
UFS_HIDDEN int ufs_jornal_read(ufs_jornal_t* ufs_restrict jornal, void* ufs_restrict buf, uint64_t bnum, size_t off, size_t len);
UFS_HIDDEN int ufs_jornal_add(ufs_jornal_t* jornal, const void* ufs_restrict buf, uint64_t bnum, int flag);
UFS_HIDDEN int ufs_jornal_sync(ufs_jornal_t* jornal);

ul_hapi void ufs_jornal_lock(ufs_jornal_t* jornal) { ulatomic_spinlock_lock(&jornal->lock); }
ul_hapi void ufs_jornal_unlock(ufs_jornal_t* jornal) { ulatomic_spinlock_unlock(&jornal->lock); }
UFS_HIDDEN void ufs_jornal_merge_nolock(ufs_jornal_t* jornal);
UFS_HIDDEN int ufs_jornal_sync_nolock(ufs_jornal_t* jornal);
UFS_HIDDEN int ufs_jornal_read_block_nolock(ufs_jornal_t* ufs_restrict jornal, void* ufs_restrict buf, uint64_t bnum);
UFS_HIDDEN int ufs_jornal_read_nolock(ufs_jornal_t* ufs_restrict jornal, void* ufs_restrict buf, uint64_t bnum, size_t off, size_t len);
UFS_HIDDEN int ufs_jornal_add_nolock(ufs_jornal_t* jornal, const void* ufs_restrict buf, uint64_t bnum, int flag);



typedef struct ufs_transcation_t {
    ufs_jornal_t* jornal;
    ufs_jornal_op_t ops[UFS_JORNAL_NUM];
    int flag[UFS_JORNAL_NUM];
    int num;
} ufs_transcation_t;

UFS_HIDDEN int ufs_transcation_init(ufs_transcation_t* ufs_restrict transcation, ufs_jornal_t* ufs_restrict jornal);
UFS_HIDDEN void ufs_transcation_deinit(ufs_transcation_t* transcation);
#define _UFS_JORNAL_ADD_ALLOC 1
#define _UFS_JORNAL_ADD_COPY  2
#define UFS_JORNAL_ADD_REF  0 // 仅引用
#define UFS_JORNAL_ADD_MOVE 1 // 转移（自动使用ufs_free销毁）
#define UFS_JORNAL_ADD_COPY 3 // 拷贝
UFS_HIDDEN int ufs_transcation_add(ufs_transcation_t* ufs_restrict transcation, const void* ufs_restrict buf, uint64_t bnum, size_t off, size_t len, int flag);
UFS_HIDDEN int ufs_transcation_add_block(ufs_transcation_t* ufs_restrict transcation, const void* ufs_restrict buf, uint64_t bnum, int flag);
UFS_HIDDEN int ufs_transcation_add_zero_block(ufs_transcation_t* transcation, uint64_t bnum);
UFS_HIDDEN int ufs_transcation_add_zero(ufs_transcation_t* transcation, uint64_t bnum, size_t off, size_t len);
UFS_HIDDEN int ufs_transcation_read_block(ufs_transcation_t* ufs_restrict transcation, void* ufs_restrict buf, uint64_t bnum);
UFS_HIDDEN int ufs_transcation_read(ufs_transcation_t* ufs_restrict transcation, void* ufs_restrict buf, uint64_t bnum, size_t off, size_t len);
UFS_HIDDEN int ufs_transcation_commit(ufs_transcation_t* transcation, int num);
UFS_HIDDEN int ufs_transcation_commit_all(ufs_transcation_t* transcation);

UFS_HIDDEN int ufs_transcation_nolock_add(ufs_transcation_t* ufs_restrict transcation, const void* ufs_restrict buf, uint64_t bnum, size_t off, size_t len, int flag);
UFS_HIDDEN int ufs_transcation_nolock_add_block(ufs_transcation_t* ufs_restrict transcation, const void* ufs_restrict buf, uint64_t bnum, int flag);
UFS_HIDDEN int ufs_transcation_nolock_add_zero_block(ufs_transcation_t* transcation, uint64_t bnum);
UFS_HIDDEN int ufs_transcation_nolock_add_zero(ufs_transcation_t* transcation, uint64_t bnum, size_t off, size_t len);
UFS_HIDDEN int ufs_transcation_nolock_read_block(ufs_transcation_t* ufs_restrict transcation, void* ufs_restrict buf, uint64_t bnum);
UFS_HIDDEN int ufs_transcation_nolock_read(ufs_transcation_t* ufs_restrict transcation, void* ufs_restrict buf, uint64_t bnum, size_t off, size_t len);
UFS_HIDDEN int ufs_transcation_nolock_commit(ufs_transcation_t* transcation, int num);
UFS_HIDDEN int ufs_transcation_nolock_commit_all(ufs_transcation_t* transcation);

UFS_HIDDEN void ufs_transcation_settop(ufs_transcation_t* transcation, int top);



#define UFS_ZLIST_ENTRY_NUM_MAX (UFS_BLOCK_SIZE / 8 - 1)
#define UFS_ZLIST_CACHE_LIST_LIMIT (8)
typedef struct _ufs_zlist_item_t {
    uint64_t next; // 下一个链接的块号（0表示没有）
    uint64_t stack[UFS_ZLIST_ENTRY_NUM_MAX];

    // 以下内容保存在内存中
    uint64_t znum;
    int num; // 剩余块的数量
} _ufs_zlist_item_t;
typedef struct _ufs_zlist_t {
    _ufs_zlist_item_t item[UFS_ZLIST_CACHE_LIST_LIMIT];
    uint64_t block;
    int top, stop;
} _ufs_zlist_t;
typedef struct ufs_zlist_t {
    _ufs_zlist_t now, backup;
    uint64_t bnum;
    ufs_transcation_t* transcation;
    ulatomic_flag_t lock;
} ufs_zlist_t;
extern int s[sizeof(ufs_zlist_t)];

UFS_HIDDEN int ufs_zlist_init(ufs_zlist_t* zlist, uint64_t start, uint64_t block);
UFS_HIDDEN int ufs_zlist_create_empty(ufs_zlist_t* zlist, uint64_t start);
UFS_HIDDEN void ufs_zlist_deinit(ufs_zlist_t* zlist);
ul_hapi void ufs_zlist_backup(ufs_zlist_t* zlist) { zlist->backup = zlist->now; }
ul_hapi void ufs_zlist_rollback(ufs_zlist_t* zlist) { zlist->now = zlist->backup; }
ul_hapi void ufs_zlist_lock(ufs_zlist_t* ufs_restrict zlist, ufs_transcation_t* ufs_restrict transcation) {
    ulatomic_spinlock_lock(&zlist->lock);
    zlist->transcation = transcation;
    ufs_zlist_backup(zlist);
}
ul_hapi void ufs_zlist_unlock(ufs_zlist_t* zlist) {
    zlist->transcation = NULL;
    ulatomic_spinlock_unlock(&zlist->lock);
}
UFS_HIDDEN int ufs_zlist_sync(ufs_zlist_t* zlist);
UFS_HIDDEN int ufs_zlist_pop(ufs_zlist_t* ufs_restrict zlist, uint64_t* ufs_restrict pznum);
UFS_HIDDEN int ufs_zlist_push(ufs_zlist_t* zlist, uint64_t znum);
UFS_HIDDEN void ufs_zlist_debug(const ufs_zlist_t* zlist, FILE* fp);



#define UFS_ILIST_ENTRY_NUM_MAX (UFS_BLOCK_SIZE / UFS_INODE_PER_BLOCK / 8 - 1)
#define UFS_ILIST_CACHE_LIST_LIMIT (32)
typedef struct _ufs_ilist_item_t {
    uint64_t next; // 下一个链接的块号（0表示没有）
    uint64_t stack[UFS_ILIST_ENTRY_NUM_MAX];

    // 以下内容保存在内存中
    uint64_t inum;
    int num; // 剩余块的数量
} _ufs_ilist_item_t;
typedef struct _ufs_ilist_t {
    _ufs_ilist_item_t item[UFS_ILIST_CACHE_LIST_LIMIT];
    uint64_t block;
    int top, stop;
} _ufs_ilist_t;
typedef struct ufs_ilist_t {
    _ufs_ilist_t now, backup;
    uint64_t bnum;
    ufs_transcation_t* transcation;
    ulatomic_flag_t lock;
} ufs_ilist_t;
UFS_HIDDEN int ufs_ilist_init(ufs_ilist_t* ilist, uint64_t start, uint64_t block);
UFS_HIDDEN int ufs_ilist_create_empty(ufs_ilist_t* ilist, uint64_t start);
UFS_HIDDEN void ufs_ilist_deinit(ufs_ilist_t* ilist);
ul_hapi void ufs_ilist_backup(ufs_ilist_t* ilist) { ilist->backup = ilist->now; }
ul_hapi void ufs_ilist_rollback(ufs_ilist_t* ilist) { ilist->now = ilist->backup; }
ul_hapi void ufs_ilist_lock(ufs_ilist_t* ufs_restrict ilist, ufs_transcation_t* ufs_restrict transcation) {
    ulatomic_spinlock_lock(&ilist->lock);
    ilist->transcation = transcation;
    ufs_ilist_backup(ilist);
}
ul_hapi void ufs_ilist_unlock(ufs_ilist_t* ilist) {
    ilist->transcation = NULL;
    ulatomic_spinlock_unlock(&ilist->lock);
}
UFS_HIDDEN int ufs_ilist_sync(ufs_ilist_t* ilist);
UFS_HIDDEN int ufs_ilist_pop(ufs_ilist_t* ufs_restrict ilist, uint64_t* ufs_restrict pinum);
UFS_HIDDEN int ufs_ilist_push(ufs_ilist_t* ilist, uint64_t inum);
UFS_HIDDEN void ufs_ilist_debug(const ufs_ilist_t* ilist, FILE* fp);



/**
 * 基于块的文件操作
 *
 * 此处的文件操作依然处于相当原始的状态，很多操作在外部是不被允许的，以避免破坏文件系统。
*/
typedef struct ufs_minode_t {
    ufs_inode_t inode;
    ufs_t* ufs;
    uint64_t inum;
} ufs_minode_t;

#define UFS_MINODE_CACHE_MAX 1024

UFS_HIDDEN int ufs_minode_init(ufs_t* ufs_restrict ufs, ufs_minode_t* ufs_restrict inode, uint64_t inum);
UFS_HIDDEN int ufs_minode_deinit(ufs_minode_t* inode);
typedef struct ufs_inode_create_t {
    int32_t uid;
    int32_t gid;
    uint16_t mode;
} ufs_inode_create_t;
UFS_HIDDEN int ufs_minode_create(ufs_t* ufs_restrict ufs, ufs_minode_t* ufs_restrict inode, const ufs_inode_create_t* ufs_restrict creat);
UFS_HIDDEN int ufs_minode_pread(
    ufs_minode_t* ufs_restrict inode, ufs_transcation_t* ufs_restrict transcation,
    char* ufs_restrict buf, size_t len, uint64_t off, size_t* ufs_restrict pread
);
UFS_HIDDEN int ufs_minode_pwrite(
    ufs_minode_t* ufs_restrict inode, ufs_transcation_t* ufs_restrict transcation,
    const char* ufs_restrict buf, size_t len, uint64_t off, size_t* ufs_restrict pwriten
);
UFS_HIDDEN int ufs_minode_sync_meta(ufs_minode_t* inode);
UFS_HIDDEN int ufs_minode_sync(ufs_minode_t* inode, int only_data);
UFS_HIDDEN int ufs_minode_fallocate(ufs_minode_t* inode, uint64_t block, uint64_t* pblock);
UFS_HIDDEN int ufs_minode_shrink(ufs_minode_t* inode, uint64_t block);
UFS_HIDDEN int ufs_minode_resize(ufs_minode_t* inode, uint64_t size);
UFS_HIDDEN void ufs_minode_debug(const ufs_minode_t* inode, FILE* fp);



struct ufs_t {
    ufs_sb_t sb;
    ufs_fd_t* fd;
    ufs_jornal_t jornal;

    ufs_zlist_t zlist;
    ufs_ilist_t ilist;
};


#endif /* LIBUFS_INTERNEL_H */
