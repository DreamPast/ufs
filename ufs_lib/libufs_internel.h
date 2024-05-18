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

struct ufs_t;
typedef struct ufs_t ufs_t;

/**
 * 文件描述符
*/

#define ufs_fd_offset(bnum) ul_static_cast(int64_t, (bnum) * UFS_BLOCK_SIZE)

UFS_HIDDEN int ufs_fd_pread_check(ufs_fd_t* fd, void* buf, size_t len, int64_t off);
UFS_HIDDEN int ufs_fd_pwrite_check(ufs_fd_t* fd, const void* buf, size_t len, int64_t off);
/* 拷贝文件描述符的两块区域（区域重叠为UB行为） */
UFS_HIDDEN int ufs_fd_copy(ufs_fd_t* fd, int64_t off_in, int64_t off_out, size_t len);

/**
 * 文件系统日志操作
 * 
 * 单次日志写入可以包括多个块。
*/
typedef struct ufs_jornal_op_t {
    uint64_t bnum; // 目标写入区块块号
    const void* buf; // 写入内容
} ufs_jornal_op_t;

/**
 * 文件系统日志
 * 
 * 当我们需要写入元信息或者目录信息时，为了防止突然的断电/硬盘损坏导致的部分数据缺失，我们提供了日志写入功能。
 * 日志写入可以保证写入操作要么完全完成，要么没有发生。
 * 我们的日志写入依赖于以下假设：
 * - 磁盘的写入一定是线性的，即其始终从磁盘的一端向另一端逐字节写入（每次刷盘的顺序可以不一致）
*/
typedef struct ufs_jornal_t {
    ulmtx_t mtx;
} ufs_jornal_t;

// 初始化日志
UFS_HIDDEN int ufs_jornal_init(ufs_jornal_t* jornal);
// 析构日志
UFS_HIDDEN void ufs_jornal_deinit(ufs_jornal_t* jornal);
// 修正日志（用于刚打开的磁盘）（线程安全）
UFS_HIDDEN int ufs_jornal_fix(ufs_jornal_t* jornal, ufs_fd_t* fd, const ufs_sb_t* sb);
// 日志性写入（线程安全）
UFS_HIDDEN int ufs_jornal_do(
    ufs_jornal_t* jornal, ufs_fd_t* fd, int wait,
    ufs_jornal_op_t* ops, int n);


/**
 * 文件系统日志缓存
 * 
 * 当我们尝试日志写入块时，写入不会立刻发生，而是等到达到日志区的限制时才会进行一次日志写入。
*/
typedef struct ufs_bcache_t {
    ufs_jornal_t* jornal;
    ufs_fd_t* fd;
    ufs_jornal_op_t ops[UFS_JORNAL_NUM];
    int flag[UFS_JORNAL_NUM];
    int n;
    ulatomic_spinlock_t lock;
} ufs_bcache_t;

UFS_HIDDEN int ufs_bcache_init(ufs_bcache_t* bcache, ufs_jornal_t* jornal, ufs_fd_t* fd);
UFS_HIDDEN void ufs_bcache_deinit(ufs_bcache_t* bcache);
#define UFS_BCACHE_ADD_REF 0 // 仅引用
#define UFS_BCACHE_ADD_COPY 1 // 拷贝
#define UFS_BCACHE_ADD_MOVE 2 // 转移（自动使用ufs_free销毁）
UFS_HIDDEN int ufs_bcache_add(ufs_bcache_t* bcache, const void* buf, uint64_t bnum, int flag);
UFS_HIDDEN int ufs_bcache_sync(ufs_bcache_t* bcache);



struct ufs_blist_t;
/**
 * 成组链接法
 * 
 * 磁盘将会以数组链表的形式被组织起来，以避免位视图法对于大存储设备的内存占用过大。
 * TODO：此处的性能较为低下，也许可以结合位视图法。
*/
typedef struct ufs_blist_t ufs_blist_t;
#define UFS_BLIST_ENTRY_NUM_MAX (UFS_BLOCK_SIZE / 8 - 1)

UFS_HIDDEN int ufs_blist_create(ufs_blist_t** pblist, ufs_bcache_t* bcache, uint64_t start);
UFS_HIDDEN void ufs_blist_destroy(ufs_blist_t* blist);
UFS_HIDDEN int ufs_blist_sync(ufs_blist_t* blist);
UFS_HIDDEN int ufs_blist_pop(ufs_blist_t* blist, uint64_t* pbnum);
UFS_HIDDEN int ufs_blist_push(ufs_blist_t* blist, uint64_t bnum);



struct ufs_t {
    ufs_sb_t sb;
    ufs_fd_t* fd;
    ufs_jornal_t jornal;
};


#endif /* LIBUFS_INTERNEL_H */
