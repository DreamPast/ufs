#ifndef UFS_LIB_H
#define UFS_LIB_H
#define _CRT_SECURE_NO_WARNINGS

#include <stdint.h>
#include <errno.h>
#include <string.h>
#include <stdlib.h>
#include <stdio.h>

#ifdef LIBUFS_BUILD_DLL
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
#define UFS_ENOTEMPTY    -21 // 错误：目录非空

// 获得error对应的解释字符串
UFS_API const char* ufs_strerror(int error);
// 将自定义的error转化为Unix Style的error
UFS_API int ufs_uniform_error(int error);

UFS_API int32_t ufs_getuid(void);
UFS_API int32_t ufs_getgid(void);

UFS_API int64_t ufs_time(int use_locale);
UFS_API size_t ufs_strtime(int64_t time, char* buf, const char* fmt, size_t len);
UFS_API int ufs_ptime(int64_t time, const char* fmt, FILE* fp);


/**
 * 文件描述符
*/

typedef struct ufs_vfs_t {
    const char* type;

    // 关闭文件
    void (*close)(struct ufs_vfs_t* vfs);

    // 带偏移量的读取
    int (*pread)(struct ufs_vfs_t* vfs, void* buf, size_t len, int64_t off, size_t* pread);
    // 带偏移量的写入
    int (*pwrite)(struct ufs_vfs_t* vfs, const void* buf, size_t len, int64_t off, size_t* pwriten);
    // 同步文件
    int (*sync)(struct ufs_vfs_t* vfs);
} ufs_vfs_t;
// 打开一个文件，当无法独立占有文件时报错
UFS_API int ufs_vfs_open_file(ufs_vfs_t** pfd, const char* path);
UFS_API int ufs_vfs_is_file(ufs_vfs_t* vfs);

UFS_API int ufs_vfs_open_memory(ufs_vfs_t** pfd, const void* src, size_t len);
UFS_API int ufs_vfs_is_memory(ufs_vfs_t* vfs);
UFS_API int ufs_vfs_lock_memory(ufs_vfs_t* vfs);
UFS_API int ufs_vfs_unlock_memory(ufs_vfs_t* vfs);
UFS_API char* ufs_vfs_get_memory(ufs_vfs_t* vfs, size_t* psize);


/**
 * 配置
*/

#define UFS_MAGIC1 (67) // 魔数1
#define UFS_MAGIC2 (74) // 魔数2

#define UFS_NAME_MAX (64 - 8 - 1) // 目录最大名称长度
#define UFS_BLOCK_SIZE (1024) // 块的大小（必须是2的指数）
#define UFS_INODE_DEFAULT_RATIO (16*1024) // inode的默认比值（每16KB添加一个inode）

#define UFS_JORNAL_NUM (120) // 最大日志数量

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
#define UFS_O_EXEC      (1l << 3)  // 打开：可执行
#define UFS_O_CREAT     (1l << 4)  // 打开：如果文件不存在，创建它
#define UFS_O_EXCL      (1l << 5)  // 打开：使用UFS_O_CREAT生效，如果文件已存在，打开失败
#define UFS_O_TRUNC     (1l << 6)  // 打开：截断文件
#define UFS_O_APPEND    (1l << 7)  // 打开：始终追加写入

#define UFS_O_MASK 0xFF // 打开标志掩码

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

// 创建磁盘
UFS_API int ufs_new(ufs_t** pufs, ufs_vfs_t* vfs);
// 创建并格式化磁盘
UFS_API int ufs_new_format(ufs_t** pufs, ufs_vfs_t* vfs, uint64_t size);
// 同步磁盘内容
UFS_API int ufs_sync(ufs_t* ufs);
// 销毁磁盘
UFS_API void ufs_destroy(ufs_t* ufs);

typedef struct ufs_statvfs_t {
    uint64_t f_bsize;
    uint64_t f_namemax;

    uint64_t f_blocks;
    uint64_t f_bfree;
    uint64_t f_bavail;

    uint64_t f_files;
    uint64_t f_ffree;
    uint64_t f_favail;
} ufs_statvfs_t;
/**
 * 获取磁盘信息
 * 
 * 错误；
 *   [UFS_EINVAL] ufs为NULL或者stat为NULL
*/
UFS_API int ufs_statvfs(ufs_t* ufs, ufs_statvfs_t* stat);


struct ufs_file_t;
typedef struct ufs_file_t ufs_file_t;
/**
 * 打开文件
 *
 * 错误：
 *   [UFS_EINVAL] context非法
 *   [UFS_EINVAL] pfile为NULL
 *   [UFS_EINVAL] path为NULL或者path为空
 *   [UFS_EINVAL] （指导性错误）用户试图使用UFS_O_RDONLY和UFS_O_WRONLY合成UFS_O_RDWR
 *   [UFS_ENAMETOOLONG] 文件名过长
 *   [UFS_ENOTDIR] 路径中存在非目录或指向非目录的符号链接
 *   [UFS_ELOOP] 路径中符号链接层数过深
 *   [UFS_ENOMEM] 无法分配内存
 *   [UFS_ENOENT] 路径中存在不存在的目录
 *   [UFS_EACCESS] 用户对路径中的目录不具备执行权限
 *   [UFS_EACCESS] 试图创建文件但用户不具备对目录的写权限
 *   [UFS_EACCESS] 用户对文件不存在指定权限
 *   [UFS_EACCESS] 试图截断文件但用户不具备写入权限
 *   [UFS_ENOSPC] 试图创建文件但磁盘空间不足
 *   [UFS_EEXIST] 文件被指定创建和排他，但是目标文件已存在
 *   [UFS_EISDIR] 用户试图打开目录
*/
UFS_API int ufs_open(ufs_context_t* context, ufs_file_t** pfile, const char* path, unsigned long flag, uint16_t mask);
/**
 * 创建文件
 *
 * 错误：
 *   [UFS_EINVAL] context非法
 *   [UFS_EINVAL] pfile为NULL
 *   [UFS_EINVAL] path为NULL或者path为空
 *   [UFS_EINVAL] （指导性错误）用户试图使用UFS_O_RDONLY和UFS_O_WRONLY合成UFS_O_RDWR
 *   [UFS_ENAMETOOLONG] 文件名过长
 *   [UFS_ENOTDIR] 路径中存在非目录或指向非目录的符号链接
 *   [UFS_ELOOP] 路径中符号链接层数过深
 *   [UFS_ENOMEM] 无法分配内存
 *   [UFS_ENOENT] 路径中存在不存在的目录
 *   [UFS_EACCESS] 用户对路径中的目录不具备执行权限
 *   [UFS_EACCESS] 试图创建文件但用户不具备对目录的写权限
 *   [UFS_EACCESS] 用户对文件不存在指定权限
 *   [UFS_ENOSPC] 试图创建文件但磁盘空间不足
 *   [UFS_EEXIST] 文件被指定创建和排他，但是目标文件已存在
 *   [UFS_EISDIR] 用户试图打开目录
*/
UFS_API int ufs_creat(ufs_context_t* context, ufs_file_t** pfile, const char* path, uint16_t mask);
/**
 * 关闭文件
 *
 * 错误：
 *   [UFS_EBADF] file为NULL
*/
UFS_API int ufs_close(ufs_file_t* file);
/**
 * 读取文件
 *
 * 错误：
 *   [UFS_EBADF] file为NULL
 *   [UFS_EBADF] file没有读取权限
 *   [UFS_EINVAL] buf为NULL
*/
UFS_API int ufs_read(ufs_file_t* file, void* buf, size_t len, size_t* pread);
/**
 * 写入文件
 *
 * 错误：
 *   [UFS_EBADF] file为NULL
 *   [UFS_EBADF] file没有写入权限
 *   [UFS_EINVAL] buf为NULL
 *   [UFS_ENOSPC] 磁盘空间不足
*/
UFS_API int ufs_write(ufs_file_t* file, const void* buf, size_t len, size_t* pwriten);
/**
 * 含偏移量的读取文件
 *
 * 错误：
 *   [UFS_EBADF] file为NULL
 *   [UFS_EBADF] file没有读取权限
 *   [UFS_EINVAL] buf为NULL
*/
UFS_API int ufs_pread(ufs_file_t* file, void* buf, size_t len, uint64_t off, size_t* pread);
/**
 * 含偏移量的写入文件
 *
 * 错误：
 *   [UFS_EBADF] file为NULL
 *   [UFS_EBADF] file没有写入权限
 *   [UFS_EINVAL] buf为NULL
 *   [UFS_ENOSPC] 磁盘空间不足
*/
UFS_API int ufs_pwrite(ufs_file_t* file, const void* buf, size_t len, uint64_t off, size_t* pwriten);

#define UFS_SEEK_SET 0 // 设置偏移：从文件起始开始
#define UFS_SEEK_CUR 1 // 设置偏移：从文件当前位置开始
#define UFS_SEEK_END 2 // 设置偏移：从文件末尾开始
/**
 * 设置偏移
 *
 * 错误：
 *   [UFS_EBADF] file为NULL
 *   [UFS_EINVAL] off非法
 *   [UFS_EINVAL] wherence非法
*/
UFS_API int ufs_seek(ufs_file_t* file, int64_t off, int wherence, uint64_t* poff);
/**
 * 获得偏移
 *
 * 错误：
 *   [UFS_EBADF] file为NULL
 *   [UFS_EINVAL] poff为NULL
*/
UFS_API int ufs_tell(ufs_file_t* file, uint64_t* poff);
/**
 * 为文件预分配大小
 * 
 * 错误：
 *   [UFS_EBADF] file为NULL
*/
UFS_API int ufs_fallocate(ufs_file_t* file, uint64_t off, uint64_t len);
/**
 * 截断文件
 * 
 * 错误：
 *   [UFS_EBADF] file为NULL
 *   [UFS_EBADF] file没有写权限
 *   [UFS_EACCESS] 用户对文件没有写权限
*/
UFS_API int ufs_ftruncate(ufs_file_t* file, uint64_t size);
/**
 * 刷新文件
 * 
 * 错误：
 *   [UFS_EBADF] file为NULL
*/
UFS_API int ufs_fsync(ufs_file_t* file, int only_data);



struct ufs_dir_t;
typedef struct ufs_dir_t ufs_dir_t;
typedef struct ufs_dirent_t {
    uint64_t d_ino;
    uint64_t d_off;
    char d_name[UFS_NAME_MAX + 1];
} ufs_dirent_t;
/*
 * 打开目录
 *
 * 错误：
 *   [UFS_EINVAL] context非法
 *   [UFS_EINVAL] pfile为NULL
 *   [UFS_EINVAL] path为NULL或者path为空
 *   [UFS_ENAMETOOLONG] 文件名过长
 *   [UFS_ENOTDIR] 路径中存在非目录或指向非目录的符号链接
 *   [UFS_ENOTDIR] 用户试图打开目录
 *   [UFS_ELOOP] 路径中符号链接层数过深
 *   [UFS_ENOMEM] 无法分配内存
 *   [UFS_ENOENT] 路径中存在不存在的目录
 *   [UFS_EACCESS] 用户对路径中的目录不具备执行权限
 *   [UFS_EACCESS] 用户对目录不具备读权限
*/
UFS_API int ufs_opendir(ufs_context_t* context, ufs_dir_t** pdir, const char* path);

/**
 * 读取目录
 *
 * 错误：
 *   [UFS_EBADF] dir非法
 *   [UFS_EINVAL] dirent为NULL
 *   [UFS_ENOENT] 目录已读取完毕
 *   [UFS_EACCESS] 对目录不具备读入权限
*/
UFS_API int ufs_readdir(ufs_dir_t* dir, ufs_dirent_t* dirent);
/**
 * 定位目录
 *
 * 错误：
 *   [UFS_EBADF] dir非法
*/
UFS_API int ufs_seekdir(ufs_dir_t* dir, uint64_t off);
/**
 * 获得目录定位
 *
 * 错误：
 *   [UFS_EBADF] dir非法
 *   [UFS_EINVAL] poff非法
*/
UFS_API int ufs_telldir(ufs_dir_t* dir, uint64_t* poff);
/**
 * 定位目录到初始状态
 *
 * 错误：
 *   [UFS_EBADF] dir非法
*/
UFS_API int ufs_rewinddir(ufs_dir_t* dir);
/**
 * 关闭目录
 *
 * 错误：
 *   [UFS_EBADF] dir非法
*/
UFS_API int ufs_closedir(ufs_dir_t* dir);

/**
 * 创建目录
 *
 * 错误：
 *   [UFS_ENAMETOOLONG] 文件名过长
 *   [UFS_ENOTDIR] 路径中存在非目录或指向非目录的符号链接
 *   [UFS_ELOOP] 路径中符号链接层数过深
 *   [UFS_ENOMEM] 无法分配内存
 *   [UFS_ENOENT] 路径中存在不存在的目录
 *   [UFS_EACCESS] 用户对路径中的目录不具备执行权限
 *   [UFS_EACCESS] 试图创建文件但用户不具备对目录的写权限
 *   [UFS_ENOSPC] 试图创建目录但磁盘空间不足
 *
 *   [UFS_EINVAL] context非法
 *   [UFS_EINVAL] path为NULL或path为空
 *   [UFS_EEXIST] 目录已经存在
*/
UFS_API int ufs_mkdir(ufs_context_t* context, const char* path, uint16_t mode);
/**
 * 删除目录
 *
 * 错误：
 *   [UFS_ENAMETOOLONG] 文件名过长
 *   [UFS_ENOTDIR] 路径中存在非目录或指向非目录的符号链接
 *   [UFS_ELOOP] 路径中符号链接层数过深
 *   [UFS_ENOMEM] 无法分配内存
 *   [UFS_ENOENT] 路径中存在不存在的目录
 *   [UFS_EACCESS] 用户对路径中的目录不具备执行权限
 *   [UFS_EACCESS] 试图创建文件但用户不具备对目录的写权限
 *   [UFS_ENOSPC] 试图创建目录但磁盘空间不足
 *
 *   [UFS_EINVAL] context非法
 *   [UFS_EINVAL] path为NULL或path为空
 *   [UFS_ENOENT] 目录不存在
 *   [UFS_ENOTDIR] 不是目录
*/
UFS_API int ufs_rmdir(ufs_context_t* context, const char* path);
/**
 * 删除文件（包括符号链接）
 *
 * 错误：
 *   [UFS_ENAMETOOLONG] 文件名过长
 *   [UFS_ENOTDIR] 路径中存在非目录或指向非目录的符号链接
 *   [UFS_ELOOP] 路径中符号链接层数过深
 *   [UFS_ENOMEM] 无法分配内存
 *   [UFS_ENOENT] 路径中存在不存在的目录
 *   [UFS_EACCESS] 用户对路径中的目录不具备执行权限
 *   [UFS_EACCESS] 试图创建文件但用户不具备对目录的写权限
 *   [UFS_ENOSPC] 试图创建目录但磁盘空间不足
 *
 *   [UFS_EINVAL] context非法
 *   [UFS_EINVAL] path为NULL或path为空
 *   [UFS_ENOENT] 目录不存在
 *   [UFS_EISDIR] 要删除的是目录
*/
UFS_API int ufs_unlink(ufs_context_t* context, const char* path);
/**
 * 创建硬链接
 *
 * 错误：
 *   [UFS_ENAMETOOLONG] 文件名过长
 *   [UFS_ENOTDIR] 路径中存在非目录或指向非目录的符号链接
 *   [UFS_ELOOP] 路径中符号链接层数过深
 *   [UFS_ENOMEM] 无法分配内存
 *   [UFS_ENOENT] 路径中存在不存在的目录
 *   [UFS_EACCESS] 用户对路径中的目录不具备执行权限
 *   [UFS_EACCESS] 试图创建文件但用户不具备对目录的写权限
 *   [UFS_ENOSPC] 试图创建链接但磁盘空间不足
 *
 *   [UFS_EINVAL] context非法
 *   [UFS_EINVAL] path为NULL或path为空
 *   [UFS_EEXIST] target已经存在
 *   [UFS_EISDIR] 试图链接目录
 *   [UFS_EMLINK] 硬链接数量过多
*/
UFS_API int ufs_link(ufs_context_t* context, const char* target, const char* source);
/**
 * 创建符号链接
 *
 * 错误：
 *   [UFS_ENAMETOOLONG] 文件名过长
 *   [UFS_ENOTDIR] 路径中存在非目录或指向非目录的符号链接
 *   [UFS_ELOOP] 路径中符号链接层数过深
 *   [UFS_ENOMEM] 无法分配内存
 *   [UFS_ENOENT] 路径中存在不存在的目录
 *   [UFS_EACCESS] 用户对路径中的目录不具备执行权限
 *   [UFS_EACCESS] 试图创建文件但用户不具备对目录的写权限
 *   [UFS_ENOSPC] 试图创建链接但磁盘空间不足
 *
 *   [UFS_EINVAL] context非法
 *   [UFS_EINVAL] path为NULL或path为空
 *   [UFS_EEXIST] target已经存在
 *   [UFS_EOVERFLOW] source过长
*/
UFS_API int ufs_symlink(ufs_context_t* context, const char* target, const char* source);
/**
 * 创建符号链接
 *
 * 错误：
 *   [UFS_ENAMETOOLONG] 文件名过长
 *   [UFS_ENOTDIR] 路径中存在非目录或指向非目录的符号链接
 *   [UFS_ELOOP] 路径中符号链接层数过深
 *   [UFS_ENOMEM] 无法分配内存
 *   [UFS_ENOENT] 路径中存在不存在的目录
 *   [UFS_EACCESS] 用户对路径中的目录不具备执行权限
 *   [UFS_EACCESS] 试图创建文件但用户不具备对目录的写权限
 *
 *   [UFS_EINVAL] context非法
 *   [UFS_EINVAL] path为NULL或path为空
 *   [UFS_EINVAL] presolved为NULL
 *   [UFS_EEXIST] target已经存在
 *   [UFS_EFTYPE] source不为符号链接
*/
UFS_API int ufs_readlink(ufs_context_t* context, const char* source, char** presolved);

typedef struct ufs_stat_t {
    uint64_t st_ino; // inode编号
    uint16_t st_mode; // 文件模式与类型
    uint32_t st_nlink; // 链接数
    int32_t st_uid; // 所属者UID
    int32_t st_gid; // 所属者GID
    uint64_t st_size; // 文件大小
    uint64_t st_blksize; // 块大小
    uint64_t st_blocks; // 使用块数

    int64_t st_ctim; // 创建时间
    int64_t st_atim; // 修改时间
    int64_t st_mtim; // 访问时间
} ufs_stat_t;

/**
 * 获取文件状态
 *
 * 错误：
 *   [UFS_EINVAL] stat为NULL
 *   [UFS_EBADF] file非法
*/
UFS_API int ufs_fstat(ufs_file_t* file, ufs_stat_t* stat);
/**
 * 获取文件状态
 *
 * 错误：
 *   [UFS_ENAMETOOLONG] 文件名过长
 *   [UFS_ENOTDIR] 路径中存在非目录或指向非目录的符号链接
 *   [UFS_ELOOP] 路径中符号链接层数过深
 *   [UFS_ENOMEM] 无法分配内存
 *   [UFS_ENOENT] 路径中存在不存在的目录
 *   [UFS_EACCESS] 用户对路径中的目录不具备执行权限
 *   [UFS_ENOSPC] 试图创建链接但磁盘空间不足
 *
 *   [UFS_EINVAL] context非法
 *   [UFS_EINVAL] stat为NULL
 *   [UFS_EINVAL] path为NULL或path为空
*/
UFS_API int ufs_stat(ufs_context_t* context, const char* path, ufs_stat_t* stat);
/**
 * 更改文件权限
*/
UFS_API int ufs_chmod(ufs_context_t* context, const char* path, uint16_t mask);
/**
 * 更改文件所属者
*/
UFS_API int ufs_chown(ufs_context_t* context, const char* path, int32_t uid, int32_t gid);
/**
 * 测试文件
*/
UFS_API int ufs_access(ufs_context_t* context, const char* path, int access);
/**
 * 更改文件时间
*/
UFS_API int ufs_utimes(ufs_context_t* context, const char* path, int64_t* ctime, int64_t* atime, int64_t* mtime);
/**
 * 截断文件
*/
UFS_API int ufs_truncate(ufs_context_t* context, const char* path, uint64_t size);
/**
 * 重命名文件
*/
UFS_API int ufs_rename(ufs_context_t* context, const char* oldname, const char* newname);

/**
 * 获得文件的物理地址
 */
typedef struct ufs_physics_addr_t {
    uint64_t inode_off;
    uint64_t zone_off[16];
} ufs_physics_addr_t;
UFS_API int ufs_physics_addr(ufs_context_t* context, const char* name, ufs_physics_addr_t* addr);

#ifdef __cplusplus
}
#endif

#endif /* UFS_LIB_H */
