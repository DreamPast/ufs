# libufs 一个简单的日志文件系统实现

| 目录           | 说明                                                         |
| -------------- | ------------------------------------------------------------ |
| libul          | 外置依赖项，文件来自我自己的库[libul](https://github.com/DreamPast/libul) |
| libufs         | 文件系统核心实现                                             |
| libufs_example | 文件系统核心样例                                             |
| libufs_on_fuse | 文件系统的FUSE包装                                           |

- 目录中的文件都是保存在UTF-8格式下的，在某些很老的Windows编译器上可能会带来一些问题，可以自己转成GBK编码。
- 除了libul遵循C89标准以外，其它文件均依赖于C99的子集，这能够通过大部分编译器。

## libufs

### 依赖

- C99子集

- CMake 3.9

### 如何构建

```bash
cd libufs # 切换到路径中
cmake . -B build -DCMAKE_BUILD_TYPE=Release # 配置项目
cmake --build build --config Release # 构建项目
```

### 如何使用

把libufs.h和构建好的二进制文件拷贝出来即可。

具体内容可以参考libufs_example中的使用

## libufs_example

### 依赖

- C99子集

- CMake 3.9

### 如何构建

```bash
cd libufs # 切换到路径中
cmake . -B build -DCMAKE_BUILD_TYPE=Release # 配置项目
cmake --build build --config Release # 构建项目
```

### 如何使用

运行build文件夹中的libufs_example.exe（可能在子目录中）

## libufs_on_fuse

### 依赖

- C99子集
- CMake 3.9
- FUSE 2.9

### 如何构建

安装完FUSE后运行build.sh即可

### 如何运行

首先需要有一个空文件夹，然后执行：

```
./fuse your_dir --path=disk_path
```

- your_dir是期望挂载的目录

- disk_path是磁盘文件所在的目录

- 如果你没有磁盘文件，添加size参数将会自动创建，例如--size=1024000会尝试创建1024000字节的磁盘文件（实际可能会因为对齐稍微小一点）

然后在路径中进行操作即可。

## 协议

依赖libul使用[MIT协议](./libul/LICENSE)

其余部分使用[Apache 2.0协议](./LICENSE)