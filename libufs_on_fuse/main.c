#include <fuse.h>
#include "libufs.h"

typedef struct _fuse_config {
    char* path;
    unsigned long long size;
} _fuse_config;
static _fuse_config _conf;
static ufs_t* _ufs = NULL;
static ufs_vfs_t* vfs = NULL; 

static void _make_context(ufs_context_t* ctx) {
    struct fuse_context* fctx = fuse_get_context();
    ctx->ufs = _ufs;
    ctx->uid = (int32_t)fctx->uid;
    ctx->gid = (int32_t)fctx->gid;
    ctx->umask = (uint16_t)fctx->umask;
}

static int _fuse_getattr(const char* path, struct stat* out) {
    int ec;
    ufs_stat_t stat;
    ufs_context_t ctx; _make_context(&ctx);
    ec = ufs_stat(&ctx, path, &stat);
    if(ec) return -ufs_uniform_error(ec);

    out->st_dev = 0;
    out->st_ino = stat.st_ino;
    out->st_mode = stat.st_mode;
    out->st_nlink = stat.st_nlink;
    out->st_uid = stat.st_uid;
    out->st_gid = stat.st_gid;
    out->st_rdev = 0;
    out->st_size = stat.st_size;
    out->st_blksize = stat.st_blksize;
    out->st_blocks = stat.st_blocks;

    out->st_atim.tv_sec = stat.st_atim / 1000;
    out->st_atim.tv_nsec = stat.st_atim % 1000 * 1000000;
    out->st_ctim.tv_sec = stat.st_ctim / 1000;
    out->st_ctim.tv_nsec = stat.st_ctim % 1000 * 1000000;
    out->st_mtim.tv_sec = stat.st_mtim / 1000;
    out->st_mtim.tv_nsec = stat.st_mtim % 1000 * 1000000;
    return 0;
}
static int _fuse_readlink(const char* path, char* dest, size_t len) {
    ufs_context_t ctx;
    char* resolved;
    size_t rlen;
    int ret;

    _make_context(&ctx);
    ret = ufs_readlink(&ctx, path, &resolved);
    if(ret) return -ufs_uniform_error(ret);
    rlen = strlen(resolved);
    if(rlen > len) { ufs_free(resolved); return -EOVERFLOW; }
    memcpy(dest, resolved, rlen);
    ufs_free(resolved);
    return 0;
}
static int _fuse_mkdir(const char* path, mode_t mode) {
    ufs_context_t ctx; _make_context(&ctx);
    return -ufs_uniform_error(ufs_mkdir(&ctx, path, (uint16_t)(mode & 0777)));
}
static int _fuse_unlink(const char* path) {
    ufs_context_t ctx; _make_context(&ctx);
    return -ufs_uniform_error(ufs_unlink(&ctx, path));
}
static int _fuse_rmdir(const char* path) {
    ufs_context_t ctx; _make_context(&ctx);
    return -ufs_uniform_error(ufs_rmdir(&ctx, path));
}
static int _fuse_symlink(const char* source, const char* target) {
    ufs_context_t ctx; _make_context(&ctx);
    return -ufs_uniform_error(ufs_symlink(&ctx, target, source));
}
static int _fuse_link(const char* source, const char* target) {
    ufs_context_t ctx; _make_context(&ctx);
    return -ufs_uniform_error(ufs_link(&ctx, target, source));
}
static int _fuse_chmod(const char* path, mode_t mode) {
    ufs_context_t ctx; _make_context(&ctx);
    return -ufs_uniform_error(ufs_chmod(&ctx, path, (uint16_t)(mode & 0777)));
}
static int _fuse_chown(const char* path, uid_t uid, gid_t gid) {
    ufs_context_t ctx; _make_context(&ctx);
    return -ufs_uniform_error(ufs_chown(&ctx, path, (int32_t)uid, (int32_t)gid));
}
static int _fuse_truncate(const char* path, off_t off) {
    ufs_context_t ctx; _make_context(&ctx);
    if(off < 0) return -EINVAL;
    return -ufs_uniform_error(ufs_truncate(&ctx, path, (uint64_t)off));
}
static int _fuse_open(const char* path, struct fuse_file_info* fi) {
    int ec;
    unsigned long mode = 0;
    ufs_file_t* file;
    ufs_context_t ctx; _make_context(&ctx);
    if(fi->flags & O_RDONLY) mode |= UFS_O_RDONLY;
    if(fi->flags & O_WRONLY) mode |= UFS_O_WRONLY;
    if(fi->flags & O_RDWR) mode |= UFS_O_RDWR;
    if(fi->flags & O_CREAT) mode |= UFS_O_CREAT;
    if(fi->flags & O_APPEND) mode |= UFS_O_APPEND;
    if(fi->flags & O_TRUNC) mode |= UFS_O_TRUNC;
    ec = ufs_open(&ctx, &file, path, mode, 0664);
    if(ec) return -ufs_uniform_error(ec);
    fi->fh = (uint64_t)file;
    return 0;
}
static int _fuse_read(const char* path, char* buf, size_t size, off_t off, struct fuse_file_info* fi) {
    size_t read;
    int ec;
    ufs_context_t ctx; _make_context(&ctx);
    (void)path;
    if(off < 0) return -EINVAL;
    ec = ufs_pread((ufs_file_t*)fi->fh, buf, size, (uint64_t)off, &read);
    if(ec) return -ufs_uniform_error(ec);
    return (int)read;
}
static int _fuse_write(const char* path, const char* buf, size_t size, off_t off, struct fuse_file_info* fi) {
    size_t writen;
    int ec;
    ufs_context_t ctx; _make_context(&ctx);
    (void)path;
    if(off < 0) return -EINVAL;
    ec = ufs_pwrite((ufs_file_t*)fi->fh, buf, size, (uint64_t)off, &writen);
    if(ec) return -ufs_uniform_error(ec);
    return (int)writen;
}
static int _fuse_statfs(const char* path, struct statvfs* vfs) {
    int ec;
    ufs_statvfs_t _vfs;
    ec = ufs_statvfs(_ufs, &_vfs);
    (void)path;
    if(ec) return -ufs_uniform_error(ec);

    vfs->f_flag = 0;
    vfs->f_frsize = 0;
    vfs->f_fsid = 0;

    vfs->f_bsize = _vfs.f_bsize;
    vfs->f_namemax = _vfs.f_namemax;
    vfs->f_blocks = _vfs.f_blocks;
    vfs->f_bfree = _vfs.f_bfree;
    vfs->f_bavail = _vfs.f_bavail;
    vfs->f_files = _vfs.f_files;
    vfs->f_ffree = _vfs.f_ffree;
    vfs->f_favail = _vfs.f_favail;
    return 0;
}
static int _fuse_release(const char* path, struct fuse_file_info* fi) {
    (void)path;
    return -ufs_uniform_error(ufs_close((ufs_file_t*)fi->fh));
}
static int _fuse_fsync(const char* path, int mode, struct fuse_file_info* fi) {
    (void)path; (void)mode;
    return -ufs_uniform_error(ufs_fsync((ufs_file_t*)fi->fh, 0));
}
static int _fuse_opendir(const char* path, struct fuse_file_info* fi) {
    ufs_dir_t* dir;
    int ec;
    ufs_context_t ctx; _make_context(&ctx);
    ec = ufs_opendir(&ctx, &dir, path);
    if(ec) return -ufs_uniform_error(ec);
    fi->fh = (uint64_t)dir;
    return 0;
}
static int _fuse_readdir(const char* path, void* buf, fuse_fill_dir_t filler, off_t offset, struct fuse_file_info* fi) {
    ufs_dirent_t dirent;
    int ec;
    (void)path; (void)offset;
    for(;;) {
        ec = ufs_readdir((ufs_dir_t*)fi->fh, &dirent);
        if(ec) {
            if(ec == UFS_ENOENT) break;
            else return -ufs_uniform_error(ec);
        }
        if(filler(buf, dirent.d_name, NULL, 0)) break;
    }
    return 0;
}
static int _fuse_releasedir(const char* path, struct fuse_file_info* fi) {
    (void)path;
    return -ufs_uniform_error(ufs_closedir((ufs_dir_t*)fi->fh));
}
static int _fuse_access(const char* path, int mode) {
    ufs_context_t ctx; _make_context(&ctx);
    return -ufs_uniform_error(ufs_access(&ctx, path, mode));
}
static int _fuse_create(const char* path, mode_t mode, struct fuse_file_info* fi) {
    int ec;
    ufs_file_t* file;
    ufs_context_t ctx; _make_context(&ctx);
    ec = ufs_creat(&ctx, &file, path, (uint16_t)(mode & 0777));
    if(ec) return -ufs_uniform_error(ec);
    fi->fh = (uint64_t)file;
    return 0;
}
static int _fuse_utime(const char* path, struct utimbuf* buf) {
    int64_t atime, mtime;
    ufs_context_t ctx; _make_context(&ctx);
    atime = (int64_t)buf->actime * 1000;
    mtime = (int64_t)buf->modtime * 1000;
    return -ufs_uniform_error(ufs_utimes(&ctx, path, NULL, &atime, &mtime));
}

static struct fuse_operations _fuse_ops = {
    .getattr = _fuse_getattr,
    .readlink = _fuse_readlink,
    .mkdir = _fuse_mkdir,
    .mknod = NULL,
    .unlink = _fuse_unlink,
    .rmdir = _fuse_rmdir,
    .symlink = _fuse_symlink,
    .rename = NULL,
    .link = _fuse_link,
    .chmod = _fuse_chmod,
    .chown = _fuse_chown,
    .open = _fuse_open,
    .read = _fuse_read,
    .write = _fuse_write,
    .statfs = _fuse_statfs,
    .flush = NULL,
    .release = _fuse_release,
    .fsync = _fuse_fsync,
    .setxattr = NULL,
    .getxattr = NULL,
    .listxattr = NULL,
    .removexattr = NULL,
    .opendir = _fuse_opendir,
    .readdir = _fuse_readdir,
    .releasedir = _fuse_releasedir,
    .fsyncdir = NULL,
    .init = NULL,
    .destroy = NULL,
    .access = _fuse_access,
    .create = _fuse_create,
    .utime = _fuse_utime,
    .truncate = _fuse_truncate
};


#include <stddef.h>
#include <string.h>
int main(int argc, char* argv[]) {
    static const struct fuse_opt _opts[] = {
        { "--path=%s", offsetof(_fuse_config, path), 0 },
        { "--size=%llu", offsetof(_fuse_config, size), 0 },
        FUSE_OPT_END
    };

    memset(&_conf, 0, sizeof(_conf));
    struct fuse_args args = FUSE_ARGS_INIT(argc, argv);
    if(fuse_opt_parse(&args, &_conf, _opts, NULL) == -1) return -1;

    printf("path: %s\n", _conf.path);
    printf("size: %llu\n", _conf.size);

    int ec = ufs_vfs_open_file(&vfs, _conf.path);
    if(ec) { puts(strerror(ec)); return 1; }
    if(_conf.size != 0) ec = ufs_new_format(&_ufs, vfs, _conf.size);
    else ec = ufs_new(&_ufs, vfs);
    if(ec) { puts(strerror(ec)); return 1; }

    int ret = fuse_main(args.argc, args.argv, &_fuse_ops, NULL);
    ufs_destroy(_ufs);
    vfs->close(vfs);
    fuse_opt_free_args(&args);
    free(_conf.path);
    return ret;
}