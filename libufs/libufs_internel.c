#include "libufs_internel.h"

UFS_API const char* ufs_strerror(int error) {
    static const char* TABLE[] = {
        "[UFS_EUNKOWN] unknown error",
        "[UFS_EACCESS] permission denied",
        "[UFS_EAGAIN] resource temporarily unavailable",
        "[UFS_EBADF] bad file descriptor",
        "[UFS_ENOENT] no such file or directory",
        "[UFS_ENOMEM] cannot allocate memory",
        "[UFS_ENOTDIR] not a directory",
        "[UFS_EISDIR] is a directory",
        "[UFS_EINVAL] invalid argument",
        "[UFS_EMFILE] too many open files",
        "[UFS_EFBIG] file too large",
        "[UFS_EMLINK] too many links",
        "[UFS_ELOOP] too many levels of symbolic links",
        "[UFS_ENAMETOOLONG] file name too long",
        "[UFS_ESTALE] stale file handle",
        "[UFS_EFTYPE] inappropriate file type or format",
        "[UFS_EILSEQ] invalid or incomplete multibye or wide character",
        "[UFS_EOVERFLOW] value too large for defined data type",
        "[UFS_ENOSPC] no space left on device",
        "[UFS_EEXIST] file exists",
        "[UFS_ENOTEMPTY] directory not empty",
    };
    static const int TABLE_CNT = sizeof(TABLE) / sizeof(TABLE[0]);
    if(error >= 0) return strerror(error);
    if(-error >= TABLE_CNT) return "[UFS_E?] unkown error";
    return TABLE[-error - 1];
}

// 在Windows上，我们无法准确地实现UID和GID
#ifdef _WIN32
    UFS_API int32_t ufs_getuid(void) { return 0; }
    UFS_API int32_t ufs_getgid(void) { return 0; }
    UFS_API int ufs_setuid(int32_t uid) { (void)uid; return 0; }
    UFS_API int ufs_setgid(int32_t gid) { (void)gid; return 0; }
#else
    #include <unistd.h>
    UFS_API int32_t ufs_getuid(void) { return ul_static_cast(int32_t, getuid()); }
    UFS_API int32_t ufs_getgid(void) { return ul_static_cast(int32_t, getgid()); }
#endif

#include "uldate.h"
UFS_API int64_t ufs_time(int use_locale) {
    const uldate_t date = use_locale ? uldate_now_locale() : uldate_now_utc();
    return ufs_likely(date != ULDATE_INVALID) ? date : 0;
}
UFS_API size_t ufs_strtime(int64_t time, char* buf, const char* fmt, size_t len) {
    if(fmt == NULL) fmt = "%FT%T.%+Z";
    if(buf == NULL) return uldate_format_len(fmt, time);
    else return uldate_format(buf, len, fmt, time);
}
UFS_API int ufs_ptime(int64_t time, const char* fmt, FILE* fp) {
    char* buf;
    size_t len;
    int ret = EOF;

    if(fmt == NULL) fmt = "%FT%T.%+Z";
    len = uldate_format_len(fmt, time);
    buf = ul_reinterpret_cast(char*, malloc(len));
    if(buf == NULL) return EOF;
    if(uldate_format(buf, len, fmt, time) == len) ret = fputs(buf, fp);
    free(buf);
    return ret;
}
