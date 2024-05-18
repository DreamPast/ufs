#include "libufs_internel.h"

UFS_API const char* ufs_strerror(int error) {
    static const char* TABLE[] = {
        "Unknown error",
    };
    static size_t TABLE_NUM = sizeof(TABLE) / sizeof(TABLE[0]);
    if(error >= 0) return strerror(error);
    error = -(error + 1);
    if(error >= ul_static_cast(int, TABLE_NUM))
        return "Illegal error code";
    else return TABLE[error];
}

UFS_API int ufs_union_error(int error) {
    static int TABLE[] = {
        EINVAL
    };
    static size_t TABLE_NUM = sizeof(TABLE) / sizeof(TABLE[0]);
    if(error >= 0) return error;
    error = -(error + 1);
    if(error >= ul_static_cast(int, TABLE_NUM))
        return EINVAL;
    else return TABLE[error];
}
