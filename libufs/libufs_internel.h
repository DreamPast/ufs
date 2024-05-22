#ifndef LIBUFS_INTERNEL_H
#define LIBUFS_INTERNEL_H

#include "libufs_thread.h"
#include "ulendian.h"

static ul_inline int ufs_popcount(unsigned v) {
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
static ul_inline int ufs_clz(unsigned v) {
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

    uint64_t inode_start;
    uint64_t zone_start;
} ufs_sb_t;

typedef struct ufs_inode_t {
    uint32_t nlink; // 链接数
    uint16_t mode; // 模式
    uint16_t _d2;

    uint64_t size; // 文件大小

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

    使用1KB时，~2GiB (2^31)
    使用2KB时，~16GiB (2^34)
    使用4KB时，~128GiB (2^37)
    使用8KB时，~1025GiB (2^40)

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

UFS_HIDDEN int ufs_jornal_fix(ufs_fd_t* ufs_restrict fd, ufs_sb_t* ufs_restrict sb);
UFS_HIDDEN int ufs_jornal_do(ufs_fd_t* ufs_restrict fd, const ufs_jornal_op_t* ufs_restrict ops, int num);



#include "ulrb.h"
#define UFS_BCACHE_MAX_LEN (UFS_JORNAL_NUM * 2)
/**
 * 文件系统缓存
 *
 * 当我们尝试日志写入块时，写入不会立刻发生，而是等到达到限制时才会进行一次日志写入。
*/
typedef struct ufs_bcache_t {
    ufs_fd_t* fd;

    ufs_jornal_op_t jornal_ops[UFS_BCACHE_MAX_LEN];
    int jornal_flag[UFS_BCACHE_MAX_LEN];
    int jornal_num;

    ulatomic_spinlock_t lock;
} ufs_bcache_t;

UFS_HIDDEN int ufs_bcache_init(ufs_bcache_t* ufs_restrict bcache, ufs_fd_t* ufs_restrict fd);
UFS_HIDDEN void ufs_bcache_deinit(ufs_bcache_t* bcache);
#define _UFS_BCACHE_ADD_ALLOC   1
#define _UFS_BCACHE_ADD_COPY    2
#define _UFS_BCACHE_ADD_MERGE 4

#define UFS_BCACHE_ADD_REF 0 // 仅引用
#define UFS_BCACHE_ADD_MOVE 1 // 转移（自动使用ufs_free销毁）
#define UFS_BCACHE_ADD_COPY 3 // 拷贝

#define UFS_BCACHE_ADD_MERGE 4 // 合并
// （不可跨区块）
UFS_HIDDEN int ufs_bcache_add(ufs_bcache_t* ufs_restrict bcache, const void* ufs_restrict buf, uint64_t bnum, size_t off, size_t len, int flag);
UFS_HIDDEN int ufs_bcache_add_block(ufs_bcache_t* ufs_restrict bcache, const void* ufs_restrict buf, uint64_t bnum, int flag);
UFS_HIDDEN int ufs_bcache_sync(ufs_bcache_t* bcache);
UFS_HIDDEN int ufs_bcache_sync_part(ufs_bcache_t* bcache, int num);
UFS_HIDDEN int ufs_bcache_read_block(ufs_bcache_t* ufs_restrict bcache, void* ufs_restrict buf, uint64_t bnum);
// （不可跨区块）
UFS_HIDDEN int ufs_bcache_read(ufs_bcache_t* ufs_restrict bcache, void* ufs_restrict buf, uint64_t bnum, size_t off, size_t len);
UFS_HIDDEN int ufs_bcache_copy(ufs_bcache_t* ufs_restrict dest, const ufs_bcache_t* ufs_restrict src);
UFS_HIDDEN void ufs_bcache_compress(ufs_bcache_t* bcache);
UFS_HIDDEN void ufs_bcache_settop(ufs_bcache_t* bcache, int top);

ul_hapi void ufs_bcache_lock(ufs_bcache_t* bcache) { ulatomic_spinlock_lock(&bcache->lock); }
ul_hapi void ufs_bcache_unlock(ufs_bcache_t* bcache) { ulatomic_spinlock_unlock(&bcache->lock); }



#define UFS_ZLIST_ENTRY_NUM_MAX (UFS_BLOCK_SIZE / 8 - 1)
#define UFS_ZLIST_CACHE_LIST_LIMIT (8)
/**
 * 成组链接法的单项（zone部分）
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
    ufs_bcache_t* bcache;
    uint64_t num;
    int n;
    ulatomic_spinlock_t lock;
} ufs_zlist_t;
UFS_HIDDEN int ufs_zlist_init(ufs_zlist_t* ufs_restrict zlist, ufs_bcache_t* ufs_restrict bcache, uint64_t start);
UFS_HIDDEN void ufs_zlist_deinit(ufs_zlist_t* zlist);
UFS_HIDDEN int ufs_zlist_sync(ufs_zlist_t* zlist);
UFS_HIDDEN int ufs_zlist_pop(ufs_zlist_t* ufs_restrict zlist, uint64_t* ufs_restrict pznum);
UFS_HIDDEN int ufs_zlist_push(ufs_zlist_t* zlist, uint64_t znum);
UFS_HIDDEN uint64_t ufs_calc_zlist_available(uint64_t num);

UFS_HIDDEN int ufs_zlist_sync_nolock(ufs_zlist_t* zlist);
UFS_HIDDEN int ufs_zlist_pop_nolock(ufs_zlist_t* ufs_restrict zlist, uint64_t* ufs_restrict pznum);
UFS_HIDDEN int ufs_zlist_push_nolock(ufs_zlist_t* zlist, uint64_t znum);
ul_hapi void ufs_zlist_lock(ufs_zlist_t* zlist) { ulatomic_spinlock_lock(&zlist->lock); }
ul_hapi void ufs_zlist_unlock(ufs_zlist_t* zlist) { ulatomic_spinlock_unlock(&zlist->lock); }


#define UFS_ILIST_ENTRY_NUM_MAX (UFS_BLOCK_SIZE / UFS_INODE_PER_BLOCK / 8 - 1)
#define UFS_ILIST_CACHE_LIST_LIMIT (32)
/**
 * 成组链接法的单项（inode部分）
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
    ufs_bcache_t* bcache;
    uint64_t num;
    int n;
    ulatomic_spinlock_t lock;
} ufs_ilist_t;
UFS_HIDDEN int ufs_ilist_init(ufs_ilist_t* ufs_restrict ilist, ufs_bcache_t* ufs_restrict bcache, uint64_t start);
UFS_HIDDEN void ufs_ilist_deinit(ufs_ilist_t* ilist);
UFS_HIDDEN int ufs_ilist_sync(ufs_ilist_t* ilist);
UFS_HIDDEN int ufs_ilist_pop(ufs_ilist_t* ufs_restrict ilist, uint64_t* ufs_restrict pinum);
UFS_HIDDEN int ufs_ilist_push(ufs_ilist_t* ilist, uint64_t inum);
UFS_HIDDEN uint64_t ufs_calc_ilist_available(uint64_t num);

UFS_HIDDEN int ufs_ilist_sync_nolock(ufs_ilist_t* ilist);
UFS_HIDDEN int ufs_ilist_pop_nolock(ufs_ilist_t* ufs_restrict ilist, uint64_t* ufs_restrict pinum);
UFS_HIDDEN int ufs_ilist_push_nolock(ufs_ilist_t* ilist, uint64_t inum);
ul_hapi void ufs_ilist_lock(ufs_ilist_t* ilist) { ulatomic_spinlock_lock(&ilist->lock); }
ul_hapi void ufs_ilist_unlock(ufs_ilist_t* ilist) { ulatomic_spinlock_unlock(&ilist->lock); }



/**
 * 基于块的文件操作
 *
 * 此处的文件操作依然处于相当原始的状态，很多操作在外部是不被允许的，以避免破坏文件系统。
*/
typedef struct ufs_minode_t {
    ufs_inode_t inode;
    ufs_t* ufs;
} ufs_minode_t;

#define UFS_MINODE_CACHE_MAX 1024

UFS_HIDDEN int ufs_minode_init(ufs_t* ufs, ufs_minode_t* inode, uint64_t inum);




struct ufs_t {
    ufs_sb_t sb;
    ufs_fd_t* fd;
    ufs_bcache_t bcache;

    ufs_zlist_t zlist;
    ufs_ilist_t ilist;
};


#endif /* LIBUFS_INTERNEL_H */
