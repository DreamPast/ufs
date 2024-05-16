#include "libufs_internel.h"
#include "ulendian.h"
#include "ulmtx.h"

static int _ufs_fd_pread_block_alloc(ufs_fd_t* fd, uint64_t bnum, char** paddr) {
    int ec;
    char* addr = ul_reinterpret_cast(char*, ufs_malloc(UFS_BLOCK_SIZE));
    if(addr == NULL) return ENOMEM;
    ec = ufs_fd_pread_check(fd, addr, UFS_BLOCK_SIZE, ufs_fd_offset(bnum));
    if(ul_unlikely(ec)) { ufs_free(addr); return ec; }
    else { *paddr = addr; return ec; }
}


// 成组链接法
#if 1
    #define UFS_BLIST_NUM_MAX (UFS_BLOCK_SIZE / 8 - 1)
    #define UFS_BLIST_STACK_NUM 10

    typedef struct _ufs_blist_item_t {
        uint64_t bnum;
        uint64_t stack[UFS_BLIST_NUM_MAX];
        unsigned num;
    } _ufs_blist_item_t;

    typedef struct _ufs_blist_t {
        _ufs_blist_item_t stack[UFS_BLIST_STACK_NUM];
        _ufs_blist_item_t* top;
    } _ufs_blist_t;

    static int __ufs_blist_read(_ufs_blist_item_t* item, ufs_fd_t* fd, uint64_t* pbnum) {
        uint64_t bnum = *pbnum;
        int ec = ufs_fd_pread_check(fd, &item->bnum, sizeof(item->bnum) + sizeof(item->stack), ufs_fd_offset(bnum));
        if(ul_unlikely(ec)) return ec;
        *pbnum = item->bnum;
        item->bnum = bnum;
        
    }

    static int _ufs_blist_init(_ufs_blist_t* blist, ufs_t* ufs, uint64_t bnum) {
        _ufs_blist_item_t* top = (blist->top = blist->stack);
        int i = 0, ec;
        while(bnum) {
            if(ul_unlikely(i++ > UFS_BLIST_STACK_NUM)) return EINVAL;
            
        }
    }
    static int _ufs_blist_sync(const _ufs_blist_t* blist, ufs_t* ufs) {

    }
#endif