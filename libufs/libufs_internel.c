#include "libufs_internel.h"

UFS_API const char* ufs_strerror(int error) {
    static const char* TABLE[] = {
        "[UFS_EUNKOWN] unknown error",
        "[UFS_EACCESS] permission denied",
        "[UFS_EAGAIN] resource temporarily unavailable",
        "[UFS_EBADF] bad file descriptor",
        "[UFS_ENOENT] no such file or directory",
        "[UFS_ENOMEM] cannot allocate memory",
        "[UFS_ENOTDIR] not a directory",
        "[UFS_EISDIR] is a directory",
        "[UFS_EINVAL] invalid argument",
        "[UFS_EMFILE] too many open files",
        "[UFS_EFBIG] file too large",
        "[UFS_EMLINK] too many links",
        "[UFS_ELOOP] too many levels of symbolic links",
        "[UFS_ENAMETOOLONG] file name too long",
        "[UFS_ESTALE] stale file handle",
        "[UFS_EFTYPE] inappropriate file type or format",
        "[UFS_EILSEQ] invalid or incomplete multibye or wide character",
        "[UFS_EOVERFLOW] value too large for defined data type",
        "[UFS_ENOSPC] no space left on device",
        "[UFS_EEXIST] file exists",
        "[UFS_ENOTEMPTY] directory not empty",
    };
    static const int TABLE_CNT = sizeof(TABLE) / sizeof(TABLE[0]);
    if(error >= 0) return strerror(error);
    if(-error >= TABLE_CNT) return "[UFS_E?] unkown error";
    return TABLE[-error - 1];
}

UFS_API int ufs_uniform_error(int error) {
    static const int TABLE[] = {
        EINVAL,
        EACCES,
        EAGAIN,
        EBADF,
        ENOENT,
        ENOMEM,
        ENOTDIR,
        EISDIR,
        EINVAL,
        EMFILE,
        EFBIG,
        EMLINK,
        ELOOP,
        ENAMETOOLONG,
    #ifdef ESTALE
        ESTALE,
    #else
        EINVAL,
    #endif
    #ifdef EFTYPE
        EFTYPE,
    #else
        EBADF,
    #endif
        EILSEQ,
        EOVERFLOW,
        ENOSPC,
        EEXIST,
        ENOTEMPTY,
    };
    static const int TABLE_CNT = sizeof(TABLE) / sizeof(TABLE[0]);
    if(error >= 0) return error;
    if(-error >= TABLE_CNT) return EINVAL;
    return TABLE[-error - 1];
}

// 在Windows上，我们无法准确地实现UID和GID
#ifdef _WIN32
    UFS_API int32_t ufs_getuid(void) { return 0; }
    UFS_API int32_t ufs_getgid(void) { return 0; }
#else
    #include <unistd.h>
    UFS_API int32_t ufs_getuid(void) { return ul_static_cast(int32_t, getuid()); }
    UFS_API int32_t ufs_getgid(void) { return ul_static_cast(int32_t, getgid()); }
#endif

#include "uldate.h"
UFS_API int64_t ufs_time(int use_locale) {
    const uldate_t date = use_locale ? uldate_now_locale() : uldate_now_utc();
    return ufs_likely(date != ULDATE_INVALID) ? date : 0;
}
UFS_API size_t ufs_strtime(int64_t time, char* buf, const char* fmt, size_t len) {
    if(fmt == NULL) fmt = "%FT%T.%+Z";
    if(buf == NULL) return uldate_format_len(fmt, time);
    else return uldate_format(buf, len, fmt, time);
}
UFS_API int ufs_ptime(int64_t time, const char* fmt, FILE* fp) {
    char* buf;
    size_t len;
    int ret = EOF;

    if(fmt == NULL) fmt = "%FT%T.%+Z";
    len = uldate_format_len(fmt, time);
    buf = ul_reinterpret_cast(char*, malloc(len));
    if(buf == NULL) return EOF;
    if(uldate_format(buf, len, fmt, time) == len) ret = fputs(buf, fp);
    free(buf);
    return ret;
}

UFS_API int ufs_new(ufs_t** pufs, ufs_vfs_t* vfs) {
    int ec;
    uint64_t tmp;
    ufs_t* ufs;
    ufs_transcation_t transcation;

    if(pufs == NULL) return EINVAL;
    if(vfs == NULL) return EINVAL;

    ufs = ul_reinterpret_cast(ufs_t*, ufs_malloc(sizeof(ufs_t)));
    if(ufs_unlikely(ufs == NULL)) return ENOMEM;
    ufs->vfs = vfs;

    // 初始化日志
    ec = ufs_jornal_init(&ufs->jornal, vfs);
    if(ufs_unlikely(ec)) goto fail_return;

    // 读取超级块
    ec = ufs_vfs_pread(vfs, &ufs->sb, sizeof(ufs->sb), UFS_BNUM_SB * UFS_BLOCK_SIZE, &tmp);
    if(ufs_unlikely(ec)) goto fail_return;

    // 检查配置是否正确
    if(ufs->sb.magic[0] != UFS_MAGIC1 && ufs->sb.magic[1] != UFS_MAGIC2) {
        ec = EINVAL; goto fail_return;
    }
    if(ufs->sb.jornal_num != UFS_JORNAL_NUM && ufs->sb.ext_offset != 0) {
        ec = EINVAL; goto fail_return;
    }
    if((ul_static_cast(uint64_t, 1) << ufs->sb.block_size_log2) != UFS_BLOCK_SIZE) {
        ec = EINVAL; goto fail_return;
    }
    ufs->sb.iblock_max = ul_trans_u64_le(ufs->sb.iblock_max);
    ufs->sb.zblock_max = ul_trans_u64_le(ufs->sb.zblock_max);
    
    // 修复日志
    ec = ufs_fix_jornal(vfs, &ufs->sb);
    if(ufs_unlikely(ec)) goto fail_return;

    ufs_transcation_init(&transcation, &ufs->jornal);
    ufs->ilist.transcation = &transcation;
    ufs->zlist.transcation = &transcation;

    // 初始化ilist
    ec = ufs_ilist_init(&ufs->ilist, UFS_BNUM_ILIST * UFS_INODE_PER_BLOCK, ul_trans_u64_le(ufs->sb.iblock));
    if(ufs_unlikely(ec)) goto fail_return;

    // 初始化zlist
    ec = ufs_zlist_init(&ufs->zlist, UFS_BNUM_ZLIST, ul_trans_u64_le(ufs->sb.zblock));
    if(ufs_unlikely(ec)) goto fail_return;
    
    // 初始化文件集合
    ec = ufs_fileset_init(&ufs->fileset, ufs);
    if(ufs_unlikely(ec)) goto fail_return;
    
    ufs->ilist.transcation = NULL;
    ufs->zlist.transcation = NULL;
    *pufs = ufs;
    return 0;

fail_return:
    ufs_free(ufs);
    return ec;
}

static uint8_t _log2(uint64_t x) {
    uint8_t i;
    for(i = 0; (ul_static_cast(uint64_t, 1) << i) < x; ++i) { }
    return i;
}
UFS_API int ufs_new_format(ufs_t** pufs, ufs_vfs_t* vfs, uint64_t size) {
    int ec;
    uint64_t iblk, zblk;
    ufs_t* ufs;

    if(pufs == NULL) return EINVAL;
    if(vfs == NULL) return EINVAL;

    ufs = ul_reinterpret_cast(ufs_t*, ufs_malloc(sizeof(ufs_t)));
    if(ufs_unlikely(ufs == NULL)) return ENOMEM;
    ufs->vfs = vfs;

    // 初始化日志
    ec = ufs_jornal_init(&ufs->jornal, vfs);
    if(ufs_unlikely(ec)) goto fail_return;
    
    // 初始化超级块
    memset(&ufs->sb, 0, sizeof(ufs->sb));
    ufs->sb.magic[0] = UFS_MAGIC1;
    ufs->sb.magic[1] = UFS_MAGIC2;
    ufs->sb.ext_offset = 0;
    ufs->sb.jornal_num = UFS_JORNAL_NUM;
    ufs->sb.block_size_log2 = _log2(UFS_BLOCK_SIZE);

    // 初始化文件集合
    ec = ufs_fileset_init(&ufs->fileset, ufs);
    if(ufs_unlikely(ec)) goto fail_return;

    // 计算inode和zone数量
    size = size / UFS_BLOCK_SIZE;
    if(size < UFS_BNUM_START) { ec = UFS_ENOSPC; goto fail_return; }
    size -= UFS_BNUM_START;
    iblk = ((UFS_INODE_DEFAULT_RATIO / UFS_BLOCK_SIZE) * UFS_INODE_PER_BLOCK + 1);
    iblk = size / iblk;
    zblk = size - iblk;
    if(iblk == 0 || zblk == 0) { ec = UFS_ENOSPC; goto fail_return; }

    // 创建ilist
    do {
        ufs_transcation_t transcation;
        uint64_t i, e;
        ufs_transcation_init(&transcation, &ufs->jornal);
        ufs->ilist.transcation = &transcation;
        ec = ufs_ilist_create_empty(&ufs->ilist, UFS_BNUM_ILIST * UFS_INODE_PER_BLOCK);
        if(ufs_unlikely(ec)) goto fail_return;
        e = UFS_INUM_ROOT + 1;
        i = (UFS_BNUM_START + iblk) * UFS_INODE_PER_BLOCK - 1;
        ufs_ilist_lock(&ufs->ilist, &transcation);
        for(; i >= e; --i) {
            ec = ufs_ilist_push(&ufs->ilist, i);
            if(ufs_unlikely(ec)) break;
            if(transcation.num >= UFS_JORNAL_NUM - UFS_ILIST_CACHE_LIST_LIMIT) {
                ec = ufs_transcation_commit_all(&transcation);
                if(ufs_unlikely(ec)) break;
            }
        }
        ufs_ilist_unlock(&ufs->ilist);
        ufs_transcation_deinit(&transcation);
        if(ufs_unlikely(ec)) goto fail_return2;
    } while(0);

    // 初始化zlist
    do {
        ufs_transcation_t transcation;
        uint64_t i, e;
        e = UFS_BNUM_START + iblk + 1;
        i = UFS_BNUM_START + iblk + zblk - 1;
        ufs_transcation_init(&transcation, &ufs->jornal);
        ufs->zlist.transcation = &transcation;
        ec = ufs_zlist_create_empty(&ufs->zlist, UFS_BNUM_ZLIST);
        if(ufs_unlikely(ec)) goto fail_return2;
        ufs_zlist_lock(&ufs->zlist, &transcation);
        for(; i >= e; --i) {
            ec = ufs_zlist_push(&ufs->zlist, i);
            if(ufs_unlikely(ec)) break;
            if(transcation.num == UFS_JORNAL_NUM - UFS_ZLIST_CACHE_LIST_LIMIT) {
                ec = ufs_transcation_commit_all(&transcation);
                if(ufs_unlikely(ec)) break;
            }
        }
        ufs_zlist_unlock(&ufs->zlist);
        ufs_transcation_deinit(&transcation);
        if(ufs_unlikely(ec)) goto fail_return2;
    } while(0);

    ufs->sb.iblock_max = ul_trans_u64_le(ufs->ilist.now.block + 1);
    ufs->sb.iblock = ul_trans_u64_le(ufs->ilist.now.block);
    ufs->sb.zblock_max = ufs->sb.zblock = ul_trans_u64_le(ufs->zlist.now.block);
    ec = ufs_jornal_add(&ufs->jornal, &ufs->sb, UFS_BNUM_SB, 0, sizeof(ufs->sb), UFS_JORNAL_ADD_COPY);
    if(ufs_unlikely(ec)) goto fail_return2;
    ec = ufs_sync(ufs);
    if(ufs_unlikely(ec)) goto fail_return2;

    ufs->sb.iblock_max = ul_trans_u64_le(ufs->sb.iblock_max);
    ufs->sb.iblock = ul_trans_u64_le(ufs->sb.iblock);
    ufs->sb.zblock_max = ul_trans_u64_le(ufs->sb.zblock_max);
    ufs->sb.zblock = ul_trans_u64_le(ufs->sb.zblock);

    do {
        ufs_inode_t inode;
        inode.nlink = 1;
        inode.mode = UFS_S_IFDIR | 0777;
        inode.size = 0;
        inode.blocks = 0;
        inode.ctime = ufs_time(0);
        inode.mtime = inode.ctime;
        inode.atime = inode.atime;
        inode.uid = 0;
        inode.gid = 0;
        memset(inode.zones, 0, sizeof(inode.zones));
        ec = _write_inode_direct(&ufs->jornal, &inode, UFS_INUM_ROOT);
        if(ufs_unlikely(ec)) goto fail_return2;
    } while(0);

    *pufs = ufs;
    return 0;

fail_return2:
    ufs_jornal_deinit(&ufs->jornal);
fail_return:
    ufs_free(ufs);
    return ec;
}

UFS_API int ufs_sync(ufs_t* ufs) {
    int ec;
    ufs_transcation_t transcation;

    if(ufs_unlikely(ufs == NULL)) return UFS_EINVAL;

    ufs_transcation_init(&transcation, &ufs->jornal);

    ufs_ilist_lock(&ufs->ilist, &transcation);
    ec = ufs_ilist_sync(&ufs->ilist);
    ufs_ilist_unlock(&ufs->ilist);
    if(ufs_unlikely(ec)) goto do_return;
    ec = ufs_transcation_commit_all(&transcation);

    ufs_zlist_lock(&ufs->zlist, &transcation);
    ec = ufs_zlist_sync(&ufs->zlist);
    ufs_zlist_unlock(&ufs->zlist);
    if(ufs_unlikely(ec)) goto do_return;
    ec = ufs_transcation_commit_all(&transcation);

    ufs_fileset_sync(&ufs->fileset);
    
    ec = ufs_jornal_sync(&ufs->jornal);

do_return:
    ufs_transcation_deinit(&transcation);
    return ec;
}

UFS_API void ufs_destroy(ufs_t* ufs) {
    if(ufs_unlikely(ufs == NULL)) return;
    ufs_sync(ufs);
    ufs_fileset_deinit(&ufs->fileset);
    ufs_free(ufs);
}

UFS_API int ufs_statvfs(ufs_t* ufs, ufs_statvfs_t* stat) {
    if(ufs_unlikely(ufs == NULL || stat == NULL)) return UFS_EINVAL;

    stat->f_bsize = UFS_BLOCK_SIZE;
    stat->f_namemax = UFS_NAME_MAX;

    stat->f_blocks = ufs->sb.zblock_max;
    ufs_zlist_lock(&ufs->zlist, NULL);
    stat->f_bavail = stat->f_bfree = ufs->zlist.now.block;
    ufs_zlist_unlock(&ufs->zlist);

    stat->f_files = ufs->sb.iblock_max;
    ufs_ilist_lock(&ufs->ilist, NULL);
    stat->f_favail = stat->f_ffree = ufs->ilist.now.block;
    ufs_ilist_unlock(&ufs->ilist);

    return 0;
}
