# MinGW-w64 交叉编译 toolchain：Linux host → Windows x86_64 target
# 用途：验证 MYP 运行时层（runtime.c + stdlib/bridges）在 Windows 的编译可行性，
#       暴露 POSIX → Win32 依赖障碍清单。不用于构建依赖 LLVM 的编译器本体
#       （mypc/myp_lsp 需 Windows 版 LLVM 库，见 docs/CHANGELOG v3.13.x 讨论）。
#
# 用法：
#   cmake -S cmake/cross-runtime -B build-win-runtime \
#         -DCMAKE_TOOLCHAIN_FILE=cmake/win64-mingw.toolchain.cmake
#   cmake --build build-win-runtime
#
# 前置：sudo apt install gcc-mingw-w64-x86-64 g++-mingw-w64-x86-64

set(CMAKE_SYSTEM_NAME Windows)
set(CMAKE_SYSTEM_PROCESSOR x86_64)

# 编译器（不带绝对路径，依赖 PATH；装好后在 PATH 中）
set(CMAKE_C_COMPILER x86_64-w64-mingw32-gcc)
set(CMAKE_CXX_COMPILER x86_64-w64-mingw32-g++)
set(CMAKE_RC_COMPILER x86_64-w64-mingw32-windres)

# 交叉环境的根路径（MinGW sysroot）
set(CMAKE_FIND_ROOT_PATH /usr/x86_64-w64-mingw32)

# 程序（如 cmake 探测工具）用宿主侧，库/头用目标侧
set(CMAKE_FIND_ROOT_PATH_MODE_PROGRAM NEVER)
set(CMAKE_FIND_ROOT_PATH_MODE_LIBRARY ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_INCLUDE ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_PACKAGE ONLY)

# 显式目标系统（避免宿主 glibc 头干扰）
add_compile_definitions(_WIN32 WIN32 _WIN64 __USE_MINGW_ANSI_STDIO=1)

# 严格：把缺头/隐式声名当警告先暴露，不阻断（先看全貌再逐个修）
add_compile_options(-Wall -Wno-unused-function)
