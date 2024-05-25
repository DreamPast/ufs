#include "libufs_internel.h"

static void _trans_ilist(_ufs_ilist_item_t* item) {
    int i;
    item->next = ul_trans_i64_le(item->next);
    for(i = 0; i < UFS_ILIST_ENTRY_NUM_MAX; ++i)
        item->stack[i] = ul_trans_u64_le(item->stack[i]);
}
static _ufs_ilist_item_t* _todisk_alloc(const _ufs_ilist_item_t* item) {
    int i;
    _ufs_ilist_item_t* ret = ul_reinterpret_cast(_ufs_ilist_item_t*, ufs_malloc(UFS_BLOCK_SIZE));
    if(ufs_unlikely(ret == NULL)) return ret;
    ret->next = ul_trans_u64_le(item->next);
    for(i = 0; i < item->num; ++i)
        ret->stack[i] = ul_trans_u64_le(item->stack[i]);
    for(; i < UFS_ILIST_ENTRY_NUM_MAX; ++i)
        ret->stack[i] = 0;
    return ret;
}

static int _read_ilist(_ufs_ilist_item_t* ufs_restrict item, ufs_transcation_t* ufs_restrict transcation, uint64_t inum) {
    int ec;
    int num = 0;
    ec = ufs_transcation_read(transcation, item, inum / UFS_INODE_PER_BLOCK,
        (inum % UFS_INODE_PER_BLOCK) * UFS_INODE_DISK_SIZE, UFS_INODE_DISK_SIZE);
    if(ufs_unlikely(ec)) return ec;

    _trans_ilist(item);
    for(num = UFS_ILIST_ENTRY_NUM_MAX; num > 0 && !item->stack[num - 1]; --num) { }
    item->num = num;
    item->inum = inum;
    return 0;
}
static int _write_ilist(const _ufs_ilist_item_t* ufs_restrict item, ufs_transcation_t* ufs_restrict transcation, uint64_t inum) {
    _ufs_ilist_item_t* ret;
    ret = _todisk_alloc(item);
    if(ufs_unlikely(ret == NULL)) return UFS_ENOMEM;
    return ufs_transcation_add(transcation, ret, inum / UFS_INODE_PER_BLOCK, UFS_JORNAL_ADD_MOVE,
        (inum % UFS_INODE_PER_BLOCK) * UFS_INODE_DISK_SIZE, UFS_INODE_DISK_SIZE);
}

static int _rewind_ilist(_ufs_ilist_t* ufs_restrict ilist, ufs_transcation_t* ufs_restrict transcation, uint64_t inum) {
    int ec, i, n;
    _ufs_ilist_item_t tmp;

    n = 0;
    while(inum && n < UFS_ILIST_CACHE_LIST_LIMIT / 2) {
        ec = _read_ilist(ilist->item + n, transcation, inum);
        if(ufs_unlikely(ec)) return ec;
        inum = ilist->item[n].next;
        ++n;
    }
    ilist->top = n;
    ilist->stop = n;

    // 倒置缓存
    --n; i = 0;
    while(i < n) {
        tmp = ilist->item[i];
        ilist->item[i++] = ilist->item[n];
        ilist->item[n--] = tmp;
    }
    return 0;
}

static int _write_multi_ilist(const _ufs_ilist_t* ufs_restrict ilist, ufs_transcation_t* ufs_restrict transcation, int i, int n) {
    int ec;
    for(; i < n; ++i) {
        ec = _write_ilist(ilist->item + i, transcation, ilist->item[i].inum);
        if(ufs_unlikely(ec)) return ec;
    }
    return 0;
}

UFS_HIDDEN int ufs_ilist_init(ufs_ilist_t* ilist, uint64_t start, uint64_t block) {
    int ec;

    ilist->bnum = start;
    ilist->transcation = NULL;
    ulatomic_spinlock_init(&ilist->lock);

    ilist->now.block = block;
    ec = _rewind_ilist(&ilist->now, ilist->transcation, start);
    if(ufs_unlikely(ec)) return ec;
    if(ufs_unlikely(ilist->now.top == 0)) { // 内存中必须至少滞留一个块
        ilist->now.item[0].next = 0;
        ilist->now.item[0].num = 0;
        ilist->now.block = 0;
        ilist->now.top = 1;
        ilist->now.stop = 0;
    }

    ilist->backup.item[0].next = 0;
    ilist->backup.item[0].num = 0;
    ilist->backup.block = 0;
    ilist->backup.top = 1;
    ilist->backup.stop = 0;
    return 0;
}
UFS_HIDDEN void ufs_ilist_deinit(ufs_ilist_t* ilist) {
    (void)ilist;
}
UFS_HIDDEN int ufs_ilist_create_empty(ufs_ilist_t* ilist, uint64_t start) {
    ilist->bnum = start;
    ilist->transcation = NULL;
    ulatomic_spinlock_init(&ilist->lock);

    ilist->now.item[0].next = 0;
    ilist->now.item[0].num = 0;
    ilist->now.block = 0;
    ilist->now.top = 1;
    ilist->now.stop = 0;

    ilist->backup.item[0].next = 0;
    ilist->backup.item[0].num = 0;
    ilist->backup.block = 0;
    ilist->backup.top = 1;
    ilist->backup.stop = 0;
    return 0;
}

UFS_HIDDEN int ufs_ilist_sync(ufs_ilist_t* ilist) {
    int ec;
    uint64_t block = ul_trans_u64_le(ilist->now.block);

    ufs_assert(ilist->now.top > 0);
    ec = _write_multi_ilist(&ilist->now, ilist->transcation, ilist->now.stop, ilist->now.top - 1);
    if(ufs_unlikely(ec)) return ec;
    ec = _write_ilist(ilist->now.item + ilist->now.top - 1, ilist->transcation, ilist->bnum);
    if(ufs_unlikely(ec)) return ec;
    ec = ufs_transcation_add(ilist->transcation, &block, UFS_BNUM_SB, offsetof(ufs_sb_t, iblock), 8, UFS_JORNAL_ADD_COPY);
    if(ufs_unlikely(ec)) return ec;
    ilist->now.stop = ilist->now.top;
    return 0;
}
UFS_HIDDEN int ufs_ilist_pop(ufs_ilist_t* ufs_restrict ilist, uint64_t* ufs_restrict pinum) {
    int ec;
    int n = ilist->now.top;

    ufs_assert(n > 0);
    if(ilist->now.item[n - 1].num != 0) { // 栈还足够
        _ufs_ilist_item_t* item = ilist->now.item + n - 1;
        *pinum = item->stack[--item->num];
        --ilist->now.block;
        ilist->now.stop = ufs_min(ilist->now.stop, n - 1);
        return 0;
    }
    if(n > 1) { // 内存中还存有多余的链表
        *pinum = ilist->now.item[n - 1].next;
        ilist->now.top = --n;
        --ilist->now.block;
        ilist->now.stop = ufs_min(ilist->now.stop, ilist->now.top);
        return 0;
    }
    if(ilist->now.item[0].next == 0) // 磁盘耗尽
        return UFS_ENOSPC;
    *pinum = ilist->now.item[0].next;
    ec = _rewind_ilist(&ilist->now, ilist->transcation, *pinum);
    if(ufs_unlikely(ec)) { *pinum = 0; return ec; }
    --ilist->now.block;
    return 0;
}
UFS_HIDDEN int ufs_ilist_push(ufs_ilist_t* ilist, uint64_t inum) {
    int ec;
    int n = ilist->now.top;

    ufs_assert(n > 0);
    if(ilist->now.item[n - 1].num != UFS_ILIST_ENTRY_NUM_MAX) {  // 栈还足够
        _ufs_ilist_item_t* item = ilist->now.item + n - 1;
        item->stack[item->num++] = inum;
        ++ilist->now.block;
        ilist->now.stop = ufs_min(ilist->now.stop, n - 1);
        return 0;
    }
    if(n == UFS_ILIST_CACHE_LIST_LIMIT) { // 内存中空间不足，我们写回链表
        ec = _write_multi_ilist(&ilist->now, ilist->transcation, 0, UFS_ILIST_CACHE_LIST_LIMIT);
        if(ufs_unlikely(ec)) return ec;
        memmove(ilist->now.item, ilist->now.item + UFS_ILIST_CACHE_LIST_LIMIT / 2,
            (UFS_ILIST_CACHE_LIST_LIMIT - UFS_ILIST_CACHE_LIST_LIMIT / 2) / 2 * sizeof(ilist->now.item[0]));
        n = UFS_ILIST_CACHE_LIST_LIMIT / 2;
    }
    ilist->now.item[n - 1].inum = ilist->now.item[n].next = inum;
    ilist->now.item[n].num = 0;
    ilist->now.top = ++n;
    ++ilist->now.block;
    ilist->now.stop = ufs_min(ilist->now.stop, n - 2);
    return 0;
}

UFS_HIDDEN void ufs_ilist_debug(const ufs_ilist_t* ilist, FILE* fp) {
    fprintf(fp, "ilist [%p]\n", ufs_const_cast(void*, ilist));
    fprintf(fp, "\ttop bnum: %" PRIu64 "\n", ilist->bnum);
    fprintf(fp, "\ttranscation: [%p]\n", ufs_const_cast(void*, ilist->transcation));

    fprintf(fp, "\tavailable: %" PRIu64 "\n", ilist->now.block);
    fprintf(fp, "\tcached length: %d\n", ilist->now.top);
    fprintf(fp, "\tsynced length: %d\n", ilist->now.stop);

    fprintf(fp, "\t<backup>available: %" PRIu64 "\n", ilist->now.block);
    fprintf(fp, "\t<backup>cached length: %d\n", ilist->now.top);
    fprintf(fp, "\t<backup>synced length: %d\n", ilist->now.stop);
}
