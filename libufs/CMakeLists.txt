cmake_minimum_required(VERSION 3.2.0)

project("libufs")
set(EXPORT_COMPILE_COMMANDS ON)

# 我们需要为MSVC使用UTF-8模式
add_compile_options("$<$<C_COMPILER_ID:MSVC>:/utf-8>")
add_compile_options("$<$<CXX_COMPILER_ID:MSVC>:/utf-8>")

option(LIBUFS_NO_THREAD_SAFE "关闭多线程安全" OFF)

add_library(
	libufs
	libufs_internel.c
	libufs_thread.c
	libufs_fd.c
	libufs_jornal.c
	libufs_bcache.c
	libufs_blist.c
)
include_directories(libul/)

if(LIBUFS_NO_THREAD_SAFE)
	add_compile_definitions(LIBUFS_NO_THREAD_SAFE)
else()
	# Unix-like下，我们需要链接pthread来获得多线程支持
	if(UNIX)
		set(THREADS_PREFER_PTHREAD_FLAG ON)
		find_package(Threads REQUIRED)
		target_link_libraries(libufs PRIVATE Threads::Threads)
	endif()
endif()

# 打开绝大部分错误
if(1)
	if("${CMAKE_C_COMPILER_ID}" MATCHES "GNU")
		set(CMAKE_C_FLAGS "${CMAKE_C_FLAGS} -Wall -Wextra -pedantic-errors -Wconversion -Wno-language-extension-token")
	elseif("${CMAKE_C_COMPILER_ID}" MATCHES "Clang")
		set(CMAKE_C_FLAGS "${CMAKE_C_FLAGS} -Wall -Wextra -pedantic-errors -Wconversion -Wno-language-extension-token -Wno-unsafe-buffer-usage -Wno-reserved-identifier -Wno-used-but-marked-unused")
	elseif("${CMAKE_C_COMPILER_ID}" MATCHES "MSVC")
		set(CMAKE_C_FLAGS "${CMAKE_C_FLAGS} /W4")
	endif()
endif()
