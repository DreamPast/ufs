#ifndef UFS_LIB_H
#define UFS_LIB_H
#define _CRT_SECURE_NO_WARNINGS

#include <stdint.h>
#include <errno.h>
#include <string.h>
#include <stdlib.h>

#ifdef UFS_BUILD_DLL
    #ifdef _WIN32
        #define UFS_API __declspec(dllexport)
    #endif
#endif
#ifndef UFS_API
    #define UFS_API
#endif

#define ufs_realloc(ptr, sz) realloc((ptr), (sz))
#define ufs_free(ptr) free(ptr)
#define ufs_malloc(sz) realloc(NULL, (sz))

#ifdef __cplusplus
extern "C" {
#endif

typedef struct ufs_fd_t {
    const char* type;

    // 关闭文件
    void (*close)(ufs_fd_t* fd);

    // 带偏移量的读取
    int (*pread)(ufs_fd_t* fd, void* buf, size_t len, int64_t off, size_t* pread);
    // 带偏移量的写入
    int (*pwrite)(ufs_fd_t* fd, const void* buf, size_t len, int64_t off, size_t* pwriten);
    // 同步文件
    int (*sync)(ufs_fd_t* fd);
} ufs_fd_t;
UFS_API int ufs_fd_open_file(ufs_fd_t** pfd, const char* path);
UFS_API int ufs_fd_is_file(ufs_fd_t* fd);



#define UFS_MAGIC1 (67)
#define UFS_MAGIC2 (74)

#define UFS_NAME_MAX (64-1)
#define UFS_BLOCK_SIZE (4096)
#define UFS_INODE_DEFAULT_RATIO (16*4096)

#define UFS_BNUM_COMPACT (0)
#define UFS_BNUM_SB (1)
#define UFS_BNUM_JORNAL (2)
#define UFS_BNUM_FREESTACK (3)
#define UFS_BNUM_START (UFS_BNUM_FREESTACK + 1)

#define UFS_JORNAL_NUM 16
#define UFS_BLIST_STACK_MAX 10

typedef struct ufs_sb_t {
    uint8_t magic[2]; // 魔数
    uint8_t ext_offset;

#define UFS_JORNAL_OFFSET offsetof(ufs_sb_t, jornal_start0)
    uint8_t jornal_start0;
    uint8_t jornal_start1;
    struct {
        uint64_t backup_bnum;
        uint64_t target_bnum;
    } jornal[UFS_JORNAL_NUM];
    uint8_t jornal_last1;
    uint8_t jornal_last0;
    uint8_t block_size_log2;
    uint8_t _jd2;
    uint32_t _jd3;

    uint64_t inode_blocks; // inode块数
    uint64_t zone_blocks; // zone块数

    uint64_t ilist[UFS_BLIST_STACK_MAX];
    uint64_t zlist[UFS_BLIST_STACK_MAX];
} ufs_sb_t;
int s[sizeof(ufs_sb_t)];

typedef struct myfs_inode_t {
    uint32_t nlink; // 链接数
    uint16_t mode; // 模式
    uint16_t _d2;

    uint64_t size; // 文件大小
    uint64_t blocks; // 文件块数

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

    uint64_t _d[9];
} myfs_inode_t;
#define MYFS_INODE_SIZE_LOG2 8
#define MYFS_INODE_SIZE 256
#define MYFS_INODE_PER_BLOCK (1024 / sizeof(myfs_inode_t))

struct ufs_t;
typedef struct ufs_t ufs_t;




#ifdef __cplusplus
}
#endif

#endif /* UFS_LIB_H */
