#include "libufs_internel.h"

static void _trans_blist(_blist_item_t* item) {
    int i;
    item->next = ul_trans_i64_le(item->next);
    for(i = 0; i < UFS_BLIST_ENTRY_NUM_MAX; ++i)
        item->stack[i] = ul_trans_u64_le(item->stack[i]);
}
static _blist_item_t* _todisk_alloc(const _blist_item_t* item) {
    int i;
    _blist_item_t* ret = ul_reinterpret_cast(_blist_item_t*, ufs_malloc(UFS_BLOCK_SIZE));
    if(ul_unlikely(ret == NULL)) return ret;
    ret->next = ul_trans_u64_le(item->next);
    for(i = 0; i < item->num; ++i)
        ret->stack[i] = ul_trans_u64_le(item->stack[i]);
    for(; i < UFS_BLIST_ENTRY_NUM_MAX; ++i)
        ret->stack[i] = 0;
    return ret;
}

static int _read_blist(_blist_item_t* item, ufs_bcache_t* bcache, uint64_t bnum) {
    int ec;
    int num = 0;
    ec = ufs_bcache_read(bcache, item, bnum);
    if(ul_unlikely(ec)) return ec;
    if(ul_unlikely(bnum == 0)) return UFS_ERROR_BROKEN_DISK;

    _trans_blist(item);
    for(num = UFS_BLIST_ENTRY_NUM_MAX; num > 0 && !item->stack[num - 1]; --num) { }
    item->num = num;
    item->bnum = bnum;
    return 0;
}
static int _write_blist(const _blist_item_t* item, ufs_bcache_t* bcache, uint64_t bnum) {
    _blist_item_t* ret;
    ret = _todisk_alloc(item);
    if(ul_unlikely(ret == NULL)) return ENOMEM;
    return ufs_bcache_add(bcache, ret, bnum, UFS_BCACHE_ADD_MOVE | UFS_BCACHE_ADD_JORNAL);
}

static int _rewind_blist(ufs_blist_t* blist, uint64_t bnum) {
    int ec, i, n;
    _blist_item_t tmp;

    n = 0;
    while(bnum && n < UFS_BLIST_CACHE_LIST_LIMIT / 2) {
        ec = _read_blist(blist->item + n, blist->bcache, bnum);
        if(ul_unlikely(ec)) return ec;
        bnum = blist->item[n].next;
        ++n;
    }
    blist->n = n;

    // 倒置缓存
    --n; i = 0;
    while(i < n) {
        tmp = blist->item[i];
        blist->item[i++] = blist->item[n];
        blist->item[n--] = tmp;
    }
    return 0;
}

static int _write_multi_blist(const ufs_blist_t* blist, int i, int n) {
    int ec;
    while(i < n) {
        ec = _write_blist(blist->item + i, blist->bcache, blist->item[i].bnum);
        if(ul_unlikely(ec)) return ec;
    }
    return 0;
}


static int _blist_sync(ufs_blist_t* blist) {
    int ec;

    ufs_assert(blist->n > 0);
    ec = _write_multi_blist(blist, 0, blist->n - 1);
    if(ul_unlikely(ec)) return ec;
    return _write_blist(blist->item + blist->n - 1, blist->bcache, blist->top_bnum);
}

static int _blist_pop(ufs_blist_t* blist, uint64_t* pbnum) {
    int ec;
    int n = blist->n;

    ufs_assert(n > 0);
    if(blist->item[n - 1].num != 0) { // 栈还足够
        _blist_item_t* item = blist->item + n - 1;
        *pbnum = item->stack[--item->num];
        return 0;
    }
    if(n > 1) { // 内存中还存有多余的链表
        *pbnum = blist->item[n - 1].next;
        blist->n = --n;
        return 0;
    }
    if(blist->item[0].next == 0) // 磁盘耗尽
        return ENOSPC;
    *pbnum = blist->item[0].next;
    ec = _rewind_blist(blist, *pbnum);
    return ec;
}

static int _blist_push(ufs_blist_t* blist, uint64_t bnum) {
    int ec;
    int n = blist->n;

    ufs_assert(n > 0);
    if(blist->item[n - 1].num != UFS_BLIST_ENTRY_NUM_MAX) {  // 栈还足够
        _blist_item_t* item = blist->item + n - 1;
        item->stack[item->num++] = bnum;
        return 0;
    }
    if(n == UFS_BLIST_CACHE_LIST_LIMIT) { // 内存中空间不足，我们写回一部分链表
        ec = _write_multi_blist(blist, 0, UFS_BLIST_CACHE_LIST_LIMIT / 2);
        if(ul_unlikely(ec)) return ec;
        memmove(blist->item, blist->item + UFS_BLIST_CACHE_LIST_LIMIT / 2, UFS_BLIST_CACHE_LIST_LIMIT);
        n = UFS_BLIST_CACHE_LIST_LIMIT / 2;
    }   
    blist->item[n - 1].bnum = blist->item[n].next = bnum;
    blist->item[n].num = 0;
    blist->n = ++n;
    return 0;
}


UFS_HIDDEN int ufs_blist_init(ufs_blist_t* blist, ufs_bcache_t* bcache, uint64_t start) {
    int ec;

    ulatomic_spinlock_init(&blist->lock);
    blist->bcache = bcache;
    blist->top_bnum = start;
    ec = _rewind_blist(blist, start);
    if(ul_unlikely(ec)) return ec;
    if(blist->n == 0) { // 内存中必须至少滞留一个块
        blist->item[0].next = 0;
        blist->item[0].num = 0;
        blist->n = 1;
    }
    return 0;
}
UFS_HIDDEN void ufs_blist_deinit(ufs_blist_t* blist) {
    (void)blist;
}
UFS_HIDDEN int ufs_blist_sync(ufs_blist_t* blist) {
    int ec;
    ulatomic_spinlock_lock(&blist->lock);
    ec = _blist_sync(blist);
    ulatomic_spinlock_unlock(&blist->lock);
    return ec;
}
UFS_HIDDEN int ufs_blist_pop(ufs_blist_t* blist, uint64_t* pbnum) {
    int ec;
    ulatomic_spinlock_lock(&blist->lock);
    ec = _blist_pop(blist, pbnum);
    ulatomic_spinlock_unlock(&blist->lock);
    return ec;
}
UFS_HIDDEN int ufs_blist_push(ufs_blist_t* blist, uint64_t bnum) {
    int ec;
    ulatomic_spinlock_lock(&blist->lock);
    ec = _blist_push(blist, bnum);
    ulatomic_spinlock_unlock(&blist->lock);
    return ec;
}
