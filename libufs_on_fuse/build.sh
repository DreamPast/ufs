#! /bin/bash
trap "exit" ERR
cd "$(dirname "$0")"

# 构建libufs
cmake ../libufs -B libufs_build -DCMAKE_BUILD_TYPE=Release
cmake --build libufs_build --config Release
cp ../libufs/libufs.h ./ # 拷贝头文件

# 编译
gcc main.c libufs_build/liblibufs.a -DFUSE_USE_VERSION=29 `pkg-config fuse --cflags --libs` -o ufs -Wall -Wextra

# 删除libufs的构建
rm -rf libufs_build
rm libufs.h
