cmake_minimum_required(VERSION 3.2.0)

project("libufs_example" LANGUAGES C)
set(EXPORT_COMPILE_COMMANDS ON)

# 我们需要为MSVC使用UTF-8模式
add_compile_options("$<$<C_COMPILER_ID:MSVC>:/utf-8>")
add_compile_options("$<$<CXX_COMPILER_ID:MSVC>:/utf-8>")

add_executable(libufs_example main.c)
add_subdirectory(../libufs libufs)
target_link_libraries(libufs_example PRIVATE libufs)
include_directories(../libufs/)
add_custom_command(TARGET libufs_example POST_BUILD
	COMMAND ${CMAKE_COMMAND} -E copy_if_different
	$<TARGET_FILE:libufs> $<TARGET_FILE_DIR:libufs_example>)