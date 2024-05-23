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

    uint64_t inode_blocks; // inode块数
    uint64_t zone_blocks; // zone块数
    uint64_t inode_max_blocks; // 最大inode块数
    uint64_t zone_max_blocks; // 最大zone块数
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

typedef struct ufs_jmanager_t {
    ufs_fd_t* fd;
    ufs_jornal_op_t ops[UFS_JORNAL_NUM];
    int flag[UFS_JORNAL_NUM];
    int num;
    ulatomic_spinlock_t lock;
} ufs_jmanager_t;

UFS_HIDDEN int ufs_jmanager_init(ufs_jmanager_t* ufs_restrict jmanager, ufs_fd_t* ufs_restrict fd);
UFS_HIDDEN void ufs_jmanager_deinit(ufs_jmanager_t* jmanager);

UFS_HIDDEN void ufs_jmanager_merge(ufs_jmanager_t* jmanager);
UFS_HIDDEN int ufs_jmanager_read_block(ufs_jmanager_t* ufs_restrict jmanager, void* ufs_restrict buf, uint64_t bnum);
UFS_HIDDEN int ufs_jmanager_read(ufs_jmanager_t* ufs_restrict jmanager, void* ufs_restrict buf, uint64_t bnum, size_t off, size_t len);
UFS_HIDDEN int ufs_jmanager_add(ufs_jmanager_t* jmanager, const void* ufs_restrict buf, uint64_t bnum, int flag);
UFS_HIDDEN int ufs_jmanager_sync(ufs_jmanager_t* jmanager);

ul_hapi void ufs_jmanager_lock(ufs_jmanager_t* jmanager) { ulatomic_spinlock_lock(&jmanager->lock); }
ul_hapi void ufs_jmanager_unlock(ufs_jmanager_t* jmanager) { ulatomic_spinlock_unlock(&jmanager->lock); }
UFS_HIDDEN void ufs_jmanager_merge_nolock(ufs_jmanager_t* jmanager);
UFS_HIDDEN int ufs_jmanager_sync_nolock(ufs_jmanager_t* jmanager);
UFS_HIDDEN int ufs_jmanager_read_block_nolock(ufs_jmanager_t* ufs_restrict jmanager, void* ufs_restrict buf, uint64_t bnum);
UFS_HIDDEN int ufs_jmanager_read_nolock(ufs_jmanager_t* ufs_restrict jmanager, void* ufs_restrict buf, uint64_t bnum, size_t off, size_t len);
UFS_HIDDEN int ufs_jmanager_add_nolock(ufs_jmanager_t* jmanager, const void* ufs_restrict buf, uint64_t bnum, int flag);

typedef struct ufs_jornal_t {
    ufs_jmanager_t* jmanager;
    ufs_jornal_op_t ops[UFS_JORNAL_NUM];
    int flag[UFS_JORNAL_NUM];
    int num;
} ufs_jornal_t;

UFS_HIDDEN int ufs_jornal_init(ufs_jornal_t* ufs_restrict jornal, ufs_jmanager_t* ufs_restrict jmanager);
UFS_HIDDEN void ufs_jornal_deinit(ufs_jornal_t* jornal);
#define _UFS_JORNAL_ADD_ALLOC 1
#define _UFS_JORNAL_ADD_COPY  2
#define UFS_JORNAL_ADD_REF  0 // 仅引用
#define UFS_JORNAL_ADD_MOVE 1 // 转移（自动使用ufs_free销毁）
#define UFS_JORNAL_ADD_COPY 3 // 拷贝
UFS_HIDDEN int ufs_jornal_add(ufs_jornal_t* ufs_restrict jornal, const void* ufs_restrict buf, uint64_t bnum, size_t off, size_t len, int flag);
UFS_HIDDEN int ufs_jornal_add_block(ufs_jornal_t* ufs_restrict jornal, const void* ufs_restrict buf, uint64_t bnum, int flag);
UFS_HIDDEN int ufs_jornal_add_zero_block(ufs_jornal_t* jornal, uint64_t bnum);
UFS_HIDDEN int ufs_jornal_add_zero(ufs_jornal_t* jornal, uint64_t bnum, size_t off, size_t len);
UFS_HIDDEN int ufs_jornal_read_block(ufs_jornal_t* ufs_restrict jornal, void* ufs_restrict buf, uint64_t bnum);
UFS_HIDDEN int ufs_jornal_read(ufs_jornal_t* ufs_restrict jornal, void* ufs_restrict buf, uint64_t bnum, size_t off, size_t len);
UFS_HIDDEN int ufs_jornal_nolock_commit(ufs_jornal_t* jornal, int num) ;
UFS_HIDDEN int ufs_jornal_nolock_commit_all(ufs_jornal_t* jornal);
UFS_HIDDEN int ufs_jornal_commit(ufs_jornal_t* jornal, int num);
UFS_HIDDEN int ufs_jornal_commit_all(ufs_jornal_t* jornal);
UFS_HIDDEN void ufs_jornal_settop(ufs_jornal_t* jornal, int top);



#define UFS_ZLIST_ENTRY_NUM_MAX (UFS_BLOCK_SIZE / 8 - 1)
#define UFS_ZLIST_CACHE_LIST_LIMIT (8)
/**
 * 成组链接法的单项（zone部分）（非线程安全）
 */
typedef struct _zlist_item_t {
    uint64_t next; // 下一个链接的块号（0表示没有）
    uint64_t stack[UFS_ZLIST_ENTRY_NUM_MAX];

    // 以下内容保存在内存中
    uint64_t znum;
    int num; // 剩余块的数量
} _zlist_item_t;

/**
 * 成组链接法（zone部分）
*/
typedef struct ufs_zlist_t {
    _zlist_item_t item[UFS_ZLIST_CACHE_LIST_LIMIT];
    uint64_t top_znum;
    ufs_jornal_t* jornal;
    uint64_t num;
    int n;
    ulatomic_spinlock_t lock;
} ufs_zlist_t;
UFS_HIDDEN int ufs_zlist_init(ufs_zlist_t* ilist, uint64_t start);
UFS_HIDDEN void ufs_zlist_deinit(ufs_zlist_t* zlist);
UFS_HIDDEN uint64_t ufs_zlist_available(const ufs_zlist_t* ufs_restrict zlist);

ul_hapi void ufs_zlist_lock(ufs_zlist_t* ufs_restrict zlist, ufs_jornal_t* ufs_restrict jornal) {
    ulatomic_spinlock_lock(&zlist->lock); zlist->jornal = jornal;
}
ul_hapi void ufs_zlist_unlock(ufs_zlist_t* zlist) {
    zlist->jornal = NULL; ulatomic_spinlock_unlock(&zlist->lock);
}
UFS_HIDDEN int ufs_zlist_sync(ufs_zlist_t* zlist);
UFS_HIDDEN int ufs_zlist_pop(ufs_zlist_t* ufs_restrict zlist, uint64_t* ufs_restrict pznum);
UFS_HIDDEN int ufs_zlist_push(ufs_zlist_t* zlist, uint64_t znum);


#define UFS_ILIST_ENTRY_NUM_MAX (UFS_BLOCK_SIZE / UFS_INODE_PER_BLOCK / 8 - 1)
#define UFS_ILIST_CACHE_LIST_LIMIT (32)
/**
 * 成组链接法的单项（inode部分）（非线程安全）
 *
 * 由于一个块可以存放多个inode，因此我们单独实现了inode和zone的成组链接法。
 */
typedef struct _ilist_item_t {
    uint64_t next; // 下一个链接的块号（0表示没有）
    uint64_t stack[UFS_ILIST_ENTRY_NUM_MAX];

    // 以下内容保存在内存中
    uint64_t inum;
    int num; // 剩余块的数量
} _ilist_item_t;

/**
 * 成组链接法（inode部分）
*/
typedef struct ufs_ilist_t {
    _ilist_item_t item[UFS_ILIST_CACHE_LIST_LIMIT];
    uint64_t top_inum;
    ufs_jornal_t* jornal;
    uint64_t num;
    int n;
    ulatomic_spinlock_t lock;
} ufs_ilist_t;
UFS_HIDDEN int ufs_ilist_init(ufs_ilist_t* ilist, uint64_t start);
UFS_HIDDEN void ufs_ilist_deinit(ufs_ilist_t* ilist);
UFS_HIDDEN uint64_t ufs_ilist_available(const ufs_zlist_t* ufs_restrict ilist);

ul_hapi void ufs_ilist_lock(ufs_ilist_t* ufs_restrict ilist, ufs_jornal_t* ufs_restrict jornal) {
    ulatomic_spinlock_lock(&ilist->lock); ilist->jornal = jornal;
}
ul_hapi void ufs_ilist_unlock(ufs_ilist_t* ilist) {
    ilist->jornal = NULL; ulatomic_spinlock_unlock(&ilist->lock);
}
UFS_HIDDEN int ufs_ilist_sync(ufs_ilist_t* ilist);
UFS_HIDDEN int ufs_ilist_pop(ufs_ilist_t* ufs_restrict ilist, uint64_t* ufs_restrict pinum);
UFS_HIDDEN int ufs_ilist_push(ufs_ilist_t* ilist, uint64_t inum);



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

UFS_HIDDEN int ufs_minode_init(ufs_t* ufs, ufs_minode_t* inode, uint64_t inum);

typedef struct ufs_inode_create_t {
    int32_t uid;
    int32_t gid;
    uint16_t mode;
} ufs_inode_create_t;



struct ufs_t {
    ufs_sb_t sb;
    ufs_fd_t* fd;
    ufs_jmanager_t jmanager;

    ufs_zlist_t zlist;
    ufs_ilist_t ilist;
};


#endif /* LIBUFS_INTERNEL_H */
