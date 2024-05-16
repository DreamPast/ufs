#ifndef LIBUFS_INTERNEL_H
#define LIBUFS_INTERNEL_H

#include "libufs.h"
#include "ulmtx.h"
#include "ulendian.h"

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



#define ufs_fd_offset(bnum) ul_static_cast(int64_t, (bnum) * UFS_BLOCK_SIZE)

/* 如果读入字节不够，填充剩余部分 */
UFS_HIDDEN int ufs_fd_pread_check(ufs_fd_t* fd, void* buf, size_t len, int64_t off);
/* 如果写入字符不够，返回ENOSPC（磁盘空间不够） */
UFS_HIDDEN int ufs_fd_pwrite_check(ufs_fd_t* fd, const void* buf, size_t len, int64_t off);
/* 拷贝文件描述符的两块区域（区域重叠为UB行为） */
UFS_HIDDEN int ufs_fd_copy(ufs_fd_t* fd, int64_t off_in, int64_t off_out, size_t len);

/**********
 * 日志区 *
 **********/


typedef struct ufs_jornal_op_t {
    uint64_t backup_bnum;
    uint64_t target_bnum;
    void* buf;
} ufs_jornal_op_t;

typedef struct ufs_jornal_t {
    ulmtx_t mtx;
} ufs_jornal_t;

UFS_HIDDEN int ufs_init_jornal(ufs_jornal_t* jornal);
UFS_HIDDEN void ufs_deinit_jornal(ufs_jornal_t* jornal);
UFS_HIDDEN int ufs_fix_jornal(ufs_jornal_t* jornal, ufs_fd_t* fd, const ufs_sb_t* sb);
UFS_HIDDEN int ufs_do_jornal(ufs_jornal_t* jornal, ufs_fd_t* fd, const ufs_sb_t* sb, ufs_jornal_op_t* ops, int n, int wait);


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
    ufs_jornal_op_t op;
};


#endif /* LIBUFS_INTERNEL_H */
