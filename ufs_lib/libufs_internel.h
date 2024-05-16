#ifndef LIBUFS_INTERNEL_H
#define LIBUFS_INTERNEL_H

#include "libufs.h"
#include "ulmtx.h"
#include "ulendian.h"
#include "ulatomic.h"

#ifdef __clang__
    #define UFS_HIDDEN __attribute__((__visibility__("hidden")))
#elif defined(__GNUC__) && __GNUC__ >= 4
    #define UFS_HIDDEN __attribute__((__visibility__("hidden")))
#else
    #define UFS_HIDDEN
#endif
#ifdef _WIN32
    #undef UFS_HIDDEN
    #define UFS_HIDDEN
#endif

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


/**********
 * 日志区 *
 **********/


typedef struct ufs_jornal_op_t {
    uint64_t bnum;
    const void* buf;
} ufs_jornal_op_t;

typedef struct ufs_jornal_t {
    ulmtx_t mtx;
} ufs_jornal_t;

// 初始化日志
UFS_HIDDEN int ufs_init_jornal(ufs_jornal_t* jornal);
// 析构日志
UFS_HIDDEN void ufs_deinit_jornal(ufs_jornal_t* jornal);
// 修正日志（用于刚打开的磁盘）（线程安全）
UFS_HIDDEN int ufs_fix_jornal(ufs_jornal_t* jornal, ufs_fd_t* fd, const ufs_sb_t* sb);
// 日志性写入（线程安全）
UFS_HIDDEN int ufs_do_jornal(
    ufs_jornal_t* jornal, ufs_fd_t* fd, const ufs_sb_t* sb, int wait,
    ufs_jornal_op_t* ops, int n);

// 获得日志块号
UFS_HIDDEN int ufs_search_jornal_bnum(ufs_t* ufs, uint64_t* pbnum);


/**
 * 块缓存
*/


typedef struct _ufs_bcache_item_t {
    char* buf;
    uint64_t bnum;
    int flag;
} _ufs_bcache_item_t;

typedef struct ufs_bcache_t {
    _ufs_bcache_item_t item[UFS_JORNAL_NUM];
    int n;
    ulatomic_spinlock_t lck;
    ufs_t* ufs;
} ufs_bcache_t;

UFS_HIDDEN int ufs_init_bcache(ufs_bcache_t* bcache, ufs_t* ufs);
UFS_HIDDEN void ufs_deinit_bcache(ufs_bcache_t* bcache);
#define UFS_ADD_BCACHE_REF 0 // 仅引用
#define UFS_ADD_BCACHE_COPY 1 // 拷贝
#define UFS_ADD_BCACHE_MOVE 2 // 转移（自动使用ufs_free销毁）
UFS_HIDDEN int ufs_add_bcache(ufs_bcache_t* bcache, void* buf, uint64_t bnum, int flag);
UFS_HIDDEN int ufs_sync_bcache(ufs_bcache_t* bcache);


/**
 * 成组链接法
*/


#define UFS_BLIST_ENTRY_NUM_MAX (UFS_BLOCK_SIZE / 8 - 2)
struct ufs_blist_t;
typedef struct ufs_blist_t ufs_blist_t;

UFS_HIDDEN int ufs_create_blist(ufs_blist_t** pblist, ufs_fd_t* fd, uint64_t start);
UFS_HIDDEN int ufs_destroy_blist(ufs_blist_t* blist);
UFS_HIDDEN int ufs_sync_blist(ufs_blist_t* blist, ufs_fd_t* fd);
UFS_HIDDEN int ufs_pop_blist(ufs_blist_t* blist, uint64_t* pbum);
UFS_HIDDEN int ufs_push_blist(ufs_blist_t* blist, uint64_t bnum);



struct ufs_t {
    ufs_sb_t sb;
    ufs_fd_t* fd;
    ufs_jornal_t jornal;
};


#endif /* LIBUFS_INTERNEL_H */
