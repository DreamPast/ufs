#include "libufs_internel.h"

#include <stdio.h>
void _errprint(int line, int ec) { fprintf(stderr, "Line %d: [%d] %s\n", line, ec, ufs_strerror(ec)); }
void _errabort(int line, int ec) { if(ec) { _errprint(line, ec); exit(1); } }
#define errprint(ec) _errprint(__LINE__, (ec))
#define errabort(ec) _errabort(__LINE__, (ec))

int main() {
    int ec;

    ufs_t ufs;
    errabort(ufs_fd_open_file(&ufs.fd, "a.bin"));
    errabort(ufs_jornal_init(&ufs.jornal, ufs.fd));
    errabort(ufs_ilist_create_empty(&ufs.ilist, UFS_BNUM_ILIST));
    errabort(ufs_zlist_create_empty(&ufs.zlist, UFS_BNUM_ZLIST));

    int tot = 10000;
    uint64_t x = UFS_BNUM_START;
    puts("zlist");
    do {
        ufs_transcation_t transcation;
        int t = 0;
        ufs_transcation_init(&transcation, &ufs.jornal);
        ufs_zlist_lock(&ufs.zlist, &transcation);
        while(tot--) {
            errabort(ufs_zlist_push(&ufs.zlist, x++));
            if(t++ >= 1000) {
                errabort(ufs_transcation_commit_all(&transcation));
                t = 0;
            }
        }
        errabort(ufs_transcation_commit_all(&transcation));
        ufs_zlist_unlock(&ufs.zlist);
        ufs_transcation_deinit(&transcation);
    } while(0);
    ufs_zlist_debug(&ufs.zlist, stdout);

    tot = 1;
    puts("ilist");
    do {
        ufs_transcation_t transcation;
        ufs_transcation_init(&transcation, &ufs.jornal);
        ufs_ilist_lock(&ufs.ilist, &transcation);
        while(tot--) {
            for(int i = 0; i < UFS_INODE_PER_BLOCK; ++i)
                errabort(ufs_ilist_push(&ufs.ilist, x * UFS_INODE_PER_BLOCK + i));
            ++x;
        }
        errabort(ufs_transcation_commit_all(&transcation));
        ufs_ilist_unlock(&ufs.ilist);
        ufs_transcation_deinit(&transcation);
    } while(0);

    ufs_minode_t minode;
    errabort(ufs_minode_create(&ufs, &minode, NULL));

    unsigned char c = 0;
    uint64_t off = 0;
    uint64_t writen;
    for(;;) {
        ec = ufs_minode_pwrite(&minode, NULL, &c, sizeof(c), off, &writen);
        off += UFS_BLOCK_SIZE;
        ++c;
        if(ec) break;
    }
    errprint(ec);
    ufs_minode_debug(&minode, stdout);
    ufs_zlist_debug(&ufs.zlist, stdout);

    errabort(ufs_minode_sync(&minode, 0));
    errabort(ufs_jornal_sync(&ufs.jornal));
    return 0;
}
