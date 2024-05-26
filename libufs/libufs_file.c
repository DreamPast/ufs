#include "libufs_internel.h"

static int _checkuid(int32_t uid, int32_t file_uid) {
    return uid == 0 || file_uid == 0 || uid == file_uid;
}
static int _checkgid(int32_t gid, int32_t file_gid) {
    return gid == 0 || file_gid == 0 || gid == file_gid;
}
static int _check_perm(int32_t uid, int32_t gid, const ufs_inode_t* inode) {
    int r = 0;
    if(_checkuid(uid, inode->uid)) r |= (inode->mode & UFS_S_IRWXU) >> 6;
    if(_checkgid(gid, inode->gid)) r |= (inode->mode & UFS_S_IRWXG) >> 3;
    return r | (inode->mode & UFS_S_IRWXO);
}

typedef struct _dirent_t {
    char name[UFS_NAME_MAX + 1];
    uint64_t inum;
} _dirent_t;

static int _readlink(ufs_context_t* ufs_restrict context, ufs_minode_t* minode, char** presolved) {
    int ec;
    char* resolved;
    size_t read;
    ufs_transcation_t transcation;

    resolved = ul_reinterpret_cast(char*, ufs_malloc(UFS_BLOCK_SIZE));
    if(ufs_unlikely(resolved == NULL)) return UFS_ENOMEM;

    ufs_assert(UFS_S_ISLNK(minode->inode.mode));
    ufs_transcation_init(&transcation, &context->ufs->jornal);

    ec = ufs_minode_pread(minode, &transcation, resolved, UFS_BLOCK_SIZE, 0, &read);
    if(ufs_unlikely(ec)) goto do_return;
    if(ufs_unlikely(read != UFS_BLOCK_SIZE)) { ec = UFS_ESTALE; goto do_return; }
    *presolved = resolved; resolved = NULL;

do_return:
    ufs_free(resolved);
    ufs_transcation_deinit(&transcation);
    return ec;
}

static int _dirent_empty(const _dirent_t* dirent) {
    int i;
    for(i = 0; i < UFS_NAME_MAX; ++i)
        if(dirent->name[i]) return 0;
    return 1;
}
static int _search_dir(ufs_minode_t* minode, const char* target, uint64_t *pinum, size_t len) {
    int ec;
    ufs_transcation_t transcation;
    _dirent_t dirent;
    uint64_t off = 0;
    size_t read;

    ufs_transcation_init(&transcation, &minode->ufs->jornal);
    ufs_assert(len <= UFS_NAME_MAX);
    for(;;) {
        ec = ufs_minode_pread(minode, &transcation, &dirent, sizeof(dirent), off, &read);
        if(ufs_unlikely(ec)) goto do_return;
        if(read != sizeof(dirent)) break;
        if(memcmp(target, dirent.name, len) == 0) {
            *pinum = ul_trans_u64_le(dirent.inum);
            goto do_return;
        }
        off += sizeof(dirent);
    }
    ec = UFS_ENOENT;

do_return:
    ufs_transcation_deinit(&transcation);
    return ec;
}

static int _add_to_dir(ufs_context_t* context, ufs_minode_t* minode, const char* fname, size_t flen, uint64_t inum) {
    int ec;
    ufs_transcation_t transcation;
    _dirent_t dirent;
    uint64_t off = 0;
    size_t read;

    ufs_transcation_init(&transcation, &context->ufs->jornal);
    for(;;) {
        ec = ufs_minode_pread(minode, &transcation, &dirent, sizeof(dirent), off, &read);
        if(ufs_unlikely(ec)) break;
        if(read != sizeof(dirent)) { ec = ENOSPC; break; }
        if(_dirent_empty(&dirent)) break;
        off += sizeof(dirent);
    }
    if(ufs_likely(ec == 0)) {
        memcpy(dirent.name, fname, flen);
        memset(dirent.name + flen, 0, UFS_NAME_MAX + 1 - flen);
        dirent.inum = ul_trans_u64_le(inum);
        ec = ufs_minode_pwrite(minode, &transcation, &dirent, sizeof(dirent), minode->inode.size, &flen);
        if(ufs_unlikely(ec == 0 && flen != sizeof(dirent))) ec = UFS_ENOSPC;
        if(ufs_likely(ec == 0)) ec = ufs_transcation_commit_all(&transcation);
    }
    ufs_transcation_deinit(&transcation);
    return ec;
}

// 是否是分隔符
#define _is_slash(c) ((c) == '/' || (c) == '\\')
// 返回NULL表示文件名超长
static const char* _find_next_part(const char* path) {
    int i;
    for(i = 0; i < UFS_NAME_MAX && path[i]; ++i)
        if(_is_slash(path[i])) return path + i;
    if(path[i]) return NULL;
    else return path + i;
}

#define UFS_SYMBOL_LOOP_LIMIT 8
// UFS_ENAMETOOLONG / UFS_ENOTDIR / UFS_ELOOP / UFS_ENOMEM / UFS_ENOENT / UFS_EACCESS
static int __ppath2inum(ufs_context_t* ufs_restrict context, const char* ufs_restrict path, ufs_minode_t** pminode, const char** ufs_restrict pfname, int sloop) {
    int ec;
    ufs_minode_t* minode;
    const char* path2;

    ec = ufs_fileset_open(&context->ufs->fileset, UFS_INUM_ROOT, &minode);
    if(ufs_unlikely(ec)) return ec;

    for(;;) {
        while(_is_slash(*path)) ++path;
        path2 = _find_next_part(path);
        if(path2[0] == 0) { if(pfname) *pfname = path; break; }
        if(!path2) {
            ufs_fileset_close(&context->ufs->fileset, minode->inum);
            return UFS_ENAMETOOLONG;
        }
        switch(minode->inode.mode & UFS_S_IFMT) {
        case UFS_S_IFREG:
            ufs_fileset_close(&context->ufs->fileset, minode->inum);
            return UFS_ENOTDIR;
        case UFS_S_IFLNK:
            if(sloop >= UFS_SYMBOL_LOOP_LIMIT) {
                ufs_fileset_close(&context->ufs->fileset, minode->inum);
                return UFS_ELOOP;
            } else {
                char* resolved;
                ufs_minode_lock(minode);
                ec = _readlink(context, minode, &resolved);
                ufs_minode_unlock(minode);
                if(ufs_unlikely(ec)) { ufs_fileset_close(&context->ufs->fileset, minode->inum); return ec; }
                ufs_fileset_close(&context->ufs->fileset, minode->inum);
                ec = __ppath2inum(context, resolved, pminode, NULL, sloop + 1);
                ufs_free(resolved);
                if(ufs_unlikely(ec)) return ec;
            }
            break;
        case UFS_S_IFDIR:
            do {
                uint64_t inum;
                if(!(_check_perm(context->uid, context->gid, &minode->inode) & UFS_X_OK)) {
                    ufs_fileset_close(&context->ufs->fileset, minode->inum);
                    return UFS_EACCESS;
                }
                ufs_minode_lock(minode);
                ec = _search_dir(minode, path, &inum, ul_static_cast(size_t, path2 - path));
                ufs_minode_unlock(minode);
                ufs_fileset_close(&context->ufs->fileset, minode->inum);
                if(ufs_unlikely(ec)) return ec;
                ec = ufs_fileset_open(&context->ufs->fileset, inum, &minode);
                if(ufs_unlikely(ec)) return ec;
            } while(0);
            break;
        default:
            ufs_fileset_close(&context->ufs->fileset, minode->inum);
        }
        path = path2;
    }

    *pminode = minode;
    return 0;
}
static int _ppath2inum(ufs_context_t* ufs_restrict context, const char* ufs_restrict path, ufs_minode_t** pminode, const char** ufs_restrict pfname) {
    return __ppath2inum(context, path, pminode, pfname, 0);
}

// 不对文件类型做校验
// UFS_EACCESS / UFS_EEXIST / UFS_ENAMETOOLONG / UFS_ENOTDIR / UFS_ELOOP / UFS_ENOMEM / UFS_ENOENT
static int _open(ufs_context_t* context, ufs_minode_t** pminode, const char* path, unsigned long flag, uint16_t mask) {
    ufs_minode_t* file_minode = NULL;
    uint64_t inum = 0;
    int ec;
    ufs_minode_t* ppath_minode;
    const char* fname;

    // 检查文件是否存在，不存在则根据flag尝试创建
    ec = _ppath2inum(context, path, &ppath_minode, &fname);
    if(ufs_unlikely(ec)) return ec;
    if(fname[0] == 0) { // 根目录需要特殊处理
        if((flag & UFS_O_CREAT) && (flag & UFS_O_EXCL)) {
            ufs_fileset_close(&context->ufs->fileset, ppath_minode->inum);
            return UFS_EEXIST;
        } else {
            *pminode = ppath_minode; return 0;
        }
    }
    ufs_minode_lock(ppath_minode);
    if(_check_perm(context->uid, context->gid, &ppath_minode->inode) & UFS_X_OK)
        ec = _search_dir(ppath_minode, fname, &inum, strlen(fname));
    else ec = UFS_EACCESS;
    if(ec == 0) {
        if(flag & UFS_O_CREAT) {
            flag &= ul_reinterpret_cast(unsigned long, ~UFS_O_CREAT); // 文件存在则擦除创建标记
            if(flag & UFS_O_EXCL) ec = UFS_EEXIST;
        }
    } else {
        if(ec == UFS_ENOENT) {
            if(flag & UFS_O_CREAT) {
                if(_check_perm(context->uid, context->gid, &ppath_minode->inode) & UFS_W_OK) ec = 0; // 可以创建文件，清空错误
                else ec = UFS_EACCESS;
            }
        }
    }
    if((ec == 0) && (flag & UFS_O_CREAT)) {
        ufs_inode_create_t creat;
        creat.uid = context->uid;
        creat.gid = context->gid;
        creat.mode = mask | UFS_S_IFREG;
        ec = ufs_fileset_creat(&context->ufs->fileset, &inum, &file_minode, &creat);
        if(ufs_likely(ec == 0)) ec = _add_to_dir(context, ppath_minode, fname, strlen(fname), inum);
    }
    ufs_minode_unlock(ppath_minode);
    ufs_fileset_close(&context->ufs->fileset, ppath_minode->inum);
    if(ufs_unlikely(ec)) {
        if(file_minode) ufs_fileset_close(&context->ufs->fileset, file_minode->inum);
        return ec;
    }

    ufs_assert(inum != 0); // 此时inum必定有值
    if(file_minode == NULL) {
        ec = ufs_fileset_open(&context->ufs->fileset, inum, &file_minode);
        if(ufs_unlikely(ec)) return ec;
    }
    *pminode = file_minode;
    return 0;
}



typedef struct ufs_file_t {
    ufs_minode_t* minode;
    uint64_t off;
    int flag;
    ulatomic_spinlock_t lock;
    int32_t uid;
    int32_t gid;
} ufs_file_t;
#define _OPEN_APPEND 8
static void _file_lock(ufs_file_t* file) { ulatomic_spinlock_lock(&file->lock); }
static void _file_unlock(ufs_file_t* file) { ulatomic_spinlock_unlock(&file->lock); }

UFS_API int ufs_open(ufs_context_t* context, ufs_file_t** pfile, const char* path, unsigned long flag, uint16_t mask) {
    ufs_file_t* file;
    int ec;
    ufs_minode_t* minode;
    int stop = 0;

    if(ufs_unlikely(context == NULL || context->ufs == NULL)) return UFS_EINVAL;
    if(ufs_unlikely(path == NULL || path[0] == 0)) return UFS_EINVAL;
    if(ufs_unlikely((flag & UFS_O_RDONLY) && (flag & UFS_O_WRONLY))) return UFS_EINVAL;

    mask &= context->umask & 0777u;
    ec = _open(context, &minode, path, flag, mask);
    if(ufs_unlikely(ec)) return ec;
    if(!UFS_S_ISREG(minode->inode.mode)) {
        if(UFS_S_ISDIR(minode->inode.mode)) {
            ufs_fileset_close(&context->ufs->fileset, minode->inum);
            return UFS_EISDIR;
        } else {
            char* resolved;
            ufs_minode_lock(minode);
            ec = _readlink(context, minode, &resolved);
            ufs_minode_unlock(minode);
            ufs_fileset_close(&context->ufs->fileset, minode->inum);
            if(ufs_unlikely(ec)) return ec;
            if(stop++ >= UFS_SYMBOL_LOOP_LIMIT) return ELOOP;
            ec = _open(context, &minode, path, flag, mask);
            ufs_free(resolved);
            if(ufs_unlikely(ec)) return ec;
        }
    }

    file = ul_reinterpret_cast(ufs_file_t*, ufs_malloc(sizeof(ufs_file_t)));
    if(ufs_unlikely(file == NULL)) { ufs_fileset_close(&context->ufs->fileset, minode->inum); return UFS_ENOMEM; }
    file->minode = minode;
    file->off = 0;
    file->flag = 0;
    file->uid = context->uid;
    file->gid = context->gid;
    ulatomic_spinlock_init(&file->lock);
    if(flag & UFS_O_RDONLY) file->flag |= UFS_R_OK;
    if(flag & UFS_O_WRONLY) file->flag |= UFS_W_OK;
    if(flag & UFS_O_RDWR) file->flag |= UFS_R_OK | UFS_W_OK;

    do {
        int access = _check_perm(context->uid, context->gid, &minode->inode);
        ufs_minode_lock(minode);
        if((file->flag & UFS_R_OK) && !(access & UFS_R_OK)) ec = EACCES;
        if((file->flag & UFS_W_OK) && !(access & UFS_W_OK)) ec = EACCES;
        ufs_minode_unlock(minode);
        if(ufs_unlikely(ec)) { ufs_fileset_close(&context->ufs->fileset, minode->inum); return ec; }
    } while(0);

    if(flag & UFS_O_APPEND) file->flag |= _OPEN_APPEND;
    if(flag & UFS_O_TRUNC) {
        ufs_minode_lock(minode);
        if(_check_perm(context->uid, context->gid, &minode->inode) & UFS_W_OK) ec = ufs_minode_resize(minode, 0);
        else ec = UFS_EACCESS;
        ufs_minode_unlock(minode);
    }
    *pfile = file;
    return 0;
}

UFS_API int ufs_creat(ufs_context_t* context, ufs_file_t** pfile, const char* path, uint16_t mask) {
    return ufs_open(context, pfile, path, UFS_O_RDWR | UFS_O_CREAT, mask);
}

UFS_API int ufs_close(ufs_file_t* file) {
    int ec;
    if(ufs_unlikely(file == NULL)) return UFS_EBADF;
    ec = ufs_fileset_close(&file->minode->ufs->fileset, file->minode->inum);
    ufs_free(file);
    return ec;
}

UFS_API int ufs_read(ufs_file_t* file, void* buf, size_t len, size_t* pread) {
    size_t read;
    int ec;
    pread = pread ? pread : &read;
    if(ufs_unlikely(file == NULL)) return UFS_EBADF;
    if(ufs_unlikely(buf == NULL)) return UFS_EINVAL;
    _file_lock(file);
    ufs_minode_lock(file->minode);
    if(!(file->flag & UFS_R_OK) || !(_check_perm(file->uid, file->gid, &file->minode->inode) & UFS_R_OK)) {
        _file_unlock(file); ufs_minode_unlock(file->minode); return UFS_EBADF;
    }
    ec = ufs_minode_pread(file->minode, NULL, buf, len, file->flag & _OPEN_APPEND ? file->minode->inode.size : file->off, &read);
    ufs_minode_unlock(file->minode);
    if(ufs_unlikely(ec)) { _file_unlock(file); return ec; }
    if(file->flag & _OPEN_APPEND) file->off = file->minode->inode.size;
    else file->off += len;
    _file_unlock(file);
    if(pread) *pread = read;
    return 0;
}

UFS_API int ufs_write(ufs_file_t* file, void* buf, size_t len, size_t* pwriten) {
    size_t writen;
    int ec;
    pwriten = pwriten ? pwriten : &writen;
    if(ufs_unlikely(file == NULL)) return UFS_EBADF;
    if(ufs_unlikely(buf == NULL)) return UFS_EINVAL;
    _file_lock(file);
    ufs_minode_lock(file->minode);
    if(!(file->flag & UFS_W_OK) || !(_check_perm(file->uid, file->gid, &file->minode->inode) & UFS_W_OK)) {
        _file_unlock(file); ufs_minode_unlock(file->minode); return UFS_EBADF;
    }
    ec = ufs_minode_pwrite(file->minode, NULL, buf, len, file->flag & _OPEN_APPEND ? file->minode->inode.size : file->off, &writen);
    ufs_minode_unlock(file->minode);
    if(ufs_unlikely(ec)) { _file_unlock(file); return ec; }
    if(file->flag & _OPEN_APPEND) file->off = file->minode->inode.size;
    else file->off += len;
    _file_unlock(file);
    if(pwriten) *pwriten = writen;
    return 0;
}

UFS_API int ufs_pread(ufs_file_t* file, void* buf, size_t len, uint64_t off, size_t* pread) {
    size_t read;
    int ec;
    pread = pread ? pread : &read;
    if(ufs_unlikely(file == NULL)) return UFS_EBADF;
    if(ufs_unlikely(buf == NULL)) return UFS_EINVAL;
    _file_lock(file);
    ufs_minode_lock(file->minode);
    if(!(file->flag & UFS_R_OK) || !(_check_perm(file->uid, file->gid, &file->minode->inode) & UFS_R_OK)) {
        _file_unlock(file); ufs_minode_unlock(file->minode); return UFS_EBADF;
    }
    ec = ufs_minode_pread(file->minode, NULL, buf, len, off, &read);
    ufs_minode_unlock(file->minode);
    _file_unlock(file);
    if(ufs_unlikely(ec)) return ec;
    if(pread) *pread = read;
    return 0;
}

UFS_API int ufs_pwrite(ufs_file_t* file, void* buf, size_t len, uint64_t off, size_t* pwriten) {
    size_t writen;
    int ec;
    pwriten = pwriten ? pwriten : &writen;
    if(ufs_unlikely(file == NULL)) return UFS_EBADF;
    if(ufs_unlikely(buf == NULL)) return UFS_EINVAL;
    _file_lock(file);
    ufs_minode_lock(file->minode);
    if(!(file->flag & UFS_W_OK) || !(_check_perm(file->uid, file->gid, &file->minode->inode) & UFS_W_OK)) {
        _file_unlock(file); ufs_minode_unlock(file->minode); return UFS_EBADF;
    }
    ec = ufs_minode_pwrite(file->minode, NULL, buf, len, off, &writen);
    ufs_minode_unlock(file->minode);
    if(ufs_unlikely(ec)) { _file_unlock(file); return ec; }
    file->off += writen;
    _file_unlock(file);
    if(pwriten) *pwriten = writen;
    return 0;
}

UFS_API int ufs_seek(ufs_file_t* file, int64_t off, int wherence, uint64_t* poff) {
    int ec = 0;
    uint64_t new_off;
    if(ufs_unlikely(file == NULL)) return UFS_EBADF;

    _file_lock(file);
    ufs_minode_lock(file->minode);
    switch(wherence) {
    case UFS_SEEK_SET:
        if(off < 0) ec = UFS_EINVAL;
        else file->off = ul_static_cast(uint64_t, off);
        break;
    case UFS_SEEK_CUR:
        new_off = ul_static_cast(uint64_t, off) + file->off;
        if(ufs_unlikely(off >= 0))
            if(ufs_unlikely(file->off < new_off)) ec = EINVAL;
            else file->off = new_off;
        else
            if(ufs_likely(new_off < file->off)) file->off = new_off;
            else ec = EINVAL;
        break;
    case UFS_SEEK_END:
        new_off = ul_static_cast(uint64_t, off) + file->minode->inode.size;
        if(ufs_unlikely(ul_static_cast(int64_t, new_off) < 0)) ec = EINVAL;
        else file->off = new_off;
        break;
    default:
        ec = UFS_EINVAL;
        break;
    }
    if(ufs_likely(ec == 0) && poff) *poff = file->off;
    ufs_minode_unlock(file->minode);
    _file_unlock(file);
    return ec;
}

UFS_API int ufs_tell(ufs_file_t* file, uint64_t* poff) {
    if(ufs_unlikely(file == NULL)) return UFS_EBADF;
    if(ufs_unlikely(poff == NULL)) return UFS_EINVAL;
    _file_lock(file);
    ufs_minode_lock(file->minode);
    *poff = file->off;
    ufs_minode_unlock(file->minode);
    _file_unlock(file);
    return 0;
}



typedef struct ufs_dir_t {
    ufs_minode_t* minode;
    uint64_t off;
    int32_t uid;
    int32_t gid;
    ulatomic_spinlock_t lock;
} ufs_dir_t;
static void _dir_lock(ufs_dir_t* dir) { ulatomic_spinlock_lock(&dir->lock); }
static void _dir_unlock(ufs_dir_t* dir) { ulatomic_spinlock_unlock(&dir->lock); }

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
UFS_API int ufs_opendir(ufs_context_t* context, ufs_dir_t** pdir, const char* path) {
    ufs_minode_t* minode;
    int ec;
    ufs_dir_t* dir;

    if(ufs_unlikely(context == NULL || context->ufs == NULL)) return UFS_EINVAL;
    if(ufs_unlikely(path == NULL || path[0] == 0)) return UFS_EINVAL;
    
    ec = _open(context, &minode, path, 0, 0664);
    if(ufs_unlikely(ec)) return ec;
    if(!UFS_S_ISDIR(minode->inode.mode)) {
        ufs_fileset_close(&context->ufs->fileset, minode->inum); return UFS_ENOTDIR;
    }
    if(!(_check_perm(context->uid, context->gid, &minode->inode) & UFS_R_OK)) {
        ufs_fileset_close(&context->ufs->fileset, minode->inum); return UFS_EACCESS;
    }

    dir = ul_reinterpret_cast(ufs_dir_t*, ufs_malloc(sizeof(ufs_dir_t)));
    if(ufs_unlikely(dir == NULL)) {
        ufs_fileset_close(&context->ufs->fileset, minode->inum); return UFS_ENOMEM;
    }
    dir->minode = minode;
    dir->off = 0;
    dir->uid = context->uid;
    dir->gid = context->gid;
    ulatomic_spinlock_init(&dir->lock);
    *pdir = dir;
    return 0;
}

/**
 * 读取目录
 * 
 * 错误：
 *   [UFS_EBADF] dir非法
 *   [UFS_EINVAL] dirent为NULL
 *   [UFS_ENOENT] 目录已读取完毕
 *   [UFS_EACCESS] 对目录不具备读入权限
*/
UFS_API int ufs_readdir(ufs_dir_t* dir, ufs_dirent_t* dirent) {
    int ec;
    uint64_t off;
    size_t read;
    _dirent_t _dirent;
    ufs_transcation_t transcation;

    if(ufs_unlikely(dir == NULL)) return UFS_EBADF;
    if(ufs_unlikely(dirent == NULL)) return UFS_EINVAL;
    
    _dir_lock(dir);
    ufs_minode_lock(dir->minode);
    ufs_transcation_init(&transcation, &dir->minode->ufs->jornal);

    off = dir->off;
    if(!(_check_perm(dir->uid, dir->gid, &dir->minode->inode) & UFS_R_OK)) {
        ec = UFS_EACCESS; goto do_return;
    }
    if(ufs_unlikely(off >= dir->minode->inode.size)) {
        ec = UFS_ENOENT; goto do_return;
    }

    for(;;) {
        ec = ufs_minode_pread(dir->minode, &transcation, &_dirent, sizeof(_dirent), off, &read);
        if(ufs_unlikely(ec)) goto do_return;
        if(read != sizeof(_dirent)) { ec = UFS_ENOENT; goto do_return; }
        off += sizeof(_dirent);
        if(!_dirent_empty(&_dirent)) break;
    }
    dir->off = off;
    memcpy(dirent->d_name, _dirent.name, UFS_NAME_MAX + 1);
    dirent->d_ino = ul_trans_u64_le(_dirent.inum);
    dirent->d_off = off;

do_return:
    ufs_transcation_deinit(&transcation);
    ufs_minode_unlock(dir->minode);
    _dir_unlock(dir);
    return ec;
}
/**
 * 定位目录
 * 
 * 错误：
 *   [UFS_EBADF] dir非法
*/
UFS_API int ufs_seekdir(ufs_dir_t* dir, uint64_t off) {
    if(ufs_unlikely(dir == NULL)) return UFS_EBADF;
    off -= off % sizeof(_dirent_t);
    _dir_lock(dir);
    dir->off = off;
    _dir_unlock(dir);
    return 0;
}
/**
 * 获得目录定位
 * 
 * 错误：
 *   [UFS_EBADF] dir非法
 *   [UFS_EINVAL] poff非法
*/
UFS_API int ufs_telldir(ufs_dir_t* dir, uint64_t* poff) {
    if(ufs_unlikely(dir == NULL)) return UFS_EBADF;
    if(ufs_unlikely(poff == NULL)) return UFS_EINVAL;
    _dir_lock(dir);
    *poff = dir->off;
    _dir_unlock(dir);
    return 0;
}
/**
 * 定位目录到初始状态
*/
UFS_API int ufs_rewinddir(ufs_dir_t* dir) {
    return ufs_seekdir(dir, 0);
}
/**
 * 关闭目录
 * 
 * 错误：
 *   [UFS_EBADF] dir非法
*/
UFS_API int ufs_closedir(ufs_dir_t* dir) {
    int ec;
    if(ufs_unlikely(dir == NULL)) return UFS_EBADF;
    ec = ufs_fileset_close(&dir->minode->ufs->fileset, dir->minode->inum);
    ufs_free(dir);
    return ec;
}

UFS_HIDDEN void ufs_file_debug(const ufs_file_t* file, FILE* fp) {
    fprintf(fp, "file [%p]\n", ufs_const_cast(void*, file));

    fprintf(fp, "\tminode: \n");
    _ufs_inode_debug(&file->minode->inode, fp, 1);
    fprintf(fp, "\t\tufs: [%p]\n", ufs_const_cast(void*, file->minode->ufs));
    fprintf(fp, "\t\tinum: [%" PRIu64 "]\n", file->minode->inum);

    fprintf(fp, "\toffset: %" PRIu64 "\n", file->off);
    fprintf(fp, "\tflag: 0%o\n", file->flag);
}
