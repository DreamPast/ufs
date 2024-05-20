#ifndef LIBUFS_THREAD_H
#define LIBUFS_THREAD_H

#ifdef LIBUFS_NO_THREAD_SAFE
    #define ULATOMIC_SINGLE_THREAD
    #define ULATOMIC_NEEDED

    #define ULMTX_SINGLE_THREAD
    #define ULMTX_NEEDED
#endif

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

#ifndef ul_const_cast
    #ifdef __cplusplus
        #define ufs_const_cast(T, val) const_cast<T>(val)
    #else
        // C语言在高警告等级下const的移除会导致警告，我们需要将其转化为地址再转回指针
        #define ufs_const_cast(T, val) ((T)((intptr_t)(val)))
    #endif
#endif

#include "libufs.h"
#include "ulatomic.h"
#include "ulmtx.h"

/*
typedef struct ufs_threadpool_t {
    void* opaque;
    ulatomic_spinlock_t lck;
} ufs_threadpool_t;
#define UFS_THREADPOOL_INIT { NULL, ULATOMIC_SPINLOCK_INIT }
UFS_HIDDEN int ufs_threadpool_init(ufs_threadpool_t* pool);
typedef void (*ufs_threadpool_func_t)(void* opaque);
UFS_HIDDEN int ufs_threadpool_push(ufs_threadpool_t* pool, ufs_threadpool_func_t func);
UFS_HIDDEN int ufs_threadpool_deinit(ufs_threadpool_t* pool);
*/

#endif /* LIBUFS_THREAD_H */
