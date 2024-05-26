#include "libufs_internel.h"
#include "ulrb.h"

typedef struct _node_t {
    ulrb_node_t base;
    uint64_t inum;
    ufs_minode_t minode;
} _node_t;

static int _node_comp(void* opaque, const void* lhs, const void* rhs) {
    const uint64_t lv = *ul_reinterpret_cast(const uint64_t*, lhs);
    const uint64_t rv = *ul_reinterpret_cast(const uint64_t*, rhs);
    (void)opaque;
    return lv < rv ? -1 : lv > rv;
}

static void _node_destroy(void* opaque, ulrb_node_t* _node) {
    _node_t* node = ul_reinterpret_cast(_node_t*, _node);
    (void)opaque;
    ulatomic_spinlock_lock(&node->minode.lock);
    ufs_minode_sync(&node->minode, 0);
    ulatomic_spinlock_unlock(&node->minode.lock);
    ufs_free(node);
}

typedef struct _fileset_t {
    ulrb_node_t* root;
    ulatomic_spinlock_t lock;
    ufs_t* ufs;
} _fileset_t;

UFS_HIDDEN int ufs_fileset_init(ufs_fileset_t* _fs, ufs_t* ufs) {
    _fileset_t* fs = ul_reinterpret_cast(_fileset_t*, _fs);
    fs->root = NULL;
    fs->ufs = ufs;
    ulatomic_spinlock_init(&fs->lock);
    return 0;
}
UFS_HIDDEN void ufs_fileset_deinit(ufs_fileset_t* _fs) {
    _fileset_t* fs = ul_reinterpret_cast(_fileset_t*, _fs);
    ulatomic_spinlock_lock(&fs->lock);
    ulrb_destroy(&fs->root, _node_destroy, NULL);
    ulatomic_spinlock_unlock(&fs->lock);
}
UFS_HIDDEN int ufs_fileset_open(ufs_fileset_t* ufs_restrict _fs, uint64_t inum, ufs_minode_t** ufs_restrict pinode) {
    int ec;
    _node_t* node;
    _fileset_t* fs = ul_reinterpret_cast(_fileset_t*, _fs);
    ulatomic_spinlock_lock(&fs->lock);
    node = ul_reinterpret_cast(_node_t*, ulrb_find(fs->root, &inum, _node_comp, NULL));
    if(node) {
        ulatomic_spinlock_unlock(&fs->lock);
        ufs_minode_lock(&node->minode);
        if(node->minode.share == UINT32_MAX) ec = UFS_EOVERFLOW;
        else { ++node->minode.share; ec = 0; }
        ufs_minode_unlock(&node->minode);
        *pinode = &node->minode;
        return ec;
    }

    node = ul_reinterpret_cast(_node_t*, ufs_malloc(sizeof(_node_t)));
    if(ufs_unlikely(node == NULL)) { ec = UFS_ENOMEM; goto do_return; }
    ec = ufs_minode_init(fs->ufs, &node->minode, inum);
    if(ufs_unlikely(ec)) { ufs_free(node); goto do_return; }
    node->inum = inum;
    ulrb_insert_unsafe(&fs->root, ul_reinterpret_cast(ulrb_node_t*, node), _node_comp, NULL);
    *pinode = &node->minode;

do_return:
    ulatomic_spinlock_unlock(&fs->lock);
    return ec;
}
UFS_HIDDEN int ufs_fileset_creat(
    ufs_fileset_t* ufs_restrict _fs, uint64_t* pinum,
    ufs_minode_t** ufs_restrict pinode, const ufs_inode_create_t* ufs_restrict creat
) {
    int ec;
    _node_t* node;
    _fileset_t* fs = ul_reinterpret_cast(_fileset_t*, _fs);
    node = ul_reinterpret_cast(_node_t*, ufs_malloc(sizeof(_node_t)));
    if(ufs_unlikely(node == NULL)) return UFS_ENOMEM;
    ec = ufs_minode_create(fs->ufs, &node->minode, creat);
    node->inum = node->minode.inum;
    if(ufs_unlikely(ec)) { ufs_free(node); return ec; }
    *pinum = node->inum;
    *pinode = &node->minode;

    ulatomic_spinlock_lock(&fs->lock);
    ulrb_insert_unsafe(&fs->root, ul_reinterpret_cast(ulrb_node_t*, node), _node_comp, NULL);
    ulatomic_spinlock_unlock(&fs->lock);

    return 0;
}
UFS_HIDDEN int ufs_fileset_close(ufs_fileset_t* _fs, uint64_t inum) {
    int ec = 0;
    _node_t* node;
    _fileset_t* fs = ul_reinterpret_cast(_fileset_t*, _fs);
    ulatomic_spinlock_lock(&fs->lock);
    node = ul_reinterpret_cast(_node_t*, ulrb_find(fs->root, &inum, _node_comp, NULL));
    if(ufs_unlikely(node == NULL)) { ulatomic_spinlock_unlock(&fs->lock); return UFS_EBADF; }

    ufs_minode_lock(&node->minode);
    if(--node->minode.share == 0) {
        ec = ufs_minode_deinit(&node->minode);
        node = ul_reinterpret_cast(_node_t*, ulrb_remove(&fs->root, &inum, _node_comp, NULL));
        ufs_free(node);
    }
    ufs_minode_unlock(&node->minode);

    ulatomic_spinlock_unlock(&fs->lock);
    return ec;
}

static void _node_walk(void* opaque, const ulrb_node_t* _node) {
    _node_t* node = ul_reinterpret_cast(_node_t*, ufs_const_cast(ulrb_node_t*, _node));
    (void)opaque;
    ufs_minode_lock(&node->minode);
    ufs_minode_sync(&node->minode, 0);
    ufs_minode_unlock(&node->minode);
}
UFS_HIDDEN void ufs_fileset_sync(ufs_fileset_t* _fs) {
    _fileset_t* fs = ul_reinterpret_cast(_fileset_t*, _fs);
    ulatomic_spinlock_lock(&fs->lock);
    ulrb_walk_preorder(fs->root, _node_walk, NULL);
    ulatomic_spinlock_unlock(&fs->lock);
}
