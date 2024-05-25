#include "libufs_internel.h"
#include "libufs_file.c"

#include <stdio.h>
void _errprint(int line, int ec) { fprintf(stderr, "Line %d: [%d] %s\n", line, ec, ufs_strerror(ec)); }
void _errabort(int line, int ec) { if(ec) { _errprint(line, ec); exit(1); } }
#define errprint(ec) _errprint(__LINE__, (ec))
#define errabort(ec) _errabort(__LINE__, (ec))

void init_all(ufs_t* ufs) {
    errabort(ufs_jornal_init(&ufs->jornal, ufs->fd));
    errabort(ufs_ilist_create_empty(&ufs->ilist, UFS_BNUM_ILIST));
    errabort(ufs_zlist_create_empty(&ufs->zlist, UFS_BNUM_ZLIST));
    errabort(ufs_fileset_init(&ufs->fileset, ufs));
}
void init_ilist(ufs_t* ufs, int tot, uint64_t* pbnum) {
    ufs_transcation_t transcation;
    int t = 0;
    puts("ilist");
    ufs_transcation_init(&transcation, &ufs->jornal);
    ufs_ilist_lock(&ufs->ilist, &transcation);
    while(tot--) {
        for(unsigned i = 0; i < UFS_INODE_PER_BLOCK; ++i)
            errabort(ufs_ilist_push(&ufs->ilist, ul_static_cast(uint64_t, *pbnum * UFS_INODE_PER_BLOCK + i)));
        if(t++ >= 100) {
            errabort(ufs_transcation_commit_all(&transcation));
            t = 0;
        }
        ++(*pbnum);
    }
    errabort(ufs_transcation_commit_all(&transcation));
    ufs_ilist_unlock(&ufs->ilist);
    ufs_transcation_deinit(&transcation);
}
void init_zlist(ufs_t* ufs, int tot, uint64_t* pbnum) {
    ufs_transcation_t transcation;
    int t = 0;
    puts("zlist");
    ufs_transcation_init(&transcation, &ufs->jornal);
    ufs_zlist_lock(&ufs->zlist, &transcation);
    while(tot--) {
        errabort(ufs_zlist_push(&ufs->zlist, (*pbnum)++));
        if(t++ >= 1000) {
            errabort(ufs_transcation_commit_all(&transcation));
            t = 0;
        }
    }
    errabort(ufs_transcation_commit_all(&transcation));
    ufs_zlist_unlock(&ufs->zlist);
    ufs_transcation_deinit(&transcation);
}

int main(void) {
    ul_unused int ec;

    ufs_t ufs;
    errabort(ufs_fd_open_file(&ufs.fd, "a.bin"));
    init_all(&ufs);

    uint64_t x = UFS_BNUM_START;
    init_ilist(&ufs, 1, &x);
    ufs_ilist_push(&ufs.ilist, UFS_INUM_ROOT);
    init_zlist(&ufs, 1000, &x);

    ufs_minode_t root;
    ufs_minode_t node;
    errabort(ufs_minode_create(&ufs, &root, NULL));
    errabort(ufs_minode_create(&ufs, &node, NULL));

#if 1
    _dirent_t dirent;
    memset(&dirent, 0, sizeof(dirent));
    strcpy(dirent.name, "temp");
    dirent.inum = ul_trans_u64_le(node.inum);
    size_t writen;
    do {
        ufs_transcation_t transcation;
        ufs_transcation_init(&transcation, &ufs.jornal);
        root.inode.mode = 0777 | UFS_S_IFDIR;
        errabort(ufs_minode_pwrite(&root, &transcation, &dirent, sizeof(dirent), 0, &writen));
        errabort(ufs_transcation_commit_all(&transcation));
        ufs_transcation_deinit(&transcation);
    } while(0);
#endif

    ufs_context_t context;
    context.ufs = &ufs;
    context.uid = 0;
    context.gid = 0;
    context.umask = 0777;
    

    ufs_file_t* file;
    ec = ufs_open(&context, &file, "/temp", UFS_O_RDONLY, 0664);
    errabort(ec);
    ufs_file_debug(file, stdout);

    return 0;
}
