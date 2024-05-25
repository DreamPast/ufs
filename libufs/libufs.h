#ifndef UFS_LIB_H
#define UFS_LIB_H
#define _CRT_SECURE_NO_WARNINGS

#include <stdint.h>
#include <errno.h>
#include <string.h>
#include <stdlib.h>
#include <stdio.h>

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

#define UFS_EUNKOWN      -1 // 错误：未知错误
#define UFS_EACCESS      -2 // 错误：没有对应的权限
#define UFS_EAGAIN       -3 // 错误：资源临时不可使用
#define UFS_EBADF        -4 // 错误：坏的文件解释符
#define UFS_ENOENT       -5 // 错误：不存在文件或目录
#define UFS_ENOMEM       -6 // 错误：内存不足
#define UFS_ENOTDIR      -7 // 错误：不是目录
#define UFS_EISDIR       -8 // 错误：是一个目录
#define UFS_EINVAL       -9 // 错误：无效参数
#define UFS_EMFILE       -10 // 错误：打开的文件过多
#define UFS_EFBIG        -11 // 错误：文件大小过大
#define UFS_EMLINK       -12 // 错误：链接过多
#define UFS_ELOOP        -13 // 错误：符号链接层数过多
#define UFS_ENAMETOOLONG -14 // 错误：文件名过长
#define UFS_ESTALE       -15 // 错误：过时的文件句柄
#define UFS_EFTYPE       -16 // 错误：文件格式不当
#define UFS_EILSEQ       -17 // 错误：无效或不完整的多字节或宽字符
#define UFS_EOVERFLOW    -18 // 错误：值过大
#define UFS_ENOSPC       -19 // 错误：磁盘空间不足
#define UFS_EEXIST       -20 // 错误：文件已存在

// 获得error对应的解释字符串
UFS_API const char* ufs_strerror(int error);

UFS_API int32_t ufs_getuid(void);
UFS_API int32_t ufs_getgid(void);
UFS_API int ufs_setuid(int32_t uid);
UFS_API int ufs_setgid(int32_t gid);

UFS_API int64_t ufs_time(int use_locale);
UFS_API size_t ufs_strtime(int64_t time, char* buf, const char* fmt, size_t len);
UFS_API int ufs_ptime(int64_t time, const char* fmt, FILE* fp);


/**
 * 文件描述符
*/

typedef struct ufs_fd_t {
    const char* type;

    // 关闭文件
    void (*close)(struct ufs_fd_t* fd);

    // 带偏移量的读取
    int (*pread)(struct ufs_fd_t* fd, void* buf, size_t len, int64_t off, size_t* pread);
    // 带偏移量的写入
    int (*pwrite)(struct ufs_fd_t* fd, const void* buf, size_t len, int64_t off, size_t* pwriten);
    // 同步文件
    int (*sync)(struct ufs_fd_t* fd);
} ufs_fd_t;
// 打开一个文件，当文件不存在/没有对齐到块数/无法独立占有文件时报错
UFS_API int ufs_fd_open_file(ufs_fd_t** pfd, const char* path);
UFS_API int ufs_fd_is_file(ufs_fd_t* fd);

UFS_API int ufs_fd_open_memory(ufs_fd_t** pfd, const void* src, size_t len);
UFS_API int ufs_fd_is_memory(ufs_fd_t* fd);
UFS_API int ufs_fd_lock_memory(ufs_fd_t* fd);
UFS_API int ufs_fd_unlock_memory(ufs_fd_t* fd);
UFS_API char* ufs_fd_get_memory(ufs_fd_t* fd, size_t* psize);


/**
 * 配置
*/


#define UFS_USE_UNIFORM_ERROR_CODE 1 // 使用统一的Unix style错误代码（默认使用）

#define UFS_MAGIC1 (67) // 魔数1
#define UFS_MAGIC2 (74) // 魔数2

#define UFS_NAME_MAX (64 - 8 - 1) // 目录最大名称长度
#define UFS_BLOCK_SIZE (1024) // 块的大小（必须是2的指数）
#define UFS_INODE_DEFAULT_RATIO (16*1024) // inode的默认比值（每16KB添加一个inode）

#define UFS_JORNAL_NUM (UFS_BLOCK_SIZE / 8 - 6) // 最大日志数量

#define UFS_BNUM_COMPACT (0) // 块号：兼容块（不使用此块，以便兼容BIOS/UEFI）
#define UFS_BNUM_SB (1) // 块号：超级块
#define UFS_BNUM_JORNAL (2) // 块号：日志块
#define UFS_BNUM_ILIST (UFS_BNUM_JORNAL + UFS_JORNAL_NUM + 1) // 块号：inode开始块
#define UFS_BNUM_ZLIST (UFS_BNUM_JORNAL + UFS_JORNAL_NUM + 2) // 块号：zone开始块
#define UFS_BNUM_START (UFS_BNUM_JORNAL + UFS_JORNAL_NUM + 3) // 块号：开始块

#define UFS_INODE_DISK_SIZE (256)
#define UFS_INODE_PER_BLOCK (UFS_BLOCK_SIZE / UFS_INODE_DISK_SIZE)
#define UFS_INUM_ROOT (UFS_BNUM_START * UFS_INODE_PER_BLOCK)



/**
 * 预定义
*/

#define UFS_S_IMASK   07777 // 文件权限掩码
#define UFS_S_IALL    00777 // 所有人可读/写/执行
#define UFS_S_IRALL   00444 // 所有人可读
#define UFS_S_IWALL   00222 // 所有人可写
#define UFS_S_IXALL   00111 // 所有人可执行

#define UFS_S_IRWXU   00700 // 等价(UFS_S_IRUSR | UFS_S_IWUSR | UFS_S_IXUSR)
#define UFS_S_IRUSR   00400 // 归属者可读
#define UFS_S_IWUSR   00200 // 归属者可写
#define UFS_S_IXUSR   00100 // 归属者可执行

#define UFS_S_IRWXG   00070 // 等价(UFS_S_IRGRP | UFS_S_IWGRP | UFS_S_IXGRP)
#define UFS_S_IRGRP   00040 // 归属组可读
#define UFS_S_IWGRP   00020 // 归属组可写
#define UFS_S_IXGRP   00010 // 归属组可执行

#define UFS_S_IRWXO   00007  // 等价(UFS_S_IROTH | UFS_S_IWOTH | UFS_S_IXOTH)
#define UFS_S_IROTH   00004  // 其余人可读
#define UFS_S_IWOTH   00002  // 其余人可写
#define UFS_S_IXOTH   00001  // 其余人可执行

#define UFS_S_IREAD   UFS_S_IRUSR // 归属者可读
#define UFS_S_IWRITE  UFS_S_IWUSR // 归属者可写
#define UFS_S_IEXEC   UFS_S_IXUSR // 归属者可执行

// IFCHR（字符设备）, IFSOCK（套接字）, IFBLK（块设备）, IFIFO（FIFO文件）不被支持
// TODO: 实现IFIFO
#define UFS_S_IFMT    0170000 // 文件类型掩码
#define UFS_S_IFLNK   0120000 // 符号链接
#define UFS_S_IFREG   0100000 // 常规文件
#define UFS_S_IFDIR   0040000 // 目录
// #define UFS_S_IFSOCK  0140000 // 套接字
// #define UFS_S_IFBLK   0060000 // 块设备
// #define UFS_S_IFCHR   0020000 // 字符设备
// #define UFS_S_IFIFO   0010000 // FIFO文件

#define UFS_S_ISLNK(val)  ((val & UFS_S_IFMT) == UFS_S_IFLNK)
#define UFS_S_ISREG(val)  ((val & UFS_S_IFMT) == UFS_S_IFREG)
#define UFS_S_ISDIR(val)  ((val & UFS_S_IFMT) == UFS_S_IFDIR)
// #define UFS_S_ISSOCK(val) ((val & UFS_S_IFMT) == UFS_S_IFSOCK)
// #define UFS_S_ISBLK(val)  ((val & UFS_S_IFMT) == UFS_S_IFBLK)
// #define UFS_S_ISFIFO(val) ((val & UFS_S_IFMT) == UFS_S_IFIFO)
// #define UFS_S_ISCHR(val)  ((val & UFS_S_IFMT) == UFS_S_IFCHR)

#define UFS_O_RDONLY    (1l << 0)  // 打开：只读
#define UFS_O_WRONLY    (1l << 1)  // 打开：只写
#define UFS_O_RDWR      (1l << 2)  // 打开：可读写
#define UFS_O_CREAT     (1l << 3)  // 打开：如果文件不存在，创建它
#define UFS_O_EXCL      (1l << 4)  // 打开：使用UFS_O_CREAT生效，如果文件已存在，打开失败
#define UFS_O_TRUNC     (1l << 5)  // 打开：截断文件
#define UFS_O_APPEND    (1l << 6)  // 打开：始终追加写入

#define UFS_F_OK 0
#define UFS_R_OK 4
#define UFS_W_OK 2
#define UFS_X_OK 1



struct ufs_t;
typedef struct ufs_t ufs_t;
typedef struct ufs_context_t {
    ufs_t* ufs;
    int32_t uid;
    int32_t gid;
    uint16_t umask;
} ufs_context_t;


struct ufs_dir_t;
typedef struct ufs_dir_t ufs_dir_t;

typedef struct ufs_dirent_t {
    uint64_t d_ino;
    uint64_t d_off;
    size_t d_reclen;
    uint8_t d_type;
    char d_name[UFS_NAME_MAX];
} ufs_dirent_t;

UFS_API int ufs_opendir(ufs_context_t* context, const char* path, ufs_dir_t** pdir);
UFS_API int ufs_readdir(ufs_dir_t* dir, ufs_dirent_t** pdirent);
UFS_API int ufs_seekdir(ufs_dir_t* dir, uint64_t off);
UFS_API int ufs_telldir(ufs_dir_t* dir, uint64_t *poff);
UFS_API int ufs_rewinddir(ufs_dir_t* dir);
UFS_API int ufs_closedir(ufs_dir_t* dir);

struct ufs_file_t;
typedef struct ufs_file_t ufs_file_t;
UFS_API int ufs_open(ufs_context_t* context, ufs_file_t** pfile, const char* path, unsigned long flag, uint16_t mask);
UFS_API int ufs_creat(ufs_context_t* context, ufs_file_t** pfile, const char* path, uint16_t mask);
UFS_API int ufs_close(ufs_file_t* file);


UFS_API int ufs_readlink(ufs_context_t* context, const char* path, char** presolved, size_t* psize);
UFS_API int ufs_unlink(ufs_context_t* context, const char* path);
UFS_API int ufs_link(ufs_context_t* context, const char* dest_path, const char* src_path);



#ifdef __cplusplus
}
#endif

#endif /* UFS_LIB_H */
