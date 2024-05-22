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

#ifdef __cplusplus
    #define ufs_const_cast(T, val) const_cast<T>(val)
#else
    // C语言在高警告等级下const的移除会导致警告，我们需要将其转化为地址再转回指针
    #define ufs_const_cast(T, val) ((T)((intptr_t)(val)))
#endif

#if defined(_MSC_VER)
    #define ufs_assume(cond) __assume(cond)
#endif
#if !defined(ufs_assume) && defined(__has_builtin)
    #if __has_builtin(__builtin_assume)
        #define ufs_assume(cond) __builtin_assume(cond)
    #endif
    #if __has_builtin(__builtin_unreachable)
        #if __has_builtin(__builtin_expect)
            #define ufs_assume(cond) (__builtin_expect(!(cond), 0) ? __builtin_unreachable() : (void)0)
        #else
            #define ufs_assume(cond) !(cond) ? __builtin_unreachable() : (void)0)
        #endif
    #endif
#endif
#ifndef ufs_assume
    #define ufs_assume(cond) ((void)(cond))
#endif

#ifndef ufs_restrict
  #if defined(_MSC_VER)
    #define ufs_restrict __restrict
  #elif defined(__GNUC__) && __GNUC__ > 3
    #define ufs_restrict __restrict__
  #elif defined(__STDC_VERSION__) && (__STDC_VERSION__ >= 199901L)
    #define ufs_restrict restrict
  #else
    #define ufs_restrict
  #endif
#endif /* ufs_restrict */

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
