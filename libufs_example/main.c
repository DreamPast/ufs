#include "libufs.h"
#include <inttypes.h>

#include <stdio.h>
static void _errprint(int line, int ec) { fprintf(stderr, "Line %d: [%d] %s\n", line, ec, ufs_strerror(ec)); }
static void _errabort(int line, int ec) { if(ec) { _errprint(line, ec); exit(1); } }
#define errprint(ec) _errprint(__LINE__, (ec))
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

    fprintf(fp, "\tcreate time: "); ufs_ptime(stat->st_ctim, NULL, fp); fputc('\n', fp);
    fprintf(fp, "\tmodify time: "); ufs_ptime(stat->st_mtim, NULL, fp); fputc('\n', fp);
    fprintf(fp, "\taccess time: "); ufs_ptime(stat->st_atim, NULL, fp); fputc('\n', fp);
}

static char* concat_file_in_dir(const char* dir, const char* filename) {
    size_t dir_len, filename_len;
    char* ret;

    dir_len = strlen(dir);
    filename_len = strlen(filename);
    if(dir[dir_len - 1] == '/' || dir[dir_len - 1] == '\\') --dir_len;
    ret = (char*)malloc(dir_len + filename_len + 2);
    if(ret == NULL) errabort(UFS_ENOMEM);
    memcpy(ret, dir, dir_len);
    ret[dir_len] = '/';
    memcpy(ret + dir_len + 1, filename, filename_len);
    ret[dir_len + filename_len + 1] = 0;
    return ret;
}

static void list_dir_detail1(ufs_context_t* context, const char* path) {
    int ec;
    ufs_dir_t* dir;
    ufs_dirent_t dirent;
    char* npath;
    int i;

    errabort(ufs_opendir(context, &dir, path));
    printf("[%s]\n", path);
    for(;;) {
        ufs_stat_t stat;
        ufs_physics_addr_t addrinfo;

        ec = ufs_readdir(dir, &dirent);
        if(ec == UFS_ENOENT) break;
        errabort(ec);

        npath = concat_file_in_dir(path, dirent.d_name);
        errabort(ufs_stat(context, npath, &stat));
        errabort(ufs_physics_addr(context, npath, &addrinfo));
        free(npath);

        if(UFS_S_ISDIR(stat.st_mode)) putchar('d');
        else if(UFS_S_ISREG(stat.st_mode)) putchar('-');
        else if(UFS_S_ISLNK(stat.st_mode)) putchar('l');
        else putchar('?');
        putchar(stat.st_mode & UFS_S_IRUSR ? 'r' : '-');
        putchar(stat.st_mode & UFS_S_IWUSR ? 'w' : '-');
        putchar(stat.st_mode & UFS_S_IXUSR ? 'x' : '-');
        putchar(stat.st_mode & UFS_S_IRGRP ? 'r' : '-');
        putchar(stat.st_mode & UFS_S_IWGRP ? 'w' : '-');
        putchar(stat.st_mode & UFS_S_IXGRP ? 'x' : '-');
        putchar(stat.st_mode & UFS_S_IROTH ? 'r' : '-');
        putchar(stat.st_mode & UFS_S_IWOTH ? 'w' : '-');
        putchar(stat.st_mode & UFS_S_IXOTH ? 'x' : '-');
        putchar(' ');

        printf("%" PRIu32 "\t", stat.st_nlink);

        printf("U%" PRId32 "\t", stat.st_uid);
        printf("G%" PRId32 "\t", stat.st_gid);

        printf("%" PRIu64 "\t", stat.st_size);

        ufs_ptime(stat.st_mtim, NULL, stdout);
        putchar('\t');

        printf("%s\t", dirent.d_name);
        
        printf("[%" PRIu64 "]\t", addrinfo.inode_off);
        for(i = 0; i < 16; ++i) {
            printf("[%" PRIu64 "]", addrinfo.zone_off[i]);
        }
        putchar('\n');
    }

    ufs_closedir(dir);
}

int main(void) {
    ufs_vfs_t* vfs;
    ufs_t* ufs;
    ufs_context_t context;
    ufs_file_t* file;

    // 打开磁盘模拟文件
    errabort(ufs_vfs_open_file(&vfs, "a.bin"));
    // 格式化磁盘
    errabort(ufs_new_format(&ufs, vfs, 1024 * UFS_BLOCK_SIZE));

    // 设置上下文，这里我们直接将用户、组分别设置为0、0，umask使用初始值0。
    context.ufs = ufs;
    context.uid = 0;
    context.gid = 0;
    context.umask = 0;

    // 创建文件并写入一定内容
    do {
        static const char buf[] = "This is a simple text file.";
        errabort(ufs_creat(&context, &file, "a.txt", 0664));
        errabort(ufs_write(file, buf, sizeof(buf) - 1, NULL));
        errabort(ufs_close(file));
    } while(0);
    list_dir_detail1(&context, "/");

    // 创建空文件、符号链接、硬链接、文件夹
    do {
        errabort(ufs_open(&context, &file, "/temp", UFS_O_RDONLY | UFS_O_CREAT, 0664));
        ufs_close(file);
        errabort(ufs_symlink(&context, "/temp_soft", "/temp"));
        errabort(ufs_link(&context, "/temp_hard", "/temp"));
        errabort(ufs_mkdir(&context, "/dir", 0777));
        errabort(ufs_link(&context, "/dir/temp_hard", "/temp"));
    } while(0);
    list_dir_detail1(&context, "/");
    list_dir_detail1(&context, "/dir");

    // 打开文件，进行读入并检查其属性
    do {
        ufs_stat_t stat;
        char buf[64];
        size_t read_bytes;
        errabort(ufs_open(&context, &file, "a.txt", UFS_O_RDONLY, 0664));
        errabort(ufs_read(file, buf, sizeof(buf), &read_bytes));
        puts("context of \"a.txt\" is:");
        fwrite(buf, 1, read_bytes, stdout);
        putchar('\n');
        errabort(ufs_fstat(file, &stat));
        puts("state of \"a.txt\" is:");
        print_stat(&stat, stdout);
    } while(0);

    // 关闭磁盘文件
    ufs_destroy(ufs);
    return 0;
}
