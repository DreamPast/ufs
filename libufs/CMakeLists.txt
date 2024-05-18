cmake_minimum_required(VERSION 3.2.0)

project("libufs")

set(EXPORT_COMPILE_COMMANDS ON)
set(CXX_STANDARD_REQUIRED C++14)

add_library(
	libufs
	libufs_internel.h
	libufs_thread.c
	libufs_fd.c
	libufs_jornal.c
	libufs_bcache.c
	libufs_blist.c
)

include_directories(libul/)

# Unix-like下，我们需要链接pthread来获得多线程支持
if(UNIX)
	set(THREADS_PREFER_PTHREAD_FLAG ON)
	find_package(Threads REQUIRED)
	target_link_libraries(libufs PRIVATE Threads::Threads)
endif()


# 打开绝大部分错误
if("${CMAKE_C_COMPILER_ID}" MATCHES "GNU")
	set(CMAKE_C_FLAGS "${CMAKE_C_FLAGS} -Wall -Wextra -pedantic-errors -Wconversion -Wno-language-extension-token")
elseif("${CMAKE_C_COMPILER_ID}" MATCHES "Clang")
	set(CMAKE_C_FLAGS "${CMAKE_C_FLAGS} -Wall -Wextra -pedantic-errors -Wconversion -Wno-language-extension-token")
elseif("${CMAKE_C_COMPILER_ID}" MATCHES "MSVC")
	set(CMAKE_C_FLAGS "${CMAKE_C_FLAGS} /W4")
endif()
