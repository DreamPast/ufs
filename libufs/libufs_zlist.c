#include "libufs_internel.h"

static void _trans_zlist(_zlist_item_t* item) {
    int i;
    item->next = ul_trans_i64_le(item->next);
    for(i = 0; i < UFS_ZLIST_ENTRY_NUM_MAX; ++i)
        item->stack[i] = ul_trans_u64_le(item->stack[i]);
}
static _zlist_item_t* _todisk_alloc(const _zlist_item_t* item) {
    int i;
    _zlist_item_t* ret = ul_reinterpret_cast(_zlist_item_t*, ufs_malloc(UFS_BLOCK_SIZE));
    if(ul_unlikely(ret == NULL)) return ret;
    ret->next = ul_trans_u64_le(item->next);
    for(i = 0; i < item->num; ++i)
        ret->stack[i] = ul_trans_u64_le(item->stack[i]);
    for(; i < UFS_ZLIST_ENTRY_NUM_MAX; ++i)
        ret->stack[i] = 0;
    return ret;
}

static int _read_zlist(_zlist_item_t* ufs_restrict item, ufs_bcache_t* ufs_restrict bcache, uint64_t znum) {
    int ec;
    int num = 0;
    ec = ufs_bcache_read_block(bcache, item, znum);
    if(ul_unlikely(ec)) return ec;
    if(ul_unlikely(znum == 0)) return UFS_ERROR_BROKEN_DISK;

    _trans_zlist(item);
    for(num = UFS_ZLIST_ENTRY_NUM_MAX; num > 0 && !item->stack[num - 1]; --num) { }
    item->num = num;
    item->znum = znum;
    return 0;
}
static int _write_zlist(const _zlist_item_t* ufs_restrict item, ufs_bcache_t* ufs_restrict bcache, uint64_t znum) {
    _zlist_item_t* ret;
    ret = _todisk_alloc(item);
    if(ul_unlikely(ret == NULL)) return ENOMEM;
    return ufs_bcache_add_block(bcache, ret, znum, UFS_BCACHE_ADD_MOVE);
}

static int _rewind_zlist(ufs_zlist_t* zlist, uint64_t znum) {
    int ec, i, n;
    _zlist_item_t tmp;

    n = 0;
    while(znum && n < UFS_ZLIST_CACHE_LIST_LIMIT / 2) {
        ec = _read_zlist(zlist->item + n, zlist->bcache, znum);
        if(ul_unlikely(ec)) return ec;
        znum = zlist->item[n].next;
        ++n;
    }
    zlist->n = n;

    // 倒置缓存
    --n; i = 0;
    while(i < n) {
        tmp = zlist->item[i];
        zlist->item[i++] = zlist->item[n];
        zlist->item[n--] = tmp;
    }
    return 0;
}

static int _write_multi_zlist(const ufs_zlist_t* zlist, int i, int n) {
    int ec;
    while(i < n) {
        ec = _write_zlist(zlist->item + i, zlist->bcache, zlist->item[i].znum);
        if(ul_unlikely(ec)) return ec;
    }
    return 0;
}

UFS_HIDDEN int ufs_zlist_init(ufs_zlist_t* zlist, ufs_bcache_t* bcache, uint64_t start) {
    int ec;
    zlist->bcache = bcache;
    zlist->top_znum = start;
    ec = _rewind_zlist(zlist, start);
    if(ul_unlikely(ec)) return ec;
    if(ul_unlikely(zlist->n == 0)) { // 内存中必须至少滞留一个块
        zlist->item[0].next = 0;
        zlist->item[0].num = 0;
        zlist->n = 1;
    }
    ulatomic_spinlock_init(&zlist->lock);
    return 0;
}
UFS_HIDDEN void ufs_zlist_deinit(ufs_zlist_t* zlist) {
    (void)zlist;
}

UFS_HIDDEN int ufs_zlist_sync_nolock(ufs_zlist_t* zlist) {
    int ec;

    ufs_assert(zlist->n > 0);
    ec = _write_multi_zlist(zlist, 0, zlist->n - 1);
    if(ul_unlikely(ec)) return ec;
    return _write_zlist(zlist->item + zlist->n - 1, zlist->bcache, zlist->top_znum);
}
UFS_HIDDEN int ufs_zlist_pop_nolock(ufs_zlist_t* ufs_restrict zlist, uint64_t* ufs_restrict pznum) {
    int ec;
    int n = zlist->n;

    ufs_assert(n > 0);
    if(zlist->item[n - 1].num != 0) { // 栈还足够
        _zlist_item_t* item = zlist->item + n - 1;
        *pznum = item->stack[--item->num];
        return 0;
    }
    if(n > 1) { // 内存中还存有多余的链表
        *pznum = zlist->item[n - 1].next;
        zlist->n = --n;
        return 0;
    }
    if(zlist->item[0].next == 0) // 磁盘耗尽
        return ENOSPC;
    *pznum = zlist->item[0].next;
    ec = _rewind_zlist(zlist, *pznum);
    return ec;
}
UFS_HIDDEN int ufs_zlist_push_nolock(ufs_zlist_t* zlist, uint64_t znum) {
    int ec;
    int n = zlist->n;

    ufs_assert(n > 0);
    if(zlist->item[n - 1].num != UFS_ZLIST_ENTRY_NUM_MAX) {  // 栈还足够
        _zlist_item_t* item = zlist->item + n - 1;
        item->stack[item->num++] = znum;
        return 0;
    }
    if(n == UFS_ZLIST_CACHE_LIST_LIMIT) { // 内存中空间不足，我们写回一部分链表
        ec = _write_multi_zlist(zlist, 0, UFS_ZLIST_CACHE_LIST_LIMIT / 2);
        if(ul_unlikely(ec)) return ec;
        memmove(zlist->item, zlist->item + UFS_ZLIST_CACHE_LIST_LIMIT / 2, UFS_ZLIST_CACHE_LIST_LIMIT);
        n = UFS_ZLIST_CACHE_LIST_LIMIT / 2;
    }
    zlist->item[n - 1].znum = zlist->item[n].next = znum;
    zlist->item[n].num = 0;
    zlist->n = ++n;
    return 0;
}

UFS_HIDDEN int ufs_zlist_sync(ufs_zlist_t* zlist) {
    int ec;
    ulatomic_spinlock_lock(&zlist->lock);
    ec = ufs_zlist_sync_nolock(zlist);
    ulatomic_spinlock_unlock(&zlist->lock);
    return ec;
}
UFS_HIDDEN int ufs_zlist_pop(ufs_zlist_t* ufs_restrict zlist, uint64_t* ufs_restrict pznum) {
    int ec;
    ulatomic_spinlock_lock(&zlist->lock);
    ec = ufs_zlist_pop_nolock(zlist, pznum);
    ulatomic_spinlock_unlock(&zlist->lock);
    return ec;
}
UFS_HIDDEN int ufs_zlist_push(ufs_zlist_t* zlist, uint64_t znum) {
    int ec;
    ulatomic_spinlock_lock(&zlist->lock);
    ec = ufs_zlist_push_nolock(zlist, znum);
    ulatomic_spinlock_unlock(&zlist->lock);
    return ec;
}
UFS_HIDDEN uint64_t ufs_calc_zlist_available(uint64_t num) {
    if(ul_unlikely(num == 0)) return 0;
    return num - (num - 1) / ul_static_cast(uint64_t, UFS_ZLIST_ENTRY_NUM_MAX);
}
