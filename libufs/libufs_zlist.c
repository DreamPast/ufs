#include "libufs_internel.h"

static void _trans_zlist(_ufs_zlist_item_t* item) {
    int i;
    item->next = ul_trans_i64_le(item->next);
    for(i = 0; i < UFS_ZLIST_ENTRY_NUM_MAX; ++i)
        item->stack[i] = ul_trans_u64_le(item->stack[i]);
}
static _ufs_zlist_item_t* _todisk_alloc(const _ufs_zlist_item_t* item) {
    int i;
    _ufs_zlist_item_t* ret = ul_reinterpret_cast(_ufs_zlist_item_t*, ufs_malloc(UFS_BLOCK_SIZE));
    if(ul_unlikely(ret == NULL)) return ret;
    ret->next = ul_trans_u64_le(item->next);
    for(i = 0; i < item->num; ++i)
        ret->stack[i] = ul_trans_u64_le(item->stack[i]);
    for(; i < UFS_ZLIST_ENTRY_NUM_MAX; ++i)
        ret->stack[i] = 0;
    return ret;
}

static int _read_zlist(_ufs_zlist_item_t* ufs_restrict item, ufs_transcation_t* ufs_restrict transcation, uint64_t znum) {
    int ec;
    int num = 0;
    ec = ufs_transcation_read_block(transcation, item, znum);
    if(ul_unlikely(ec)) return ec;

    _trans_zlist(item);
    for(num = UFS_ZLIST_ENTRY_NUM_MAX; num > 0 && !item->stack[num - 1]; --num) { }
    item->num = num;
    item->znum = znum;
    return 0;
}
static int _write_zlist(const _ufs_zlist_item_t* ufs_restrict item, ufs_transcation_t* ufs_restrict transcation, uint64_t znum) {
    _ufs_zlist_item_t* ret;
    ret = _todisk_alloc(item);
    if(ul_unlikely(ret == NULL)) return ENOMEM;
    return ufs_transcation_add_block(transcation, ret, znum, UFS_JORNAL_ADD_MOVE);
}

static int _rewind_zlist(_ufs_zlist_t* ufs_restrict zlist, ufs_transcation_t* ufs_restrict transcation, uint64_t znum) {
    int ec, i, top;
    _ufs_zlist_item_t tmp;

    top = 0;
    while(znum && top < UFS_ZLIST_CACHE_LIST_LIMIT / 2) {
        ec = _read_zlist(zlist->item + top, transcation, znum);
        if(ul_unlikely(ec)) return ec;
        znum = zlist->item[top].next;
        ++top;
    }
    zlist->top = top;
    zlist->stop = top;

    // 倒置缓存
    --top; i = 0;
    while(i < top) {
        tmp = zlist->item[i];
        zlist->item[i++] = zlist->item[top];
        zlist->item[top--] = tmp;
    }
    return 0;
}

static int _write_multi_zlist(const _ufs_zlist_t* ufs_restrict zlist, ufs_transcation_t* ufs_restrict transcation, int i, int n) {
    int ec;
    for(; i < n; ++i) {
        ec = _write_zlist(zlist->item + i, transcation, zlist->item[i].znum);
        if(ul_unlikely(ec)) return ec;
    }
    return 0;
}

UFS_HIDDEN int ufs_zlist_init(ufs_zlist_t* zlist, uint64_t start, uint64_t block) {
    int ec;

    zlist->bnum = start;
    zlist->transcation = NULL;
    ulatomic_spinlock_init(&zlist->lock);

    zlist->now.block = block;
    ec = _rewind_zlist(&zlist->now, zlist->transcation, start);
    if(ul_unlikely(ec)) return ec;
    if(ul_unlikely(zlist->now.top == 0)) { // 内存中必须至少滞留一个块
        zlist->now.item[0].next = 0;
        zlist->now.item[0].num = 0;
        zlist->now.block = 0;
        zlist->now.top = 1;
        zlist->now.stop = 0;
    }

    zlist->backup.item[0].next = 0;
    zlist->backup.item[0].num = 0;
    zlist->backup.block = 0;
    zlist->backup.top = 1;
    zlist->backup.stop = 0;
    return 0;
}
UFS_HIDDEN void ufs_zlist_deinit(ufs_zlist_t* zlist) {
    (void)zlist;
}
UFS_HIDDEN int ufs_zlist_create_empty(ufs_zlist_t* zlist, uint64_t start) {
    zlist->bnum = start;
    zlist->transcation = NULL;
    ulatomic_spinlock_init(&zlist->lock);

    zlist->now.item[0].next = 0;
    zlist->now.item[0].num = 0;
    zlist->now.block = 0;
    zlist->now.top = 1;
    zlist->now.stop = 0;

    zlist->backup.item[0].next = 0;
    zlist->backup.item[0].num = 0;
    zlist->backup.block = 0;
    zlist->backup.top = 1;
    zlist->backup.stop = 0;
    return 0;
}

UFS_HIDDEN int ufs_zlist_sync(ufs_zlist_t* zlist) {
    int ec;
    uint64_t block = ul_trans_u64_le(zlist->now.block);

    ufs_assert(zlist->now.top > 0);
    ec = _write_multi_zlist(&zlist->now, zlist->transcation, zlist->now.stop, zlist->now.top - 1);
    if(ul_unlikely(ec)) return ec;
    ec = _write_zlist(zlist->now.item + zlist->now.top - 1, zlist->transcation, zlist->bnum);
    if(ul_unlikely(ec)) return ec;
    zlist->now.stop = zlist->now.top;
    ec = ufs_transcation_add(zlist->transcation, &block, UFS_BNUM_SB, offsetof(ufs_sb_t, zblock), 8, UFS_JORNAL_ADD_COPY);
    if(ul_unlikely(ec)) return ec;
    return 0;
}
UFS_HIDDEN int ufs_zlist_pop(ufs_zlist_t* ufs_restrict zlist, uint64_t* ufs_restrict pznum) {
    int ec;
    int n = zlist->now.top;

    ufs_assert(n > 0);
    if(zlist->now.item[n - 1].num != 0) { // 栈还足够
        _ufs_zlist_item_t* item = zlist->now.item + n - 1;
        *pznum = item->stack[--item->num];
        --zlist->now.block;
        zlist->now.stop = ufs_min(zlist->now.stop, n - 1);
        return 0;
    }
    if(n > 1) { // 内存中还存有多余的链表
        *pznum = zlist->now.item[n - 1].next;
        zlist->now.top = --n;
        --zlist->now.block;
        zlist->now.stop = ufs_min(zlist->now.stop, zlist->now.top);
        return 0;
    }
    if(zlist->now.item[0].next == 0) // 磁盘耗尽
        return ENOSPC;
    *pznum = zlist->now.item[0].next;
    ec = _rewind_zlist(&zlist->now, zlist->transcation, *pznum);
    if(ul_unlikely(ec)) { *pznum = 0; return ec; }
    --zlist->now.block;
    return 0;
}
UFS_HIDDEN int ufs_zlist_push(ufs_zlist_t* zlist, uint64_t znum) {
    int ec;
    int n = zlist->now.top;

    ufs_assert(n > 0);
    if(zlist->now.item[n - 1].num != UFS_ZLIST_ENTRY_NUM_MAX) {  // 栈还足够
        _ufs_zlist_item_t* item = zlist->now.item + n - 1;
        item->stack[item->num++] = znum;
        ++zlist->now.block;
        zlist->now.stop = ufs_min(zlist->now.stop, n - 1);
        return 0;
    }
    if(n == UFS_ZLIST_CACHE_LIST_LIMIT) { // 内存中空间不足，我们写回链表
        ec = _write_multi_zlist(&zlist->now, zlist->transcation, 0, UFS_ZLIST_CACHE_LIST_LIMIT);
        if(ul_unlikely(ec)) return ec;
        memmove(zlist->now.item, zlist->now.item + UFS_ZLIST_CACHE_LIST_LIMIT / 2, UFS_ZLIST_CACHE_LIST_LIMIT);
        n = UFS_ZLIST_CACHE_LIST_LIMIT / 2;
    }
    zlist->now.item[n - 1].znum = zlist->now.item[n].next = znum;
    zlist->now.item[n].num = 0;
    zlist->now.top = ++n;
    ++zlist->now.block;
    zlist->now.stop = ufs_min(zlist->now.stop, n - 2);
    return 0;
}

UFS_HIDDEN void ufs_zlist_debug(const ufs_zlist_t* zlist, FILE* fp) {
    fprintf(fp, "zlist [%p]\n", ufs_const_cast(void*, zlist));
    fprintf(fp, "\ttop bnum: %" PRIu64 "\n", zlist->bnum);
    fprintf(fp, "\ttranscation: [%p]\n", ufs_const_cast(void*, zlist->transcation));

    fprintf(fp, "\tavailable: %" PRIu64 "\n", zlist->now.block);
    fprintf(fp, "\tcached length: %d\n", zlist->now.top);
    fprintf(fp, "\tsynced length: %d\n", zlist->now.stop);

    fprintf(fp, "\t<backup>available: %" PRIu64 "\n", zlist->now.block);
    fprintf(fp, "\t<backup>cached length: %d\n", zlist->now.top);
    fprintf(fp, "\t<backup>synced length: %d\n", zlist->now.stop);
}
