#include "libufs_internel.h"

typedef struct _blist_item_t {
    int64_t num; // 剩余块的数量。在磁盘上如果num为负数表示到达链的结尾，否则表示链还有下一项
    uint64_t stack[UFS_BLIST_ENTRY_NUM_MAX];

    uint64_t bnum;
    int flag; // 1表示到达链的结尾
} _blist_item_t;

struct ufs_blist_t {
    _blist_item_t item[UFS_BLIST_STACK_MAX];
    ufs_bcache_t* bcache;
    int n;
    ulatomic_spinlock_t lock;
};

static void _trans_blist(_blist_item_t* item) {
    int i;
    item->num = ul_trans_i64_le(item->num);
    for(i = 0; i < UFS_BLIST_ENTRY_NUM_MAX; ++i)
        item->stack[i] = ul_trans_u64_le(item->stack[i]);
}
static _blist_item_t* _todisk_alloc(const _blist_item_t* item) {
    int i;
    _blist_item_t* ret = ul_reinterpret_cast(_blist_item_t*, ufs_malloc(UFS_BLOCK_SIZE));
    if(ul_unlikely(ret == NULL)) return ret;
    ret->num = ul_trans_i64_le(item->num);
    for(i = 0; i < UFS_BLIST_ENTRY_NUM_MAX; ++i)
        ret->stack[i] = ul_trans_u64_le(item->stack[i]);
    return ret;
}

static int _read_blist(_blist_item_t* item, ufs_fd_t* fd, uint64_t bnum) {
    int ec, i;
    ec = ufs_fd_pread_check(fd, item, UFS_BLOCK_SIZE, ufs_fd_offset(bnum));
    if(ul_unlikely(ec)) return ec;

    _trans_blist(item);
    if(item->num > UFS_BLIST_ENTRY_NUM_MAX) return UFS_ERROR_BROKEN_DISK;

    if(item->num < 0) {
        item->flag = (ul_static_cast(uint64_t, item->num) & 0x8000000000000000) != 0;
        item->num = item->num & 0x7FFFFFFFFFFFFFFF;
    }
    for(i = ul_static_cast(int, item->num); i >= 0; --i) {
        if(item->stack[i] != 0) break;
    }
    if(i < 0) return UFS_ERROR_BROKEN_DISK;

    item->num = ul_static_cast(int64_t, i);
    item->bnum = bnum;
    return 0;
}
static int _write_blist(const _blist_item_t* item, ufs_bcache_t* bcache) {
    _blist_item_t* ret;
    ret = _todisk_alloc(item);
    if(ul_unlikely(ret == NULL)) return ENOMEM;
    ret->num = ul_static_cast(int64_t, ul_static_cast(uint64_t, item->num) | (item->flag ? 0u : 0x8000000000000000u));
    return ufs_bcache_add(bcache, ret, item->bnum, UFS_BCACHE_ADD_MOVE);
}

static int _expand_blist(ufs_blist_t* blist) {
    int i = blist->n, ec = 0;
    if(ul_unlikely(i == 0)) return UFS_ERROR_BROKEN_DISK;
    while(i < UFS_BLIST_STACK_MAX && blist->item[i - 1].flag == 0) {
        if(ul_unlikely(blist->item[i - 1].num == 0)) return UFS_ERROR_BROKEN_DISK;
        ec = _read_blist(blist->item + i, blist->bcache->fd, blist->item[i - 1].stack[blist->item[i - 1].num - 1]);
        if(ul_unlikely(ec)) return ec;
    }
    blist->n = i;
    return 0;
}

UFS_HIDDEN int ufs_blist_create(ufs_blist_t** pblist, ufs_bcache_t* bcache, uint64_t start) {
    int ec;
    ufs_blist_t* blist = ul_reinterpret_cast(ufs_blist_t*, ufs_malloc(sizeof(ufs_blist_t)));
    if(ul_unlikely(blist == NULL)) return ENOMEM;
    blist->bcache = bcache;
    blist->n = 1;
    ec = _read_blist(blist->item, blist->bcache->fd, start);
    if(ul_unlikely(ec)) { ufs_free(blist); return ec; }
    ec = _expand_blist(blist);
    if(ul_unlikely(ec)) { ufs_free(blist); return ec; }
    *pblist = blist;
    return 0;
}
UFS_HIDDEN void ufs_blist_destroy(ufs_blist_t* blist) {
    (void)blist;
}
UFS_HIDDEN int ufs_blist_sync(ufs_blist_t* blist) {
    int i, ec = 0;
    ulatomic_spinlock_lock(&blist->lock);

    for(i = 0; i < blist->n; ++i) {
        ec = _write_blist(blist->item + i, blist->bcache);
        if(ul_unlikely(ec)) goto do_return;
    }

do_return:
    ulatomic_spinlock_unlock(&blist->lock);
    return ec;
}
UFS_HIDDEN int ufs_blist_pop(ufs_blist_t* blist, uint64_t* pbnum) {
    int i, ec = 0;
    ulatomic_spinlock_lock(&blist->lock);

    ec = _expand_blist(blist);
    if(ul_unlikely(ec)) goto do_return;

    for(i = blist->n; i > 0; --i) {
        if(blist->item[i - 1].num != 0) break;
        ec = _write_blist(blist->item + i - 1, blist->bcache);
    }
    blist->n = i;
    if(i) --blist->item[i - 1].num;
    ec = _expand_blist(blist);
    if(ul_unlikely(ec)) goto do_return;
    *pbnum = blist->item[blist->n - 1].stack[--blist->item[blist->n - 1].num];

do_return:
    ulatomic_spinlock_unlock(&blist->lock);
    return ec;
}
UFS_HIDDEN int ufs_blist_push(ufs_blist_t* blist, uint64_t bnum) {
    int i, ec = 0;
    uint64_t x;
    ulatomic_spinlock_lock(&blist->lock);
    
    ec = _expand_blist(blist);
    if(ul_unlikely(ec)) goto do_return;

    for(i = blist->n; i > 0; --i) {
        if(blist->item[i - 1].num != UFS_BLIST_ENTRY_NUM_MAX) break;
        ec = _write_blist(blist->item + i - 1, blist->bcache);
        x = blist->item[i - 1].bnum;
    }
    blist->n = i;
    if(i) blist->item[i - 1].stack[blist->item[i - 1].num++] = x;
    ec = _expand_blist(blist);
    if(ul_unlikely(ec)) goto do_return;
    blist->item[blist->n - 1].stack[blist->item[blist->n - 1].num++] = bnum;

    
do_return:
    ulatomic_spinlock_unlock(&blist->lock);
    return ec;
}
