#include "libufs_internel.h"
#include "ulfd.h"

UFS_HIDDEN int ufs_fd_pread_check(ufs_fd_t* fd, void* buf, size_t len, int64_t off) {
    char* _buf = ul_reinterpret_cast(char*, buf);
    size_t read;
    int ec = 0;

    while(len) {
        ec = fd->pread(fd, _buf, len, off, &read);
        if(ec == 0) {
            if(read == 0) {
                return UFS_ERROR_READ_NOT_ENOUGH;
            }
            _buf += read;
            len -= read;
            off += read;
        } else if(ec != EAGAIN && ec != EWOULDBLOCK) return ec;
    }
    return 0;
}
UFS_HIDDEN int ufs_fd_pwrite_check(ufs_fd_t* fd, const void* buf, size_t len, int64_t off) {
    const char* _buf = ul_reinterpret_cast(const char*, buf);
    size_t writen;
    int ec = 0;

    while(len) {
        ec = fd->pwrite(fd, _buf, len, off, &writen);
        if(ec == 0) {
            if(writen == 0) return ENOSPC;
            _buf += writen;
            len -= writen;
            off += writen;
        } else if(ec != EAGAIN && ec != EWOULDBLOCK) return ec;
    }
    return 0;
}
UFS_HIDDEN int ufs_fd_copy(ufs_fd_t* fd, int64_t off_in, int64_t off_out, size_t len) {
    char* cache = NULL;
    size_t cache_len = len;
    int ec = 0;
    size_t nread, nwrite;

    while(cache_len) {
        cache = ul_reinterpret_cast(char*, ufs_malloc(cache_len));
        if(ul_likely(cache != NULL)) break;
        cache_len >>= 1;
    }
    if(ul_unlikely(cache_len == 0)) return ENOMEM;

    while(len) {
        for(;;) {
            ec = fd->pread(fd, cache, len < cache_len ? len : cache_len, off_in, &nread);
            if(ec) {
                if(ec != EAGAIN && ec != EWOULDBLOCK) goto do_return;
            } else {
                if(nread == 0) { ec = UFS_ERROR_READ_NOT_ENOUGH; goto do_return; }
                else break;
            }
        }
        
        for(;;) {
            ec = fd->pwrite(fd, cache, nread, off_out, &nwrite);
            if(ec) {
                if(ec != EAGAIN && ec != EWOULDBLOCK) goto do_return;
            } else {
                if(nwrite == 0) { ec = ENOSPC; goto do_return; }
                else break;
            }
        }

        off_in += nwrite;
        off_out += nwrite;
        len -= nwrite;
    }

do_return:
    ul_free(cache);
    return ec;
}
UFS_HIDDEN int ufs_fd_pwrite_zeros(ufs_fd_t* fd, size_t len, int64_t off) {
    int ec;
    static const char zeros[1024] = { 0 };
    while(len > 1024) {
        ec = ufs_fd_pwrite_check(fd, zeros, sizeof(zeros), off);
        if(ul_unlikely(ec)) return ec;
        off += sizeof(zeros);
    }
    return ufs_fd_pwrite_check(fd, zeros, len, off);
}

#if 1
    typedef struct _fd_file_t {
        ufs_fd_t b;
        ulfd_t fd;
    #if defined(ULFD_NO_PREAD) || defined(ULFD_NO_PWRITE)
        ulatomic_spinlock_t lck;
    #endif
    } _fd_file_t;
    static const char _fd_file_type[] = "FILE";

    static void _fd_file_close(ufs_fd_t* fd) {
        _fd_file_t* _fd = ul_reinterpret_cast(_fd_file_t*, fd);
        ulfd_lock(_fd->fd, 0, UFS_BNUM_START * UFS_BLOCK_SIZE, ULFD_F_UNLCK);
        ulfd_close(_fd->fd);
        ufs_free(fd);
    }
    static int _fd_file_pread(ufs_fd_t* fd, void* buf, size_t len, int64_t off, size_t* pread) {
        _fd_file_t* _fd = ul_reinterpret_cast(_fd_file_t*, fd);
    #if defined(ULFD_NO_PREAD) || defined(ULFD_NO_PWRITE)
        ulatomic_spinlock_lock(&_fd->lck);
        do {
            ulfd_int64_t off;
            int err = ulfd_seek(_fd->fd, off, ULFD_SEEK_SET, &off);
            if(ul_unlikely(err)) return err;
            return ulfd_read(_fd->fd, buf, len, pread);
        } while(0);
        ulatomic_spinlock_unlock(&_fd->lck);
    #else
        return ulfd_pread(_fd->fd, buf, len, off, pread);
    #endif
    }
    static int _fd_file_pwrite(ufs_fd_t* fd, const void* buf, size_t len, int64_t off, size_t* pwriten) {
        _fd_file_t* _fd = ul_reinterpret_cast(_fd_file_t*, fd);
    #if defined(ULFD_NO_PREAD) || defined(ULFD_NO_PWRITE)
        ulatomic_spinlock_lock(&_fd->lck);
        do {
            ulfd_int64_t off;
            int err = ulfd_seek(_fd->fd, off, ULFD_SEEK_SET, &off);
            if(ul_unlikely(err)) return err;
            return ulfd_write(_fd->fd, buf, len, pwriten);
        } while(0);
        ulatomic_spinlock_unlock(&_fd->lck);
    #else
        return ulfd_pwrite(_fd->fd, buf, len, off, pwriten);
    #endif

    }
    static int _fd_file_sync(ufs_fd_t* fd) {
        return ulfd_ffullsync(ul_reinterpret_cast(_fd_file_t*, fd)->fd);
    }

    UFS_API int ufs_fd_open_file(ufs_fd_t** pfd, const char* path) {
        _fd_file_t* fd;
        int err;

        if(ul_unlikely(pfd == NULL || path == NULL)) return EINVAL;

        fd = ul_reinterpret_cast(_fd_file_t*, ufs_realloc(NULL, sizeof(_fd_file_t)));
        if(ul_unlikely(fd == NULL)) return ENOMEM;

        err = ulfd_open(&fd->fd, path, ULFD_O_RDWR, 0664);
        if(err) { ufs_free(fd); return err; }

        // 锁定文件，避免第二个进程进行读写
        err = ulfd_lock(fd->fd, 0, UFS_BNUM_START * UFS_BLOCK_SIZE, ULFD_F_WRLCK);
        if(err) { ulfd_close(fd->fd); ufs_free(fd); return err; }

        { // 检查文件大小是否是整块数
            ulfd_stat_t stat;
            err = ulfd_stat(path, &stat);
            if(ul_unlikely(err)) return err;
            if(stat.size % UFS_BLOCK_SIZE != 0) { ulfd_close(fd->fd); ufs_free(fd); return UFS_ERROR_BROKEN_DISK; }
        }

        fd->b.type = _fd_file_type;
        fd->b.close = &_fd_file_close;
        fd->b.pread = &_fd_file_pread;
        fd->b.pwrite = &_fd_file_pwrite;
        fd->b.sync = &_fd_file_sync;
        
        *pfd = ul_reinterpret_cast(ufs_fd_t*, fd);
    #if defined(ULFD_NO_PREAD) || defined(ULFD_NO_PWRITE)
        fd->lck = 0;
    #endif
        return 0;
    }
    UFS_API int ufs_fd_is_file(ufs_fd_t* fd) {
        return fd && fd->type == _fd_file_type;
    }
#endif

#if 1
    typedef struct _fd_memory_t {
        ufs_fd_t b;
        ulatomic_spinlock_t lock;
        char* memory;
        size_t len;
    } _fd_memory_t;
    static const char _fd_memory_type[] = "MEMORY";

    static void _fd_memory_close(ufs_fd_t* _fd) {
        _fd_memory_t* fd = ul_reinterpret_cast(_fd_memory_t*, _fd);
        ufs_free(fd->memory);
        ufs_free(fd);
    }
    static int _fd_memory_pread(ufs_fd_t* _fd, void* buf, size_t len, int64_t off, size_t* pread) {
        _fd_memory_t* fd = ul_reinterpret_cast(_fd_memory_t*, _fd);
        int ec = 0;
        size_t read;

        ulatomic_spinlock_lock(&fd->lock);
        if(off < 0) { ec = EINVAL; goto do_return; }
        if(ul_static_cast(size_t, off) >= fd->len) {
            ec = 0; *pread = 0; goto do_return;
        }
        read = fd->len - ul_static_cast(size_t, off);
        if(read > len) read = len;
        memcpy(buf, fd->memory, read);
        *pread = read;

    do_return:
        ulatomic_spinlock_unlock(&fd->lock);
        return ec;
    }
    static int _fd_memory_pwrite(ufs_fd_t* _fd, const void* buf, size_t len, int64_t off, size_t* pwriten) {
        _fd_memory_t* fd = ul_reinterpret_cast(_fd_memory_t*, _fd);
        int ec = 0;
        char* new_memory;

        ulatomic_spinlock_lock(&fd->lock);
        if(off < 0) { ec = EINVAL; goto do_return; }
        if(ul_static_cast(size_t, off) + len > fd->len) {
            new_memory = ul_realloc(fd->memory, ul_static_cast(size_t, off) + len);
            if(ul_unlikely(new_memory == NULL)) { ec = ENOSPC; goto do_return; }
            fd->memory = new_memory;
            fd->len = ul_static_cast(size_t, off) + len;
        }
        memcpy(fd->memory + off, buf, len);
        *pwriten = len;

    do_return:
        ulatomic_spinlock_unlock(&fd->lock);
        return ec;
    }
    static int _fd_memory_sync(ufs_fd_t* _fd) {
        (void)_fd; return 0;
    }

    UFS_API int ufs_fd_open_memory(ufs_fd_t** pfd, const void* src, size_t len) {
        _fd_memory_t* fd;
        char* mem;

        if(ul_unlikely(pfd == NULL)) return EINVAL;
        fd = ul_reinterpret_cast(_fd_memory_t*, ufs_malloc(sizeof(_fd_memory_t)));
        if(ul_unlikely(fd == NULL)) return ENOMEM;
        mem = ul_reinterpret_cast(char*, ufs_malloc(len));
        if(ul_unlikely(mem == NULL)) { ufs_free(fd); return ENOMEM; }

        if(src) memcpy(mem, src, len);
        else memset(mem, 0, len);

        fd->memory = mem;
        fd->len = len;
        ulatomic_spinlock_unlock(&fd->lock);

        fd->b.type = _fd_memory_type;
        fd->b.close = _fd_memory_close;
        fd->b.pread = _fd_memory_pread;
        fd->b.pwrite = _fd_memory_pwrite;
        fd->b.sync = _fd_memory_sync;
        
        *pfd = ul_reinterpret_cast(ufs_fd_t*, fd);
        return 0;
    }
    UFS_API int ufs_fd_is_memory(ufs_fd_t* fd) {
        return fd && fd->type == _fd_memory_type;
    }
    UFS_API int ufs_fd_lock_memory(ufs_fd_t* fd) {
        if(ufs_fd_is_memory(fd)) { ulatomic_spinlock_lock(&ul_reinterpret_cast(_fd_memory_t*, fd)->lock); return 0; }
        else return EINVAL;
    }
    UFS_API int ufs_fd_unlock_memory(ufs_fd_t* fd) {
        if(ufs_fd_is_memory(fd)) { ulatomic_spinlock_unlock(&ul_reinterpret_cast(_fd_memory_t*, fd)->lock); return 0; }
        else return EINVAL;
    }
    UFS_API char* ufs_fd_get_memory(ufs_fd_t* _fd, size_t* psize) {
        _fd_memory_t* fd = ul_reinterpret_cast(_fd_memory_t*, _fd);
        if(!ufs_fd_is_memory(_fd)) return NULL;
        if(psize) *psize = fd->len;
        return fd->memory;
    }
#endif
