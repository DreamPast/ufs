#include "libufs_internel.h"

static void _trans_ilist(_ilist_item_t* item) {
    int i;
    item->next = ul_trans_i64_le(item->next);
    for(i = 0; i < UFS_ILIST_ENTRY_NUM_MAX; ++i)
        item->stack[i] = ul_trans_u64_le(item->stack[i]);
}
static _ilist_item_t* _todisk_alloc(const _ilist_item_t* item) {
    int i;
    _ilist_item_t* ret = ul_reinterpret_cast(_ilist_item_t*, ufs_malloc(UFS_BLOCK_SIZE));
    if(ul_unlikely(ret == NULL)) return ret;
    ret->next = ul_trans_u64_le(item->next);
    for(i = 0; i < item->num; ++i)
        ret->stack[i] = ul_trans_u64_le(item->stack[i]);
    for(; i < UFS_ILIST_ENTRY_NUM_MAX; ++i)
        ret->stack[i] = 0;
    return ret;
}

static int _read_ilist(_ilist_item_t* ufs_restrict item, ufs_bcache_t* ufs_restrict bcache, uint64_t inum) {
    int ec;
    int num = 0;
    ec = ufs_bcache_read(bcache, item, inum / UFS_INODE_PER_BLOCK,
        (inum % UFS_INODE_PER_BLOCK) * UFS_INODE_DISK_SIZE, UFS_INODE_DISK_SIZE);
    if(ul_unlikely(ec)) return ec;
    if(ul_unlikely(inum == 0)) return UFS_ERROR_BROKEN_DISK;

    _trans_ilist(item);
    for(num = UFS_ILIST_ENTRY_NUM_MAX; num > 0 && !item->stack[num - 1]; --num) { }
    item->num = num;
    item->inum = inum;
    return 0;
}
static int _write_ilist(const _ilist_item_t* ufs_restrict item, ufs_bcache_t* ufs_restrict bcache, uint64_t inum) {
    _ilist_item_t* ret;
    ret = _todisk_alloc(item);
    if(ul_unlikely(ret == NULL)) return ENOMEM;
    return ufs_bcache_add(bcache, ret, inum / UFS_INODE_PER_BLOCK, UFS_BCACHE_ADD_MOVE,
        (inum % UFS_INODE_PER_BLOCK) * UFS_INODE_DISK_SIZE, UFS_INODE_DISK_SIZE);
}

static int _rewind_ilist(ufs_ilist_t* ilist, uint64_t inum) {
    int ec, i, n;
    _ilist_item_t tmp;

    n = 0;
    while(inum && n < UFS_ILIST_CACHE_LIST_LIMIT / 2) {
        ec = _read_ilist(ilist->item + n, ilist->bcache, inum);
        if(ul_unlikely(ec)) return ec;
        inum = ilist->item[n].next;
        ++n;
    }
    ilist->n = n;

    // 倒置缓存
    --n; i = 0;
    while(i < n) {
        tmp = ilist->item[i];
        ilist->item[i++] = ilist->item[n];
        ilist->item[n--] = tmp;
    }
    return 0;
}

static int _write_multi_ilist(const ufs_ilist_t* ilist, int i, int n) {
    int ec;
    while(i < n) {
        ec = _write_ilist(ilist->item + i, ilist->bcache, ilist->item[i].inum);
        if(ul_unlikely(ec)) return ec;
    }
    return 0;
}

UFS_HIDDEN int ufs_ilist_init(ufs_ilist_t* ufs_restrict ilist, ufs_bcache_t* ufs_restrict bcache, uint64_t start) {
    int ec;
    ilist->bcache = bcache;
    ilist->top_inum = start;
    ec = _rewind_ilist(ilist, start);
    if(ul_unlikely(ec)) return ec;
    if(ilist->n == 0) { // 内存中必须至少滞留一个块
        ilist->item[0].next = 0;
        ilist->item[0].num = 0;
        ilist->n = 1;
    }
    return 0;
}
UFS_HIDDEN void ufs_ilist_deinit(ufs_ilist_t* ilist) {
    (void)ilist;
}

UFS_HIDDEN int ufs_ilist_sync_nolock(ufs_ilist_t* ilist) {
    int ec;

    ufs_assert(ilist->n > 0);
    ec = _write_multi_ilist(ilist, 0, ilist->n - 1);
    if(ul_unlikely(ec)) return ec;
    return _write_ilist(ilist->item + ilist->n - 1, ilist->bcache, ilist->top_inum);
}
UFS_HIDDEN int ufs_ilist_pop_nolock(ufs_ilist_t* ufs_restrict ilist, uint64_t* ufs_restrict pinum) {
    int ec;
    int n = ilist->n;

    ufs_assert(n > 0);
    if(ilist->item[n - 1].num != 0) { // 栈还足够
        _ilist_item_t* item = ilist->item + n - 1;
        *pinum = item->stack[--item->num];
        return 0;
    }
    if(n > 1) { // 内存中还存有多余的链表
        *pinum = ilist->item[n - 1].next;
        ilist->n = --n;
        return 0;
    }
    if(ilist->item[0].next == 0) // 磁盘耗尽
        return ENOSPC;
    *pinum = ilist->item[0].next;
    ec = _rewind_ilist(ilist, *pinum);
    return ec;
}
UFS_HIDDEN int ufs_ilist_push_nolock(ufs_ilist_t* ilist, uint64_t inum) {
    int ec;
    int n = ilist->n;

    ufs_assert(n > 0);
    if(ilist->item[n - 1].num != UFS_ILIST_ENTRY_NUM_MAX) {  // 栈还足够
        _ilist_item_t* item = ilist->item + n - 1;
        item->stack[item->num++] = inum;
        return 0;
    }
    if(n == UFS_ILIST_CACHE_LIST_LIMIT) { // 内存中空间不足，我们写回一部分链表
        ec = _write_multi_ilist(ilist, 0, UFS_ILIST_CACHE_LIST_LIMIT / 2);
        if(ul_unlikely(ec)) return ec;
        memmove(ilist->item, ilist->item + UFS_ILIST_CACHE_LIST_LIMIT / 2, UFS_ILIST_CACHE_LIST_LIMIT);
        n = UFS_ILIST_CACHE_LIST_LIMIT / 2;
    }
    ilist->item[n - 1].inum = ilist->item[n].next = inum;
    ilist->item[n].num = 0;
    ilist->n = ++n;
    return 0;
}

UFS_HIDDEN int ufs_ilist_sync(ufs_ilist_t* ilist) {
    int ec;
    ulatomic_spinlock_lock(&ilist->lock);
    ec = ufs_ilist_sync_nolock(ilist);
    ulatomic_spinlock_unlock(&ilist->lock);
    return ec;
}
UFS_HIDDEN int ufs_ilist_pop(ufs_ilist_t* ilist, uint64_t* pinum) {
    int ec;
    ulatomic_spinlock_lock(&ilist->lock);
    ec = ufs_ilist_pop_nolock(ilist, pinum);
    ulatomic_spinlock_unlock(&ilist->lock);
    return ec;
}
UFS_HIDDEN int ufs_ilist_push(ufs_ilist_t* ilist, uint64_t inum) {
    int ec;
    ulatomic_spinlock_lock(&ilist->lock);
    ec = ufs_ilist_push_nolock(ilist, inum);
    ulatomic_spinlock_unlock(&ilist->lock);
    return ec;
}
UFS_HIDDEN uint64_t ufs_calc_ilist_available(uint64_t num) {
    if(ul_unlikely(num == 0)) return 0;
    return num - (num - 1) / ul_static_cast(uint64_t, UFS_ILIST_ENTRY_NUM_MAX);
}
