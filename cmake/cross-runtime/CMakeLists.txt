# 交叉验证工程：仅编译 MYP 运行时层（不依赖 LLVM）
# 目的：用 MinGW-w64 把 runtime.c + 无外部依赖的 bridges 编成 Windows 目标，
#       暴露所有 POSIX → Win32 移植障碍（pthread/ucontext/termios/unistd/...）。
# 编译器本体（mypc，依赖 LLVM）不在本工程范围。
#
# 构建：
#   cmake -S cmake/cross-runtime -B build-win-runtime \
#         -DCMAKE_TOOLCHAIN_FILE=cmake/win64-mingw.toolchain.cmake
#   cmake --build build-win-runtime

cmake_minimum_required(VERSION 3.16)
project(myp_win_runtime LANGUAGES C ASM)

set(MYP_SRC_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/../..")

# ---- 核心运行时 ----
set(MYP_RUNTIME_SRCS
    ${MYP_SRC_ROOT}/src/runtime/runtime.c
)

# ---- Windows 协程切换汇编（Win64 ABI；Linux 用 coro_ctx.S 由主构建提供）----
if(MINGW OR MSVC)
    list(APPEND MYP_RUNTIME_SRCS ${MYP_SRC_ROOT}/src/runtime/coro_ctx_win.S)
endif()

# ---- 无外部依赖的 stdlib bridges（net/uds/process/regex/json/base64/date/hash）
set(MYP_BRIDGE_SRCS
    ${MYP_SRC_ROOT}/stdlib/bridges/net_bridge.c
    ${MYP_SRC_ROOT}/stdlib/bridges/uds_bridge.c
    ${MYP_SRC_ROOT}/stdlib/bridges/process_bridge.c
    ${MYP_SRC_ROOT}/stdlib/bridges/regex_bridge.c
    ${MYP_SRC_ROOT}/stdlib/bridges/json_bridge.c
    ${MYP_SRC_ROOT}/stdlib/bridges/base64_bridge.c
    ${MYP_SRC_ROOT}/stdlib/bridges/date_bridge.c
    ${MYP_SRC_ROOT}/stdlib/bridges/hash_bridge.c
)

# 注意：SDL/CUDA 桥（sdl_bridge/sdl_ttf_bridge/runtime_gpu.c）依赖额外库，
# 交叉编译验证阶段先排除；runtime_gpu.c 需 Windows CUDA 环境。

add_library(myp_win_runtime STATIC ${MYP_RUNTIME_SRCS} ${MYP_BRIDGE_SRCS})

target_include_directories(myp_win_runtime PRIVATE
    ${MYP_SRC_ROOT}/include
    ${MYP_SRC_ROOT}/src/runtime
)

# MinGW 提供 winpthreads；其余 POSIX 头缺失会在此暴露
find_package(Threads QUIET)
if(Threads_FOUND)
    target_link_libraries(myp_win_runtime PRIVATE Threads::Threads)
endif()
