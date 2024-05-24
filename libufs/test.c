#include "libufs_internel.h"

#include <stdio.h>
void _errabort(int line, int ec) {
    if(ec) {
        fprintf(stderr, "Line %d: [%d] %s\n", line, ec, ufs_strerror(ec));
        exit(1);
    }
}
#define errabort(ec) _errabort(__LINE__, (ec))

int main() {
    int ec;

    ufs_t ufs;
    errabort(ufs_fd_open_file(&ufs.fd, "a.bin"));
    errabort(ufs_jornal_init(&ufs.jornal, ufs.fd));
    errabort(ufs_ilist_create_empty(&ufs.ilist, UFS_BNUM_ILIST));
    errabort(ufs_zlist_create_empty(&ufs.zlist, UFS_BNUM_ZLIST));

    int tot = 100;
    uint64_t x = UFS_BNUM_START;
    while(tot--) {
        errabort(ufs_zlist_push(&ufs.zlist, x++));
    }

    tot = 100;
    while(tot--) {
        for(int i = 0; i < UFS_INODE_PER_BLOCK; ++i)
            errabort(ufs_ilist_push(&ufs.ilist, x * UFS_INODE_PER_BLOCK + i));
        ++x;
    }


    ufs_minode_t minode;
    errabort(ufs_minode_create(&ufs, &minode, NULL));

    ufs_minode_debug(&minode, stdout);

    errabort(ufs_minode_sync(&minode, 0));
    errabort(ufs_jornal_sync(&ufs.jornal));

    char s[] = "This is a text";
    size_t writen;
    errabort(ufs_minode_pwrite(&minode, NULL, s, sizeof(s), 44444444, &writen));
    printf("writen: %zu\n", writen);

    ufs_minode_debug(&minode, stdout);
    ufs_zlist_debug(&ufs.zlist, stdout);
    ufs_ilist_debug(&ufs.ilist, stdout);
    
    char t[sizeof(s)];
    size_t read;
    errabort(ufs_minode_pread(&minode, NULL, t, sizeof(t), 44444444, &read));
    printf("writen: %zu\n", read);


    return 0;
}