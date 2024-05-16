cmake_minimum_required(VERSION 3.2.0)

project("libufs")

set(EXPORT_COMPILE_COMMANDS ON)
set(CXX_STANDARD_REQUIRED C++14)

add_library(
	libufs
	libufs_fd.c
	libufs_blist.c
	libufs_jornal.c
	libufs.c
)

include_directories(libul/)

# Unix-like下，我们需要链接pthread来获得多线程支持
if(UNIX)
	set(THREADS_PREFER_PTHREAD_FLAG ON)
	find_package(Threads REQUIRED)
	target_link_libraries(libufs PRIVATE Threads::Threads)
endif()
