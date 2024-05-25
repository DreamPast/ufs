#include "libufs_internel.h"

static int _checkuid(int32_t uid, int32_t file_uid) {
    return uid == 0 || file_uid == 0 || uid == file_uid;
}
static int _checkgid(int32_t gid, int32_t file_gid) {
    return gid == 0 || file_gid == 0 || gid == file_gid;
}
static int _check_perm(const ufs_context_t* context, const ufs_inode_t* inode) {
    int r = 0;
    if(_checkuid(context->uid, inode->uid)) r |= (inode->mode & UFS_S_IRWXU) >> 6;
    if(_checkgid(context->gid, inode->gid)) r |= (inode->mode & UFS_S_IRWXG) >> 3;
    return r | (inode->mode & UFS_S_IRWXO);
}

typedef struct _dirent_t {
    char name[UFS_NAME_MAX];
    uint64_t inum;
} _dirent_t;

ul_unused static int _dirent_empty(const _dirent_t* dirent) {
    int i;
    for(i = 0; i < UFS_NAME_MAX; ++i)
        if(dirent->name[i]) return 0;
    return 1;
}

static int _readlink(ufs_context_t* ufs_restrict context, uint64_t inum, char** presolved) {
    ufs_minode_t minode;
    int ec;
    char* resolved;
    size_t read;
    ufs_transcation_t transcation;

    resolved = ul_reinterpret_cast(char*, ufs_malloc(UFS_BLOCK_SIZE));
    if(ufs_unlikely(resolved == NULL)) return UFS_ENOMEM;

    ec = ufs_minode_init(context->ufs, &minode, inum);
    if(ufs_unlikely(ec)) { ufs_free(resolved); return ec; }
    ufs_assert(UFS_S_ISLNK(minode.inode.mode));
    ufs_transcation_init(&transcation, &context->ufs->jornal);

    ec = ufs_minode_pread(&minode, &transcation, resolved, UFS_BLOCK_SIZE, 0, &read);
    if(ufs_unlikely(ec)) goto do_return;
    if(ufs_unlikely(read != UFS_BLOCK_SIZE)) { ec = UFS_ESTALE; goto do_return; }
    *presolved = resolved; resolved = NULL;

do_return:
    ufs_free(resolved);
    ufs_minode_deinit(&minode);
    ufs_transcation_deinit(&transcation);
    return ec;
}

static int _search_dir(ufs_minode_t* minode, const char* target, uint64_t *pinum, size_t len) {
    int ec;
    ufs_transcation_t transcation;
    _dirent_t dirent;
    uint64_t off = 0;
    size_t read;

    ufs_transcation_init(&transcation, &minode->ufs->jornal);
    ufs_assert(len < UFS_NAME_MAX);
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

#if 0
// UFS_ENAMETOOLONG / UFS_ENOTDIR / UFS_ELOOP / UFS_ENOMEM / UFS_ENOENT / UFS_EACCESS
static int __path2inum(ufs_context_t* ufs_restrict context, const char* ufs_restrict path, ufs_minode_t** pminode, int sloop) {
    int ec;
    ufs_minode_t* minode;
    const char* path2;

    ec = ufs_fileset_open(&context->ufs->fileset, UFS_INUM_ROOT, &minode);
    if(ufs_unlikely(ec)) return ec;
    
    for(;;) {
        while(_is_slash(*path)) ++path;
        path2 = _find_next_part(path);
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
                ec = _readlink(context, minode->inum, &resolved);
                if(ufs_unlikely(ec)) { ufs_fileset_close(&context->ufs->fileset, minode->inum); return ec; }
                ufs_fileset_close(&context->ufs->fileset, minode->inum);
                ec = __path2inum(context, resolved, pminode, sloop + 1);
                ufs_free(resolved);
                if(ufs_unlikely(ec)) return ec;
            }
            break;
        case UFS_S_IFDIR:
            do {
                uint64_t inum;
                if(!(_check_perm(context, &minode->inode) & UFS_X_OK)) {
                    ufs_fileset_close(&context->ufs->fileset, minode->inum);
                    return UFS_EACCESS;
                }
                ec = _search_dir(minode, path, &inum, path2 - path);
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
        if(path2[0] == 0) break;
    }

    *pminode = minode;
    return 0;
}
static int _path2inum(ufs_context_t* ufs_restrict context, const char* ufs_restrict path, ufs_minode_t** pminode) {
    return __path2inum(context, path, pminode, 0);
}
#endif

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
                ec = _readlink(context, minode->inum, &resolved);
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
                if(!(_check_perm(context, &minode->inode) & UFS_X_OK)) {
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


typedef struct ufs_file_t {
    ufs_minode_t* minode;
    uint64_t off;
    int flag;
} ufs_file_t;
#define _OPEN_APPEND 8

/**
 * 打开文件
 * 
 * 错误：
 *   [UFS_EINVAL] context非法
 *   [UFS_EINVAL] pfile为NULL
 *   [UFS_EINVAL] path为NULL或者path为空
 *   [UFS_EINVAL] （指导性错误）用户试图使用UFS_O_RDONLY和UFS_O_WRONLY合成UFS_O_RDWR
 *   [UFS_ENAMETOOLONG] 文件名过长
 *   [UFS_ENOTDIR] 路径中存在非目录或指向非目录的符号链接
 *   [UFS_ELOOP] 路径中符号链接层数过深
 *   [UFS_ENOMEM] 无法分配内存
 *   [UFS_ENOENT] 路径中存在不存在的目录
 *   [UFS_EACCESS] 用户对路径中的目录不具备执行权限
 *   [UFS_EACCESS] 试图创建文件但用户不具备对目录的写权限
 *   [UFS_EACCESS] 用户对文件不存在指定权限
 *   [UFS_ENOSPC] 试图创建文件但磁盘空间不足
 *   [UFS_EEXIST] 文件被指定创建和排他，但是目标文件已存在
 *   [UFS_EACCESS] 试图截断文件但用户不对文件具备写入权限
 *   [UFS_EISDIR] 用户试图打开非常规文件
*/
UFS_API int ufs_open(ufs_context_t* context, ufs_file_t** pfile, const char* path, unsigned long flag, uint16_t mask) {
    ufs_file_t* file;
    ufs_minode_t* file_minode = NULL;
    uint64_t inum = 0;
    int ec;

    if(ufs_unlikely(context == NULL || context->ufs == NULL)) return UFS_EINVAL;
    if(ufs_unlikely(path == NULL || path[0] == 0)) return UFS_EINVAL;
    if(ufs_unlikely((flag & UFS_O_RDONLY) && (flag & UFS_O_WRONLY))) return UFS_EINVAL;

    mask &= context->umask & 0777u;

    // 检查文件是否存在，不存在则根据flag尝试创建
    do {
        ufs_minode_t* ppath_minode;
        const char* fname;
        ec = _ppath2inum(context, path, &ppath_minode, &fname);
        if(ufs_unlikely(ec)) return ec;
        ufs_minode_lock(ppath_minode);
        if(_check_perm(context, &ppath_minode->inode) & UFS_X_OK)
            ec = _search_dir(ppath_minode, fname, &inum, strlen(fname));
        else ec = UFS_EACCESS;
        if(ec == 0) {
            if(flag & UFS_O_CREAT) {
                flag &= ul_reinterpret_cast(unsigned long, ~UFS_O_CREAT); // 文件存在则擦除创建标记
                if(flag & UFS_O_EXCL) ec = EEXIST;
            }
        } else {
            if(ec == UFS_ENOENT) {
                if(flag & UFS_O_CREAT) {
                    if(_check_perm(context, &ppath_minode->inode) & UFS_W_OK) ec = 0; // 可以创建文件，清空错误
                    else ec = UFS_EACCESS;
                }
            }
        }
        if((ec == 0) && (flag & UFS_O_CREAT)) {
            ufs_inode_create_t creat;
            ufs_transcation_t transcation;
            _dirent_t dirent;
            size_t flen;
            
            creat.uid = context->uid;
            creat.gid = context->gid;
            creat.mode = mask | UFS_S_IFREG;
            ec = ufs_fileset_creat(&context->ufs->fileset, &inum, &file_minode, &creat);
            if(ufs_likely(ec == 0)) {
                ufs_transcation_init(&transcation, &context->ufs->jornal);
                flen = strlen(fname);
                memcpy(dirent.name, fname, flen);
                memset(dirent.name + flen, 0, UFS_NAME_MAX - flen);
                dirent.inum = ul_trans_u64_le(inum);
                ec = ufs_minode_pwrite(ppath_minode, &transcation, &dirent, sizeof(dirent), ppath_minode->inode.size, &flen);
                if(ufs_unlikely(ec == 0 && flen != sizeof(dirent))) ec = UFS_ENOSPC;
                if(ufs_likely(ec == 0)) ec = ufs_transcation_commit_all(&transcation);
                ufs_transcation_deinit(&transcation);
            }
        }
        ufs_minode_unlock(ppath_minode);
        ufs_fileset_close(&context->ufs->fileset, ppath_minode->inum);
        if(ufs_unlikely(ec)) {
            if(file_minode) ufs_fileset_close(&context->ufs->fileset, file_minode->inum);
            return ec;
        }
    } while(0);

    ufs_assert(inum != 0); // 此时inum必定有值
    if(file_minode == NULL) {
        ec = ufs_fileset_open(&context->ufs->fileset, inum, &file_minode);
        if(ufs_unlikely(ec)) return ec;
    }
    file = ul_reinterpret_cast(ufs_file_t*, ufs_malloc(sizeof(ufs_file_t)));
    if(ufs_unlikely(file == NULL)) { ufs_fileset_close(&context->ufs->fileset, inum); return UFS_ENOMEM; }
    file->minode = file_minode;
    file->off = 0;
    file->flag = 0;
    if(flag & UFS_O_RDONLY) file->flag |= UFS_R_OK;
    if(flag & UFS_O_WRONLY) file->flag |= UFS_W_OK;
    if(flag & UFS_O_RDWR) file->flag |= UFS_R_OK | UFS_W_OK;
    if(flag & UFS_O_APPEND) file->flag |= _OPEN_APPEND;
    if(flag & UFS_O_TRUNC) {
        ufs_minode_lock(file_minode);
        if(_check_perm(context, &file_minode->inode) & UFS_W_OK) ec = ufs_minode_resize(file_minode, 0);
        else ec = UFS_EACCESS;
        ufs_minode_unlock(file_minode);
    }
    if(!UFS_S_ISREG(file_minode->inode.mode)) {
        if(UFS_S_ISDIR(file_minode->inode.mode)) {
            ec = EISDIR;
        } else {
            // TODO: 处理符号链接
        }
    }

    if(ufs_unlikely(ec)) {
        ufs_free(file);
        ufs_fileset_close(&context->ufs->fileset, inum);
        return ec;
    }
    *pfile = file;
    return 0;
}
UFS_API int ufs_creat(ufs_context_t* context, ufs_file_t** pfile, const char* path, uint16_t mask) {
    return ufs_open(context, pfile, path, UFS_O_RDWR | UFS_O_CREAT, mask);
}
UFS_API int ufs_close(ufs_file_t* file) {
    int ec = ufs_fileset_close(&file->minode->ufs->fileset, file->minode->inum);
    ufs_free(file);
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
