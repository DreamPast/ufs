#include "libufs.h"
#include <inttypes.h>

#include <stdio.h>
static void _errprint(int line, int ec) { fprintf(stderr, "Line %d: [%d] %s\n", line, ec, ufs_strerror(ec)); }
static void _errabort(int line, int ec) { if(ec) { _errprint(line, ec); exit(1); } }
// #define errprint(ec) _errprint(__LINE__, (ec))
#define errabort(ec) _errabort(__LINE__, (ec))

static void list_dir(ufs_context_t* context, const char* path) {
    int ec;
    ufs_dir_t* dir;
    ufs_dirent_t dirent;
    errabort(ufs_opendir(context, &dir, path));
    printf("[%s]\n", path);
    for(;;) {
        ec = ufs_readdir(dir, &dirent);
        if(ec == UFS_ENOENT) break;
        errabort(ec);
        printf("\t%" PRIu64 "\t%s\n", dirent.d_ino, dirent.d_name);
    }

    ufs_closedir(dir);
}
static void print_stat(ufs_stat_t* stat, FILE* fp) {
    fprintf(fp, "\tinode: %" PRIu64 "\n", stat->st_ino);
    fprintf(fp, "\tmode: 0%" PRIo16 "\n", stat->st_mode);
    fprintf(fp, "\tlink: %" PRIu32 "\n", stat->st_nlink);

    fprintf(fp, "\tuid: %" PRIi32 "\n", stat->st_uid);
    fprintf(fp, "\tgid: %" PRIi32 "\n", stat->st_gid);

    fprintf(fp, "\tsize: %" PRIu64 "\n", stat->st_size);
    fprintf(fp, "\tblock size: %" PRIu64 "\n", stat->st_blksize);
    fprintf(fp, "\tblock: %" PRIu64 "\n", stat->st_blocks);

    fprintf(fp, "\tcreate time: "); ufs_ptime(stat->st_ctime, NULL, fp); fputc('\n', fp);
    fprintf(fp, "\tmodify time: "); ufs_ptime(stat->st_mtime, NULL, fp); fputc('\n', fp);
    fprintf(fp, "\taccess time: "); ufs_ptime(stat->st_atime, NULL, fp); fputc('\n', fp);

}

int main(void) {
    ufs_vfs_t* vfs;
    ufs_t* ufs;
    ufs_context_t context;
    ufs_file_t* file;
    ufs_stat_t stat;

    errabort(ufs_vfs_open_file(&vfs, "a.bin"));
    errabort(ufs_new_format(&ufs, vfs, 1024 * UFS_BLOCK_SIZE));
    // errabort(ufs_new(&ufs, vfs));

    context.ufs = ufs;
    context.uid = 0;
    context.gid = 0;
    context.umask = 0777;
    
    list_dir(&context, "/");

    errabort(ufs_open(&context, &file, "/temp", UFS_O_RDONLY | UFS_O_CREAT, 0664));
    ufs_close(file);

    errabort(ufs_symlink(&context, "/temp_soft", "/temp"));
    errabort(ufs_link(&context, "/temp_hard", "/temp"));
    errabort(ufs_mkdir(&context, "/dir", 0777));
    list_dir(&context, "/");

    errabort(ufs_link(&context, "/dir/temp_hard", "/temp"));
    list_dir(&context, "/dir");

    errabort(ufs_open(&context, &file, "/dir/temp_hard", UFS_O_RDWR, 0664));
    errabort(ufs_ftruncate(file, 1024));
    errabort(ufs_fallocate(file, 114514));
    errabort(ufs_fstat(file, &stat));
    puts("");
    print_stat(&stat, stdout);
    ufs_close(file);

    ufs_destroy(ufs);
    return 0;
}
