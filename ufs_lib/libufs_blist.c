#include "libufs_internel.h"
#include "ulatomic.h"

#define BLIST_EOF (0)
typedef struct _blist_item_t {
    uint64_t next;
    uint64_t num;
    uint64_t stack[UFS_BLIST_ENTRY_NUM_MAX];

    uint64_t bnum;
} _blist_item_t;

struct ufs_blist_t {
    _blist_item_t item[UFS_BLIST_STACK_MAX];
    int n;
};


UFS_HIDDEN void _trans_blist(_blist_item_t* item) {
    int i;
    item->next = ul_trans_u64_le(item->next);
    item->num = ul_trans_u64_le(item->num);
    for(i = 0; i < UFS_BLIST_ENTRY_NUM_MAX; ++i)
        item->stack[i] = ul_trans_u64_le(item->stack[i]);
}

UFS_HIDDEN int _read_blist(_blist_item_t* item, ufs_fd_t* fd, uint64_t bnum) {
    int ec, i;
    ec = ufs_fd_pread_check(fd, item, UFS_BLOCK_SIZE, ufs_fd_offset(bnum));
    if(ul_unlikely(ec)) return ec;

    _trans_blist(item);
    if(item->num > UFS_BLIST_ENTRY_NUM_MAX) return EINVAL;

    for(i = ul_static_cast(int, item->num); i >= 0; --i) {
        if(item->stack[i] != 0) break;
    }
    if(i < 0) return EINVAL;

    item->num = ul_static_cast(uint64_t, i);
    item->bnum = bnum;
    return 0;
}


static int _expand(ufs_blist_t* blist, ufs_fd_t* fd, uint64_t bnum) {
    int ec;
    int i = blist->n;
    while(bnum != BLIST_EOF) {
        if(ul_unlikely(i >= UFS_BLIST_STACK_MAX)) return EINVAL;
        ec = _read_blist(blist->item + i, fd, bnum);
        if(ul_unlikely(ec)) return ec;
        bnum = blist->item[i].next;
        ++i;
    }
    blist->n = i;
    return 0;
}

UFS_HIDDEN int ufs_create_blist(ufs_blist_t** pblist, ufs_fd_t* fd, uint64_t start) {
    int i, ec;
    ufs_blist_t* blist;
    blist = ul_reinterpret_cast(ufs_blist_t*, ufs_malloc(sizeof(ufs_blist_t)));
    if(ul_unlikely(blist == NULL)) return ENOMEM;

    blist->n = 0;
    ec = _expand(blist, fd, start);
    if(ul_unlikely(ec)) { ufs_free(blist); return ec; }
    *pblist = blist;
    return 0;
}
UFS_HIDDEN int ufs_destroy_blist(ufs_blist_t* blist);
UFS_HIDDEN int ufs_sync_blist(ufs_blist_t* blist, ufs_fd_t* fd, int need_back) {
    ufs_jornal_op_t ops[UFS_BLIST_STACK_MAX];
    int i;

    for(i = 0; i < blist->n; ++i) {
        _trans_blist(blist->item + i);
        
    }

    for(i = 0; i < blist->n; ++i) {
        _trans_blist(blist->item + i);
    }
}

UFS_HIDDEN int ufs_pop_blist(ufs_blist_t* blist, uint64_t* pbnum) {
    _blist_item_t* item = blist->item + (blist->n - 1);
    if(blist->n == 0) return ENOSPC;
    if(item->num != 0) {
        *pbnum = item->stack[--item->num];
        return 0;
    }

    for(;;) {
        if(blist->n <= 0) return ENOSPC;
        item = blist->item + (blist->n - 1);
        --blist->n;

    }



}
UFS_HIDDEN int ufs_push_blist(ufs_blist_t* blist, uint64_t bnum) {

}