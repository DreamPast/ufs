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
static int _shrink_dir(ufs_minode_t* minode) {
    int ec;
    ufs_transcation_t transcation;
    uint64_t off = minode->inode.size;
    _dirent_t dirent;
    size_t read;
    ufs_transcation_init(&transcation, &minode->ufs->jornal);
    for(;;) {
        ec = ufs_minode_pread(minode, &transcation, &dirent, sizeof(dirent), off - sizeof(dirent), &read);
        if(ufs_unlikely(ec)) goto do_return;
        if(read != sizeof(dirent)) break;
        if(!_dirent_empty(&dirent)) break;
        off -= sizeof(dirent);
    }

do_return:
    ufs_minode_resize(minode, off);
    ufs_transcation_deinit(&transcation);
    return 0;
}
static int _search_dir(ufs_minode_t* minode, const char* target, uint64_t *pinum, size_t len, uint64_t* poff) {
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
        if(memcmp(target, dirent.name, len) == 0 && dirent.name[len] == 0) {
            *pinum = ul_trans_u64_le(dirent.inum);
            goto do_return;
        }
        off += sizeof(dirent);
    }
    ec = UFS_ENOENT;

do_return:
    if(poff) *poff = off;
    ufs_transcation_deinit(&transcation);
    return ec;
}
static int _delete_from_dir(ufs_context_t* context, ufs_minode_t* minode, uint64_t off) {
    int ec;
    ufs_transcation_t transcation;
    static const _dirent_t dirent = { { 0 }, 0 };
    size_t writen;

    ufs_transcation_init(&transcation, &context->ufs->jornal);
    ec = ufs_minode_pwrite(minode, &transcation, &dirent, sizeof(dirent), off, &writen);
    if(ufs_unlikely(ec == 0 && writen != sizeof(dirent))) ec = UFS_ENOSPC;
    if(ufs_likely(ec == 0)) ec = ufs_transcation_commit_all(&transcation);
    ufs_transcation_deinit(&transcation);

    if(ufs_unlikely(ec)) return ec;
    return _shrink_dir(minode);
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
        if(read == 0) break;
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
                ec = __ppath2inum(context, resolved, &minode, NULL, sloop + 1);
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
                ec = _search_dir(minode, path, &inum, ul_static_cast(size_t, path2 - path), NULL);
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

#define _UFS_O_NOFOLLOW 0x100 // 打开（内部）：追随符号链接
static int __open_ex(ufs_context_t* context, ufs_minode_t** pminode, const char* path, unsigned long flag, uint16_t mode, uint64_t creat_inum) {
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
        ec = _search_dir(ppath_minode, fname, &inum, strlen(fname), NULL);
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
        if(!(_check_perm(context->uid, context->gid, &ppath_minode->inode) & UFS_W_OK)) ec = EACCES;
        else {
            if(creat_inum) {
                inum = creat_inum;
                ec = ufs_fileset_open(&context->ufs->fileset, inum, &file_minode);
                if(ufs_likely(ec == 0)) {
                    ufs_minode_lock(file_minode);
                    if(++file_minode->inode.nlink == 0) {
                        --file_minode->inode.nlink;
                        ec = UFS_EMLINK;
                    }
                    ufs_minode_unlock(file_minode);
                }
            } else {
                ufs_inode_create_t creat;
                creat.uid = context->uid;
                creat.gid = context->gid;
                creat.mode = mode;
                ec = ufs_fileset_creat(&context->ufs->fileset, &inum, &file_minode, &creat);
            }
            if(ufs_likely(ec == 0)) {
                ec = _add_to_dir(context, ppath_minode, fname, strlen(fname), inum);
                if(ufs_unlikely(ec)) {
                    ufs_minode_lock(file_minode);
                    --file_minode->inode.nlink;
                    ufs_minode_unlock(file_minode);
                }
            }
        }
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

/**
 * 打开文件，不对文件本身做检查
 * 如果creat_inum不为0，则创建时直接使用creat_inum而不是使用mode创建
 *
 * 错误：
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
 *   [UFS_EMLINK] 文件链接数过多
*/
static int _open_ex(ufs_context_t* context, ufs_minode_t** pminode, const char* path, unsigned long flag, uint16_t mode, uint64_t creat_inum) {
    int ec;
    int stop = 0;
    ufs_minode_t* minode;

    ec = __open_ex(context, &minode, path, flag, mode, creat_inum);
    if(ufs_unlikely(ec)) return ec;
do_again:
    if(!(mode & _UFS_O_NOFOLLOW) && UFS_S_ISLNK(minode->inode.mode)) {
        char* resolved;
        ufs_minode_t* minode2;
        if(stop++ >= UFS_SYMBOL_LOOP_LIMIT) { ufs_fileset_close(&context->ufs->fileset, minode->inum); return ELOOP; }
        ec = _readlink(context, minode, &resolved);
        if(ufs_unlikely(ec)) { ufs_fileset_close(&context->ufs->fileset, minode->inum); return ec; }
        ec = __open_ex(context, &minode2, resolved, flag, mode, creat_inum);
        ufs_free(resolved);
        ufs_fileset_close(&context->ufs->fileset, minode->inum);
        minode = minode2;
        if(ufs_unlikely(ec)) { ufs_fileset_close(&context->ufs->fileset, minode->inum); return ec; }
        goto do_again;
    }
    *pminode = minode;
    return 0;
}
/**
 * 打开文件，不对文件本身做检查
 *
 * 错误：
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
*/
static int _open(ufs_context_t* context, ufs_minode_t** pminode, const char* path, unsigned long flag, uint16_t mode) {
    return _open_ex(context, pminode, path, flag, mode, 0);
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

    if(ufs_unlikely(context == NULL || context->ufs == NULL)) return UFS_EINVAL;
    if(ufs_unlikely(path == NULL || path[0] == 0)) return UFS_EINVAL;
    if(ufs_unlikely((flag & UFS_O_RDONLY) && (flag & UFS_O_WRONLY))) return UFS_EINVAL;

    mask &= ~context->umask & 0777u;
    ec = _open(context, &minode, path, (flag & 0xFF), (mask & 0777) | UFS_S_IFREG);
    if(ufs_unlikely(ec)) return ec;
    if(UFS_S_ISDIR(minode->inode.mode)) {
        ufs_fileset_close(&context->ufs->fileset, minode->inum);
        return UFS_EISDIR;
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
    if(flag & UFS_O_EXEC) file->flag |= UFS_X_OK;

    do {
        int access;
        ufs_minode_lock(minode);
        access = _check_perm(context->uid, context->gid, &minode->inode);
        ufs_minode_unlock(minode);
        if((file->flag & UFS_R_OK) && !(access & UFS_R_OK)) ec = EACCES;
        if((file->flag & UFS_W_OK) && !(access & UFS_W_OK)) ec = EACCES;
        if((file->flag & UFS_X_OK) && !(access & UFS_X_OK)) ec = EACCES;
        if(ufs_unlikely(ec)) {
            ufs_fileset_close(&context->ufs->fileset, minode->inum);
            ufs_free(file);
            return ec;
        }
    } while(0);

    if(flag & UFS_O_APPEND) file->flag |= _OPEN_APPEND;
    if(flag & UFS_O_TRUNC) {
        ufs_minode_lock(minode);
        if(_check_perm(context->uid, context->gid, &minode->inode) & UFS_W_OK) ec = ufs_minode_resize(minode, 0);
        else ec = UFS_EACCESS;
        ufs_minode_unlock(minode);
        if(ufs_unlikely(ec)) {
            ufs_fileset_close(&context->ufs->fileset, minode->inum);
            ufs_free(file);
            return ec;
        }
    }
    *pfile = file;
    return 0;
}

UFS_API int ufs_creat(ufs_context_t* context, ufs_file_t** pfile, const char* path, uint16_t mask) {
    return ufs_open(context, pfile, path, UFS_O_CREAT | UFS_O_WRONLY | UFS_O_TRUNC, mask);
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

UFS_API int ufs_write(ufs_file_t* file, const void* buf, size_t len, size_t* pwriten) {
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

UFS_API int ufs_pwrite(ufs_file_t* file, const void* buf, size_t len, uint64_t off, size_t* pwriten) {
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

UFS_API int ufs_fallocate(ufs_file_t* file, uint64_t off, uint64_t len) {
    int ec;
    if(ufs_unlikely(file == NULL)) return UFS_EBADF;
    _file_lock(file);
    ufs_minode_lock(file->minode);
    ec = ufs_minode_fallocate(file->minode, (off + UFS_BLOCK_SIZE - 1) / UFS_BLOCK_SIZE,
        (off + len + UFS_BLOCK_SIZE - 1) / UFS_BLOCK_SIZE + 1);
    ufs_minode_unlock(file->minode);
    _file_unlock(file);
    return ec;
}

UFS_API int ufs_ftruncate(ufs_file_t* file, uint64_t size) {
    int ec;
    if(ufs_unlikely(file == NULL)) return UFS_EBADF;
    _file_lock(file);
    ufs_minode_lock(file->minode);
    if(!(file->flag & UFS_W_OK)) ec = EBADF;
    else if(!(_check_perm(file->uid, file->gid, &file->minode->inode) & UFS_W_OK)) ec = EACCES;
    else ec = ufs_minode_resize(file->minode, size);
    ufs_minode_unlock(file->minode);
    _file_unlock(file);
    return ec;
}

UFS_API int ufs_fsync(ufs_file_t* file, int only_data) {
    int ec;
    if(ufs_unlikely(file == NULL)) return UFS_EBADF;
    _file_lock(file);
    ufs_minode_lock(file->minode);
    ec = ufs_minode_sync(file->minode, only_data);
    ufs_minode_unlock(file->minode);
    _file_unlock(file);
    return ec;
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

UFS_API int ufs_opendir(ufs_context_t* context, ufs_dir_t** pdir, const char* path) {
    ufs_minode_t* minode;
    int ec;
    ufs_dir_t* dir;

    if(ufs_unlikely(context == NULL || context->ufs == NULL)) return UFS_EINVAL;
    if(ufs_unlikely(path == NULL || path[0] == 0)) return UFS_EINVAL;

    ec = _open(context, &minode, path, 0, 0664 | UFS_S_IFDIR);
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

UFS_API int ufs_seekdir(ufs_dir_t* dir, uint64_t off) {
    if(ufs_unlikely(dir == NULL)) return UFS_EBADF;
    off -= off % sizeof(_dirent_t);
    _dir_lock(dir);
    dir->off = off;
    _dir_unlock(dir);
    return 0;
}

UFS_API int ufs_telldir(ufs_dir_t* dir, uint64_t* poff) {
    if(ufs_unlikely(dir == NULL)) return UFS_EBADF;
    if(ufs_unlikely(poff == NULL)) return UFS_EINVAL;
    _dir_lock(dir);
    *poff = dir->off;
    _dir_unlock(dir);
    return 0;
}

UFS_API int ufs_rewinddir(ufs_dir_t* dir) {
    return ufs_seekdir(dir, 0);
}

UFS_API int ufs_closedir(ufs_dir_t* dir) {
    int ec;
    if(ufs_unlikely(dir == NULL)) return UFS_EBADF;
    ec = ufs_fileset_close(&dir->minode->ufs->fileset, dir->minode->inum);
    ufs_free(dir);
    return ec;
}



UFS_API int ufs_mkdir(ufs_context_t* context, const char* path, uint16_t mode) {
    int ec;
    ufs_minode_t* minode;

    if(ufs_unlikely(context == NULL || context->ufs == NULL)) return UFS_EINVAL;
    if(ufs_unlikely(path == NULL || path[0] == 0)) return UFS_EINVAL;

    ec = _open(context, &minode, path, UFS_O_CREAT | UFS_O_EXCL | _UFS_O_NOFOLLOW, (mode & UFS_S_IALL) | UFS_S_IFDIR);
    if(ufs_unlikely(ec)) return ec;
    return ufs_fileset_close(&context->ufs->fileset, minode->inum);
}

UFS_API int ufs_rmdir(ufs_context_t* context, const char* path) {
    int ec;
    ufs_minode_t* minode = NULL;
    ufs_minode_t* ppath_minode;
    const char* fname;
    uint64_t inum = 0;
    uint64_t off = 0;

    if(ufs_unlikely(context == NULL || context->ufs == NULL)) return UFS_EINVAL;
    if(ufs_unlikely(path == NULL || path[0] == 0)) return UFS_EINVAL;

    ec = _ppath2inum(context, path, &ppath_minode, &fname);
    if(ufs_unlikely(ec)) return ec;
    if(fname[0] == 0) { // 根目录不可删除
        ufs_fileset_close(&context->ufs->fileset, ppath_minode->inum);
        return UFS_EACCESS;
    }
    ufs_minode_lock(ppath_minode);
    if(_check_perm(context->uid, context->gid, &ppath_minode->inode) & (UFS_W_OK | UFS_X_OK))
        ec = _search_dir(ppath_minode, fname, &inum, strlen(fname), &off);
    else ec = UFS_EACCESS;
    if(ec == 0) ec = ufs_fileset_open(&context->ufs->fileset, inum, &minode);
    if(ec == 0 && !UFS_S_ISDIR(minode->inode.mode)) ec = ENOTDIR;
    if(ec == 0) {
        ufs_minode_lock(minode);
        ec = _shrink_dir(minode);
        if(ufs_likely(ec == 0)) {
            if(minode->inode.size == 0) ec = _delete_from_dir(context, ppath_minode, off);
            else ec = UFS_ENOTEMPTY;
        }
        ufs_minode_unlock(minode);
    }
    ufs_minode_unlock(ppath_minode);
    ufs_fileset_close(&context->ufs->fileset, ppath_minode->inum);
    if(ufs_unlikely(ec)) {
        if(minode) ufs_fileset_close(&context->ufs->fileset, minode->inum);
        return ec;
    }

    ufs_minode_lock(minode);
    --minode->inode.nlink;
    ufs_minode_unlock(minode);
    return ufs_fileset_close(&context->ufs->fileset, minode->inum);
}

UFS_API int ufs_unlink(ufs_context_t* context, const char* path) {
    int ec;
    ufs_minode_t* minode = NULL;
    ufs_minode_t* ppath_minode;
    const char* fname;
    uint64_t inum = 0;
    uint64_t off = 0;

    if(ufs_unlikely(context == NULL || context->ufs == NULL)) return UFS_EINVAL;
    if(ufs_unlikely(path == NULL || path[0] == 0)) return UFS_EINVAL;

    ec = _ppath2inum(context, path, &ppath_minode, &fname);
    if(ufs_unlikely(ec)) return ec;
    if(fname[0] == 0) { // 根目录不可删除
        ufs_fileset_close(&context->ufs->fileset, ppath_minode->inum);
        return UFS_EACCESS;
    }
    ufs_minode_lock(ppath_minode);
    if(_check_perm(context->uid, context->gid, &ppath_minode->inode) & (UFS_W_OK | UFS_X_OK))
        ec = _search_dir(ppath_minode, fname, &inum, strlen(fname), &off);
    else ec = UFS_EACCESS;
    if(ec == 0) ec = ufs_fileset_open(&context->ufs->fileset, inum, &minode);
    if(ec == 0 && UFS_S_ISDIR(minode->inode.mode)) ec = EISDIR;
    if(ec == 0) ec = _delete_from_dir(context, ppath_minode, off);
    ufs_minode_unlock(ppath_minode);
    ufs_fileset_close(&context->ufs->fileset, ppath_minode->inum);
    if(ufs_unlikely(ec)) {
        if(minode) ufs_fileset_close(&context->ufs->fileset, minode->inum);
        return ec;
    }

    ufs_minode_lock(minode);
    --minode->inode.nlink;
    ufs_minode_unlock(minode);
    return ufs_fileset_close(&context->ufs->fileset, minode->inum);
}

UFS_API int ufs_link(ufs_context_t* context, const char* target, const char* source) {
    int ec;
    ufs_minode_t* tinode = NULL;
    ufs_minode_t* sinode;
    ec = _open(context, &sinode, source, 0, 0);
    if(ufs_unlikely(ec)) return ec;
    if(ufs_likely(!UFS_S_ISDIR(sinode->inode.mode)))
        ec = _open_ex(context, &tinode, target, UFS_O_CREAT | UFS_O_EXCL | _UFS_O_NOFOLLOW, 0777, sinode->inum);
    else ec = EISDIR;
    ufs_fileset_close(&context->ufs->fileset, sinode->inum);
    if(ufs_unlikely(ec)) return ec;
    ufs_fileset_close(&context->ufs->fileset, tinode->inum);
    return 0;
}

UFS_API int ufs_symlink(ufs_context_t* context, const char* target, const char* source) {
    int ec, ec2;
    ufs_transcation_t transcation;
    size_t len, writen;
    ufs_minode_t* minode;

    if(ufs_unlikely(context == NULL || context->ufs == NULL)) return UFS_EINVAL;
    if(ufs_unlikely(target == NULL || target[0] == 0)) return UFS_EINVAL;
    if(ufs_unlikely(source == NULL || source[0] == 0)) return UFS_EINVAL;
    len = strlen(source) + 1;
    if(len > UFS_BLOCK_SIZE) return UFS_EOVERFLOW;

    ec = _open(context, &minode, target, UFS_O_CREAT | UFS_O_EXCL | _UFS_O_NOFOLLOW, 0777 | UFS_S_IFLNK);
    if(ufs_unlikely(ec)) return ec;

    ufs_transcation_init(&transcation, &context->ufs->jornal);
    ufs_minode_lock(minode);
    ec = ufs_minode_pwrite(minode, &transcation, source, len, 0, &writen);
    if(ufs_likely(ec == 0 && writen != len)) ec = ENOSPC;
    if(ufs_likely(ec == 0)) ec = ufs_transcation_commit_all(&transcation);
    ufs_minode_unlock(minode);
    ufs_transcation_deinit(&transcation);

    ec2 = ufs_fileset_close(&context->ufs->fileset, minode->inum);
    return ec ? ec : ec2;
}

UFS_API int ufs_readlink(ufs_context_t* context, const char* source, char** presolved) {
    int ec;
    ufs_minode_t* minode;

    if(ufs_unlikely(context == NULL || context->ufs == NULL)) return UFS_EINVAL;
    if(ufs_unlikely(source == NULL || source[0] == 0)) return UFS_EINVAL;
    if(ufs_unlikely(presolved == NULL)) return UFS_EINVAL;


    ec = _open(context, &minode, source, _UFS_O_NOFOLLOW, 0);
    if(ufs_unlikely(ec)) return ec;
    if(ufs_likely(UFS_S_ISLNK(minode->inode.mode))) ec = _readlink(context, minode, presolved);
    else ec = UFS_EFTYPE;
    ufs_fileset_close(&context->ufs->fileset, minode->inum);
    return ec;
}



static void _stat(ufs_minode_t* inode, ufs_stat_t* stat) {
    stat->st_ino = inode->inum;
    stat->st_mode = inode->inode.mode;
    stat->st_nlink = inode->inode.nlink;
    stat->st_uid = inode->inode.uid;
    stat->st_gid = inode->inode.gid;
    stat->st_size = inode->inode.size;
    stat->st_blksize = UFS_BLOCK_SIZE;
    stat->st_blocks = inode->inode.blocks;
    stat->st_ctim = inode->inode.ctime;
    stat->st_atim = inode->inode.atime;
    stat->st_mtim = inode->inode.mtime;
}
UFS_API int ufs_fstat(ufs_file_t* file, ufs_stat_t* stat) {
    if(ufs_unlikely(file == NULL)) return UFS_EBADF;
    if(ufs_unlikely(stat == NULL)) return UFS_EINVAL;

    ufs_minode_lock(file->minode);
    _stat(file->minode, stat);
    ufs_minode_unlock(file->minode);
    return 0;
}
UFS_API int ufs_stat(ufs_context_t* context, const char* path, ufs_stat_t* stat) {
    int ec;
    ufs_minode_t* minode;

    if(ufs_unlikely(context == NULL || context->ufs == NULL)) return UFS_EINVAL;
    if(ufs_unlikely(path == NULL || path[0] == 0)) return UFS_EINVAL;

    ec = _open(context, &minode, path, 0, 0664);
    if(ufs_unlikely(ec)) return ec;

    ufs_minode_lock(minode);
    _stat(minode, stat);
    ufs_minode_unlock(minode);
    ufs_fileset_close(&context->ufs->fileset, minode->inum);

    return 0;
}

UFS_API int ufs_chmod(ufs_context_t* context, const char* path, uint16_t mask) {
    int ec;
    ufs_minode_t* minode;
    if(ufs_unlikely(context == NULL || context->ufs == NULL)) return UFS_EINVAL;
    if(ufs_unlikely(path == NULL || path[0] == 0)) return UFS_EINVAL;
    ec = _open(context, &minode, path, 0, 0664);
    if(ufs_unlikely(ec)) return ec;
    ufs_minode_lock(minode);
    if(_checkuid(context->uid, minode->inode.uid)) {
        minode->inode.mode = (minode->inode.mode & UFS_S_IFMT) | mask;
    } else ec = EACCES;
    ufs_minode_unlock(minode);
    ufs_fileset_close(&context->ufs->fileset, minode->inum);
    return ec;
}

UFS_API int ufs_chown(ufs_context_t* context, const char* path, int32_t uid, int32_t gid) {
    int ec;
    ufs_minode_t* minode;
    if(ufs_unlikely(context == NULL || context->ufs == NULL)) return UFS_EINVAL;
    if(ufs_unlikely(path == NULL || path[0] == 0)) return UFS_EINVAL;
    ec = _open(context, &minode, path, 0, 0664);
    if(ufs_unlikely(ec)) return ec;
    ufs_minode_lock(minode);
    if(_checkuid(context->uid, minode->inode.uid)) {
        if(uid >= 0) minode->inode.uid = uid;
        if(gid >= 0) minode->inode.gid = gid;
    } else ec = EACCES;
    ufs_minode_unlock(minode);
    ufs_fileset_close(&context->ufs->fileset, minode->inum);
    return ec;
}

UFS_API int ufs_access(ufs_context_t* context, const char* path, int access) {
    int ec, x;
    ufs_minode_t* minode;
    if(ufs_unlikely(context == NULL || context->ufs == NULL)) return UFS_EINVAL;
    if(ufs_unlikely(path == NULL || path[0] == 0)) return UFS_EINVAL;
    ec = _open(context, &minode, path, 0, 0664);
    if(ufs_unlikely(ec)) return ec;
    ufs_minode_lock(minode);
    x = _check_perm(context->uid, context->gid, &minode->inode);
    ufs_minode_unlock(minode);

    if((access & UFS_R_OK) && !(x & UFS_R_OK)) ec = EACCES;
    if((access & UFS_W_OK) && !(x & UFS_W_OK)) ec = EACCES;
    if((access & UFS_X_OK) && !(x & UFS_X_OK)) ec = EACCES;
    ufs_fileset_close(&context->ufs->fileset, minode->inum);
    return ec;
}

UFS_API int ufs_utimes(ufs_context_t* context, const char* path, int64_t* ctime, int64_t* atime, int64_t* mtime) {
    int ec;
    ufs_minode_t* minode;
    if(ufs_unlikely(context == NULL || context->ufs == NULL)) return UFS_EINVAL;
    if(ufs_unlikely(path == NULL || path[0] == 0)) return UFS_EINVAL;
    ec = _open(context, &minode, path, 0, 0664);
    if(ufs_unlikely(ec)) return ec;
    ufs_minode_lock(minode);
    if(_checkuid(context->uid, minode->inode.uid)) {
        if(ctime) minode->inode.ctime = *ctime;
        if(atime) minode->inode.atime = *atime;
        if(mtime) minode->inode.mtime = *mtime;
    } else ec = EACCES;
    ufs_minode_unlock(minode);
    ufs_fileset_close(&context->ufs->fileset, minode->inum);
    return ec;
}
UFS_API int ufs_truncate(ufs_context_t* context, const char* path, uint64_t size) {
    int ec;
    ufs_minode_t* minode;
    if(ufs_unlikely(context == NULL || context->ufs == NULL)) return UFS_EINVAL;
    if(ufs_unlikely(path == NULL || path[0] == 0)) return UFS_EINVAL;
    ec = _open(context, &minode, path, 0, 0664);
    if(ufs_unlikely(ec)) return ec;
    ufs_minode_lock(minode);
    if(!(_check_perm(context->uid, context->gid, &minode->inode) & UFS_W_OK)) ec = EACCES;
    else ec = ufs_minode_resize(minode, size);
    ufs_minode_unlock(minode);
    ufs_fileset_close(&context->ufs->fileset, minode->inum);
    return ec;
}
UFS_API int ufs_rename(ufs_context_t* context, const char* oldname, const char* newname) {
    int ec;
    ufs_minode_t* minode;
    ufs_minode_t* ppath_minode;
    ufs_minode_t* new_minode;
    const char* fname;
    uint64_t inum, off;

    if(ufs_unlikely(context == NULL || context->ufs == NULL)) return UFS_EINVAL;
    if(ufs_unlikely(oldname == NULL || oldname[0] == 0)) return UFS_EINVAL;
    if(ufs_unlikely(newname == NULL || newname[0] == 0)) return UFS_EINVAL;
    
    ec = _ppath2inum(context, oldname, &ppath_minode, &fname);
    if(ufs_unlikely(ec)) return ec;
    if(fname[0] == 0) { // 根目录不可删除
        ufs_fileset_close(&context->ufs->fileset, ppath_minode->inum);
        return UFS_EACCESS;
    }
    ufs_minode_lock(ppath_minode);
    if(_check_perm(context->uid, context->gid, &ppath_minode->inode) & (UFS_W_OK | UFS_X_OK))
        ec = _search_dir(ppath_minode, fname, &inum, strlen(fname), &off);
    else ec = UFS_EACCESS;
    if(ec == 0) ec = ufs_fileset_open(&context->ufs->fileset, inum, &minode);
    if(ec == 0) ec = _delete_from_dir(context, ppath_minode, off);
    ufs_minode_unlock(ppath_minode);
    ufs_fileset_close(&context->ufs->fileset, ppath_minode->inum);
    if(ufs_unlikely(ec)) {
        if(minode) ufs_fileset_close(&context->ufs->fileset, minode->inum);
        return ec;
    }

    ec = _open_ex(context, &new_minode, newname, _UFS_O_NOFOLLOW | UFS_O_CREAT | UFS_O_EXCL, 0, minode->inum);
    if(ufs_likely(ec == 0)) {
        ufs_minode_lock(minode);
        --minode->inode.nlink;
        ufs_minode_unlock(minode);
        ufs_fileset_close(&context->ufs->fileset, minode->inum);
    }

    return ufs_fileset_close(&context->ufs->fileset, minode->inum);
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
