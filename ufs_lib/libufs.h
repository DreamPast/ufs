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


/**
 * 错误代码
 * 
 * 部分代码我们难以准确地使用Unix style的error code表示，因此我们使用负数来表示这些错误
 * 如果你确实需要统一使用Unix Style的error code，可以使用ufs_uniform_error函数。
*/

// 错误：未知错误
#define UFS_ERROR_UNKOWN -1 
// 错误：无法读入足够的字节（这通常是因为遇到了EOF，但是我们的磁盘文件是对齐BLOCK_SIZE的，这个不应当发生）
#define UFS_ERROR_READ_NOT_ENOUGH -2
// 错误：磁盘已损坏（这通常是因为磁盘文件被外部程序进行了不正确的修改）
#define UFS_ERROR_BROKEN_DISK -2
// 错误：互斥锁已损坏（这几乎不会发生，但是我们依然会进行判断）
#define UFS_ERROR_BROKEN_MUTEX -3

// 获得error对应的解释字符串，对于Unix style的error code，这将直接使用系统的错误信息
UFS_API const char* ufs_strerror(int error);
UFS_API int ufs_uniform_error(int error);


/**
 * 文件描述符
*/


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


/**
 * 配置
*/


#define UFS_USE_UNIFORM_ERROR_CODE 0 // 使用统一的Unix style错误代码（默认不使用）

#define UFS_MAGIC1 (67) // 魔数1
#define UFS_MAGIC2 (74) // 魔数2

#define UFS_NAME_MAX (64-1) // 目录最大名称长度（建议为2的指数-1）
#define UFS_BLOCK_SIZE (4096) // 块的大小（必须是2的指数）
#define UFS_INODE_DEFAULT_RATIO (16*1024) // inode的默认比值（每16KB添加一个inode）

#define UFS_JORNAL_NUM 32 // 最大日志数量
#define UFS_BLIST_STACK_MAX 10 // 成组链接法最大长度

#define UFS_BNUM_COMPACT (0) // 块号：兼容块（不使用此块，以便兼容BIOS/UEFI）
#define UFS_BNUM_SB (1) // 块号：超级块
#define UFS_BNUM_JORNAL (2) // 块号：日志块（当存储空间不够时，使用此块慢慢拷写）
#define UFS_BNUM_START (UFS_BNUM_JORNAL + UFS_JORNAL_NUM + 1) // 块号：开始块


/**
 * 
*/


typedef struct ufs_sb_t {
    uint8_t magic[2]; // 魔数
    uint8_t ext_offset; // 扩展标记（仅作向后兼容，当前版本为0）

#define UFS_JORNAL_OFFSET offsetof(ufs_sb_t, jornal_start0)
    uint8_t jornal_start0; // 日志起始标记0
    uint8_t jornal_start1; // 日志起始标记1
    uint64_t jornal[UFS_JORNAL_NUM]; // 日志
    uint8_t jornal_last1; // 日志终止标记1
    uint8_t jornal_last0; // 日志终止标记0
    uint8_t block_size_log2; // 块的大小（2的指数）
    uint8_t _jd2;
    uint32_t _jd3;

    uint64_t inode_blocks; // inode块数
    uint64_t zone_blocks; // zone块数

    uint64_t inode_start;
    uint64_t zone_start;
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
