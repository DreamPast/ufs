#include "libufs_internel.h"

UFS_HIDDEN int ufs_transcation_init(ufs_transcation_t* ufs_restrict transcation, ufs_jornal_t* ufs_restrict jornal) {
    transcation->jornal = jornal;
    transcation->num = 0;
    return 0;
}
UFS_HIDDEN void ufs_transcation_deinit(ufs_transcation_t* transcation) {
    ufs_transcation_settop(transcation, 0);
}

UFS_HIDDEN int ufs_transcation_add(ufs_transcation_t* ufs_restrict transcation, const void* ufs_restrict buf, uint64_t bnum, size_t off, size_t len, int flag) {
    int ec;
    char* tmp;
    tmp = ul_reinterpret_cast(char*, ufs_malloc(UFS_BLOCK_SIZE));
    if(ufs_unlikely(tmp == NULL)) { ec = UFS_ENOMEM; goto do_return; }
    ec = ufs_transcation_read_block(transcation, tmp, bnum);
    if(ufs_unlikely(ec)) { ufs_free(tmp); goto do_return; }
    memcpy(tmp + off, buf, len);
    ec = ufs_transcation_add_block(transcation, tmp, bnum, UFS_JORNAL_ADD_MOVE);
do_return:
    if(flag == UFS_JORNAL_ADD_MOVE) ufs_free(ufs_const_cast(void*, buf));
    return ec;
}
UFS_HIDDEN int ufs_transcation_add_block(ufs_transcation_t* ufs_restrict transcation, const void* ufs_restrict buf, uint64_t bnum, int flag) {
    if(ufs_unlikely(transcation->num == UFS_JORNAL_NUM)) {
        if(flag == UFS_JORNAL_ADD_MOVE) ufs_free(ufs_const_cast(void*, buf));
        return UFS_EOVERFLOW;
    }
    switch(flag) {
    case UFS_JORNAL_ADD_COPY:
        transcation->ops[transcation->num].buf = ufs_malloc(UFS_BLOCK_SIZE);
        if(ufs_unlikely(transcation->ops[transcation->num].buf == NULL)) return UFS_ENOMEM;
        memcpy(ufs_const_cast(void*, transcation->ops[transcation->num].buf), buf, UFS_BLOCK_SIZE);
        break;
    case UFS_JORNAL_ADD_MOVE:
        transcation->ops[transcation->num].buf = buf;
        break;
    default:
        return UFS_EINVAL;
    }
    transcation->ops[transcation->num].bnum = bnum;
    ++transcation->num;
    return 0;
}
UFS_HIDDEN int ufs_transcation_add_zero_block(ufs_transcation_t* transcation, uint64_t bnum) {
    char* tmp = ul_reinterpret_cast(char*, ufs_malloc(UFS_BLOCK_SIZE));
    if(ufs_unlikely(tmp == NULL)) return UFS_ENOMEM;
    memset(tmp, 0, UFS_BLOCK_SIZE);
    return ufs_transcation_add_block(transcation, tmp, bnum, UFS_JORNAL_ADD_MOVE);
}
UFS_HIDDEN int ufs_transcation_add_zero(ufs_transcation_t* transcation, uint64_t bnum, size_t off, size_t len) {
    int ec;
    char* tmp;
    tmp = ul_reinterpret_cast(char*, ufs_malloc(UFS_BLOCK_SIZE));
    if(ufs_unlikely(tmp == NULL)) return UFS_ENOMEM;
    ec = ufs_transcation_read_block(transcation, tmp, bnum);
    if(ufs_unlikely(ec)) { ufs_free(tmp); return ec; }
    memset(tmp + off, 0, len);
    return ufs_transcation_add_block(transcation, tmp, bnum, UFS_JORNAL_ADD_MOVE);
}
UFS_HIDDEN int ufs_transcation_read_block(ufs_transcation_t* ufs_restrict transcation, void* ufs_restrict buf, uint64_t bnum) {
    int i;
    for(i = transcation->num - 1; i >= 0; --i)
        if(transcation->ops[i].bnum == bnum) {
            memcpy(buf, transcation->ops[i].buf, UFS_BLOCK_SIZE);
            return 0;
        }
    return ufs_jornal_read_block(transcation->jornal, buf, bnum);
}
UFS_HIDDEN int ufs_transcation_read(ufs_transcation_t* ufs_restrict transcation, void* ufs_restrict buf, uint64_t bnum, size_t off, size_t len) {
    int i;
    for(i = transcation->num - 1; i >= 0; --i)
        if(transcation->ops[i].bnum == bnum) {
            memcpy(buf, ul_reinterpret_cast(const char*, transcation->ops[i].buf) + off, len);
            return 0;
        }
    return ufs_jornal_read(transcation->jornal, buf, bnum, off, len);
}
UFS_HIDDEN int ufs_transcation_commit(ufs_transcation_t* transcation, int num) {
    int ec;
    ec = ufs_jornal_append(transcation->jornal, transcation->ops, num);
    if(ufs_unlikely(ec)) return ec;
    transcation->num -= num;
    return 0;
}
UFS_HIDDEN int ufs_transcation_commit_all(ufs_transcation_t* transcation) {
    return ufs_transcation_commit(transcation, transcation->num);
}
UFS_HIDDEN void ufs_transcation_settop(ufs_transcation_t* transcation, int top) {
    int i;
    ufs_assert(top <= transcation->num);
    for(i = top; i < transcation->num; ++i)
        ufs_free(ufs_const_cast(void*, transcation->ops[i].buf));
    transcation->num = top;
}
