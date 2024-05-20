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

UFS_HIDDEN int ufs_jornal_fix(ufs_fd_t* fd, ufs_sb_t* sb);
UFS_HIDDEN int ufs_jornal_do(ufs_fd_t* fd, const ufs_jornal_op_t* ops, int num);


#include "ulrb.h"
#define UFS_BCACHE_NOJORNAL_LIMIT 1024
/**
 * 文件系统缓存
 * 
 * 当我们尝试写入块时，写入不会立刻发生，而是等到达到限制时才会进行写入。
*/
typedef struct ufs_bcache_t {
    ufs_fd_t* fd;
    ulmtx_t lock;

    ulrb_node_t* nojornal_root;
    size_t nojornal_num;

    ufs_jornal_op_t jornal_ops[UFS_JORNAL_NUM];
    int jornal_flag[UFS_JORNAL_NUM];
    int jornal_num;
} ufs_bcache_t;

UFS_HIDDEN int ufs_bcache_init(ufs_bcache_t* bcache, ufs_fd_t* fd);
UFS_HIDDEN void ufs_bcache_deinit(ufs_bcache_t* bcache);
#define UFS_BCACHE_ADD_REF 0 // 仅引用
#define UFS_BCACHE_ADD_COPY 1 // 拷贝
#define UFS_BCACHE_ADD_MOVE 2 // 转移（自动使用ufs_free销毁）

#define UFS_BCACHE_ADD_JORNAL 0x10 // 使用日志写入
UFS_HIDDEN int ufs_bcache_add(ufs_bcache_t* bcache, const void* buf, uint64_t bnum, int flag);
UFS_HIDDEN int ufs_bcache_sync(ufs_bcache_t* bcache);
UFS_HIDDEN int ufs_bcache_read(ufs_bcache_t* bcache, void* buf, uint64_t bnum);


#define UFS_BLIST_ENTRY_NUM_MAX (UFS_BLOCK_SIZE / 8 - 1)
#define UFS_BLIST_CACHE_LIST_LIMIT (8)
typedef struct _blist_item_t {
    uint64_t next; // 下一个链接的块号（0表示没有）
    uint64_t stack[UFS_BLIST_ENTRY_NUM_MAX];

    // 以下内容保存在内存中

    uint64_t bnum;
    int num; // 剩余块的数量
} _blist_item_t;

/**
 * 成组链接法
 * 
 * 磁盘将会以数组链表的形式被组织起来，以避免位视图法对于大存储设备的内存占用过大。
*/
typedef struct ufs_blist_t {
    _blist_item_t item[UFS_BLIST_CACHE_LIST_LIMIT];
    uint64_t top_bnum;
    ufs_bcache_t* bcache;
    int n;
    ulatomic_spinlock_t lock;
} ufs_blist_t;

UFS_HIDDEN int ufs_blist_init(ufs_blist_t* blist, ufs_bcache_t* bcache, uint64_t start);
UFS_HIDDEN void ufs_blist_deinit(ufs_blist_t* blist);
UFS_HIDDEN int ufs_blist_sync(ufs_blist_t* blist);
UFS_HIDDEN int ufs_blist_pop(ufs_blist_t* blist, uint64_t* pbnum);
UFS_HIDDEN int ufs_blist_push(ufs_blist_t* blist, uint64_t bnum);



struct ufs_t {
    ufs_sb_t sb;
    ufs_fd_t* fd;
};


#endif /* LIBUFS_INTERNEL_H */
