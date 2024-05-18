#include "libufs_thread.h"

/*
#ifdef _WIN32
    #include <process.h>
#endif

typedef struct _list_t {

} _list_t;

typedef struct _threadpool_t {
#ifdef _WIN32
    UINT_PTR thread;
#else
    pthread_t thread;
#endif
    int destory;
    ulatomic_spinlock_t* lck;
} _threadpool_t;

#ifdef _WIN32
    static void _subthread(void* _pool) {
        _threadpool_t* pool = ul_reinterpret_cast(_threadpool_t*, _pool);

    }
#else

#endif

UFS_HIDDEN int ufs_threadpool_init(ufs_threadpool_t* pool) {

    ulatomic_spinlock_lock(&pool->lck);
    if(!pool->opaque) {
        // _beginthread()
    }
    ulatomic_spinlock_unlock(&pool->lck);
}


typedef void (*ufs_threadpool_func_t)(void* opaque);
UFS_HIDDEN int ufs_threadpool_push(ufs_threadpool_t* pool, ufs_threadpool_func_t func);
UFS_HIDDEN int ufs_threadpool_deinit(ufs_threadpool_t* pool);
*/
