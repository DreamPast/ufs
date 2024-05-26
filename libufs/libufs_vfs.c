#include "libufs_internel.h"
#include "ulfd.h"

UFS_HIDDEN int ufs_vfs_pread_check(ufs_vfs_t* ufs_restrict vfs, void* ufs_restrict buf, size_t len, int64_t off) {
    char* _buf = ul_reinterpret_cast(char*, buf);
    size_t read;
    int ec = 0;

    while(len) {
        ec = vfs->pread(vfs, _buf, len, off, &read);
        if(ec == 0) {
            if(read == 0) {
                memset(_buf, 0, len);
                read = len;
            }
            _buf += read;
            len -= read;
            off += ul_static_cast(int64_t, read);
        } else if(ec != EAGAIN && ec != EWOULDBLOCK) return ec;
    }
    return 0;
}
UFS_HIDDEN int ufs_vfs_pwrite_check(ufs_vfs_t* ufs_restrict vfs, const void* ufs_restrict buf, size_t len, int64_t off) {
    const char* _buf = ul_reinterpret_cast(const char*, buf);
    size_t writen;
    int ec = 0;

    while(len) {
        ec = vfs->pwrite(vfs, _buf, len, off, &writen);
        if(ec == 0) {
            if(writen == 0) return ENOSPC;
            _buf += writen;
            len -= writen;
            off += ul_static_cast(int64_t, writen);
        } else if(ec != EAGAIN && ec != EWOULDBLOCK) return ec;
    }
    return 0;
}
UFS_HIDDEN int ufs_vfs_copy(ufs_vfs_t* vfs, int64_t off_in, int64_t off_out, size_t len) {
    char* cache = NULL;
    size_t cache_len = len;
    int ec = 0;
    size_t nread, nwrite;

    while(cache_len) {
        cache = ul_reinterpret_cast(char*, ufs_malloc(cache_len));
        if(ufs_likely(cache != NULL)) break;
        cache_len >>= 1;
    }
    if(ufs_unlikely(cache_len == 0)) return ENOMEM;

    while(len) {
        for(;;) {
            ec = vfs->pread(vfs, cache, len < cache_len ? len : cache_len, off_in, &nread);
            if(ec) {
                if(ec != EAGAIN && ec != EWOULDBLOCK) goto do_return;
            } else {
                if(nread == 0) {
                    nread = len < cache_len ? len : cache_len;
                    memset(cache, 0, nread);
                }
                break;
            }
        }

        for(;;) {
            ec = vfs->pwrite(vfs, cache, nread, off_out, &nwrite);
            if(ec) {
                if(ec != EAGAIN && ec != EWOULDBLOCK) goto do_return;
            } else {
                if(nwrite == 0) { ec = ENOSPC; goto do_return; }
                else break;
            }
        }

        off_in += ul_static_cast(int64_t, nwrite);
        off_out += ul_static_cast(int64_t, nwrite);
        len -= nwrite;
    }

do_return:
    ul_free(cache);
    return ec;
}
UFS_HIDDEN int ufs_vfs_pwrite_zeros(ufs_vfs_t* vfs, size_t len, int64_t off) {
    int ec;
    static const char zeros[1024] = { 0 };
    while(len > 1024) {
        ec = ufs_vfs_pwrite_check(vfs, zeros, sizeof(zeros), off);
        if(ufs_unlikely(ec)) return ec;
        off += ul_static_cast(int64_t, sizeof(zeros));
    }
    return ufs_vfs_pwrite_check(vfs, zeros, len, off);
}

#if 1
    typedef struct _fd_file_t {
        ufs_vfs_t b;
        ulfd_t vfs;
    #if defined(ULFD_NO_PREAD) || defined(ULFD_NO_PWRITE)
        ulatomic_spinlock_t lock;
    #endif
    } _fd_file_t;
    static const char _fd_file_type[] = "FILE";

    static void _fd_file_close(ufs_vfs_t* vfs) {
        _fd_file_t* _fd = ul_reinterpret_cast(_fd_file_t*, vfs);
        ulfd_lock(_fd->vfs, 0, UFS_BNUM_START * UFS_BLOCK_SIZE, ULFD_F_UNLCK);
        ulfd_close(_fd->vfs);
        ufs_free(vfs);
    }
    static int _fd_file_pread(ufs_vfs_t* vfs, void* buf, size_t len, int64_t off, size_t* pread) {
        _fd_file_t* _fd = ul_reinterpret_cast(_fd_file_t*, vfs);
    #if defined(ULFD_NO_PREAD) || defined(ULFD_NO_PWRITE)
        ulatomic_spinlock_lock(&_fd->lock);
        do {
            ulfd_int64_t off;
            int err = ulfd_seek(_fd->vfs, off, ULFD_SEEK_SET, &off);
            if(ufs_unlikely(err)) return err;
            return ulfd_read(_fd->vfs, buf, len, pread);
        } while(0);
        ulatomic_spinlock_unlock(&_fd->lock);
    #else
        return ulfd_pread(_fd->vfs, buf, len, off, pread);
    #endif
    }
    static int _fd_file_pwrite(ufs_vfs_t* vfs, const void* buf, size_t len, int64_t off, size_t* pwriten) {
        _fd_file_t* _fd = ul_reinterpret_cast(_fd_file_t*, vfs);
    #if defined(ULFD_NO_PREAD) || defined(ULFD_NO_PWRITE)
        ulatomic_spinlock_lock(&_fd->lock);
        do {
            ulfd_int64_t off;
            int err = ulfd_seek(_fd->vfs, off, ULFD_SEEK_SET, &off);
            if(ufs_unlikely(err)) return err;
            return ulfd_write(_fd->vfs, buf, len, pwriten);
        } while(0);
        ulatomic_spinlock_unlock(&_fd->lock);
    #else
        return ulfd_pwrite(_fd->vfs, buf, len, off, pwriten);
    #endif

    }
    static int _fd_file_sync(ufs_vfs_t* vfs) {
        return ulfd_ffullsync(ul_reinterpret_cast(_fd_file_t*, vfs)->vfs);
    }

    UFS_API int ufs_vfs_open_file(ufs_vfs_t** pfd, const char* path) {
        _fd_file_t* vfs;
        int err;

        if(ufs_unlikely(pfd == NULL || path == NULL)) return EINVAL;

        vfs = ul_reinterpret_cast(_fd_file_t*, ufs_realloc(NULL, sizeof(_fd_file_t)));
        if(ufs_unlikely(vfs == NULL)) return ENOMEM;

        err = ulfd_open(&vfs->vfs, path, ULFD_O_RDWR | ULFD_O_CREAT, 0666);
        if(err) { ufs_free(vfs); return err; }

        // 锁定文件，避免第二个进程进行读写
        err = ulfd_lock(vfs->vfs, 0, UFS_BNUM_START * UFS_BLOCK_SIZE, ULFD_F_WRLCK);
        if(err) { ulfd_close(vfs->vfs); ufs_free(vfs); return err; }

        vfs->b.type = _fd_file_type;
        vfs->b.close = &_fd_file_close;
        vfs->b.pread = &_fd_file_pread;
        vfs->b.pwrite = &_fd_file_pwrite;
        vfs->b.sync = &_fd_file_sync;

        *pfd = ul_reinterpret_cast(ufs_vfs_t*, vfs);
    #if defined(ULFD_NO_PREAD) || defined(ULFD_NO_PWRITE)
        ulatomic_spinlock_init(&vfs->lock);
    #endif
        return 0;
    }
    UFS_API int ufs_vfs_is_file(ufs_vfs_t* vfs) {
        return vfs && vfs->type == _fd_file_type;
    }
#endif

#if 1
    typedef struct _fd_memory_t {
        ufs_vfs_t b;
        ulatomic_spinlock_t lock;
        char* memory;
        size_t len;
    } _fd_memory_t;
    static const char _fd_memory_type[] = "MEMORY";

    static void _fd_memory_close(ufs_vfs_t* _fd) {
        _fd_memory_t* vfs = ul_reinterpret_cast(_fd_memory_t*, _fd);
        ufs_free(vfs->memory);
        ufs_free(vfs);
    }
    static int _fd_memory_pread(ufs_vfs_t* _fd, void* buf, size_t len, int64_t off, size_t* pread) {
        _fd_memory_t* vfs = ul_reinterpret_cast(_fd_memory_t*, _fd);
        int ec = 0;
        size_t read;

        ulatomic_spinlock_lock(&vfs->lock);
        if(off < 0) { ec = EINVAL; goto do_return; }
        if(ul_static_cast(size_t, off) >= vfs->len) {
            ec = 0; *pread = 0; goto do_return;
        }
        read = vfs->len - ul_static_cast(size_t, off);
        if(read > len) read = len;
        memcpy(buf, vfs->memory, read);
        *pread = read;

    do_return:
        ulatomic_spinlock_unlock(&vfs->lock);
        return ec;
    }
    static int _fd_memory_pwrite(ufs_vfs_t* _fd, const void* buf, size_t len, int64_t off, size_t* pwriten) {
        _fd_memory_t* vfs = ul_reinterpret_cast(_fd_memory_t*, _fd);
        int ec = 0;
        char* new_memory;

        ulatomic_spinlock_lock(&vfs->lock);
        if(off < 0) { ec = EINVAL; goto do_return; }
        if(ul_static_cast(size_t, off) + len > vfs->len) {
            new_memory = ul_realloc(vfs->memory, ul_static_cast(size_t, off) + len);
            if(ufs_unlikely(new_memory == NULL)) { ec = ENOSPC; goto do_return; }
            vfs->memory = new_memory;
            vfs->len = ul_static_cast(size_t, off) + len;
        }
        memcpy(vfs->memory + off, buf, len);
        *pwriten = len;

    do_return:
        ulatomic_spinlock_unlock(&vfs->lock);
        return ec;
    }
    static int _fd_memory_sync(ufs_vfs_t* _fd) {
        (void)_fd; return 0;
    }

    UFS_API int ufs_vfs_open_memory(ufs_vfs_t** pfd, const void* src, size_t len) {
        _fd_memory_t* vfs;
        char* mem;

        if(ufs_unlikely(pfd == NULL)) return EINVAL;
        vfs = ul_reinterpret_cast(_fd_memory_t*, ufs_malloc(sizeof(_fd_memory_t)));
        if(ufs_unlikely(vfs == NULL)) return ENOMEM;
        mem = ul_reinterpret_cast(char*, ufs_malloc(len));
        if(ufs_unlikely(mem == NULL)) { ufs_free(vfs); return ENOMEM; }

        if(src) memcpy(mem, src, len);
        else memset(mem, 0, len);

        vfs->memory = mem;
        vfs->len = len;
        ulatomic_spinlock_unlock(&vfs->lock);

        vfs->b.type = _fd_memory_type;
        vfs->b.close = _fd_memory_close;
        vfs->b.pread = _fd_memory_pread;
        vfs->b.pwrite = _fd_memory_pwrite;
        vfs->b.sync = _fd_memory_sync;

        *pfd = ul_reinterpret_cast(ufs_vfs_t*, vfs);
        return 0;
    }
    UFS_API int ufs_vfs_is_memory(ufs_vfs_t* vfs) {
        return vfs && vfs->type == _fd_memory_type;
    }
    UFS_API int ufs_vfs_lock_memory(ufs_vfs_t* vfs) {
        if(ufs_vfs_is_memory(vfs)) { ulatomic_spinlock_lock(&ul_reinterpret_cast(_fd_memory_t*, vfs)->lock); return 0; }
        else return EINVAL;
    }
    UFS_API int ufs_vfs_unlock_memory(ufs_vfs_t* vfs) {
        if(ufs_vfs_is_memory(vfs)) { ulatomic_spinlock_unlock(&ul_reinterpret_cast(_fd_memory_t*, vfs)->lock); return 0; }
        else return EINVAL;
    }
    UFS_API char* ufs_vfs_get_memory(ufs_vfs_t* _fd, size_t* psize) {
        _fd_memory_t* vfs = ul_reinterpret_cast(_fd_memory_t*, _fd);
        if(!ufs_vfs_is_memory(_fd)) return NULL;
        if(psize) *psize = vfs->len;
        return vfs->memory;
    }
#endif
