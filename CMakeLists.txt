cmake_minimum_required(VERSION 3.20)
project(myp-compiler
    VERSION 0.1.0
    LANGUAGES C CXX
)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

# ---- Sanitizers (for compiler/runtime stability testing) ----
# Usage: cmake -B build-asan -DMYP_SANITIZE=ON
# Instruments mypc/lsp/fmt/viz/runtime with AddressSanitizer + UBSan.
# Generated programs also get ASAN when MYP_SANITIZE=1 is set at mypc runtime
# (see src/main.cpp linkObjects).
option(MYP_SANITIZE "Build with AddressSanitizer + UndefinedBehaviorSanitizer" OFF)
if(MYP_SANITIZE)
    add_compile_options(-fsanitize=address,undefined -fno-omit-frame-pointer -g)
    add_link_options(-fsanitize=address,undefined)
    message(STATUS "Sanitizers enabled: AddressSanitizer + UndefinedBehaviorSanitizer")
endif()

# ---- LLVM ----
find_package(LLVM REQUIRED CONFIG)
message(STATUS "Found LLVM: ${LLVM_DIR} (${LLVM_PACKAGE_VERSION})")

include_directories(${LLVM_INCLUDE_DIRS})
add_definitions(${LLVM_DEFINITIONS})

# ---- GPU offload (optional; can be fully excluded for CI/minimal builds) ----
# Usage: cmake -DMYP_ENABLE_GPU=OFF
#   → 不链接 NVPTX、不编译 GPU 代码（codegen_gpu 内核路径 / runtime_gpu.c）；
#     `@gpu for` 编译为 CPU 顺序回退（语义保持，无 GPU 加速）。
#   CI 若因 NVPTX/CUDA 构建失败，用 -DMYP_ENABLE_GPU=OFF 即可纯 CPU 构建。
option(MYP_ENABLE_GPU "Build with GPU (CUDA/NVPTX) offload support" ON)
if(MYP_ENABLE_GPU)
    add_compile_definitions(MYP_ENABLE_GPU=1)
    message(STATUS "GPU offload enabled (NVPTX + CUDA runtime)")
else()
    message(STATUS "GPU offload DISABLED — @gpu for compiles to CPU fallback")
endif()

# §9.5 P4 跨厂商（AMD）：ROCm/HIP 运行时（runtime_rocm.c）。无 AMD 硬件时保持
# OFF（交叉编译验证走 MYP_GPU_TARGET=amdgcn 编译期 GCN 发射 + CPU 回退语义）。
option(MYP_ENABLE_ROCM "Build with ROCm/HIP runtime (AMD GPU)" OFF)
if(MYP_ENABLE_ROCM)
    add_compile_definitions(MYP_ENABLE_ROCM=1)
    message(STATUS "ROCm/HIP runtime enabled (runtime_rocm.c)")
endif()

# Get LLVM library list for linking（nvptx 组件仅 GPU 构建解析，llvm_map 给完整路径）
set(MYP_LLVM_COMPONENTS
    core
    support
    mc
    irreader
    target
    passes
    codegen
    nativecodegen
    x86desc
    x86codegen
    x86info
    x86asmparser
    x86disassembler
)
if(MYP_ENABLE_GPU)
    list(APPEND MYP_LLVM_COMPONENTS nvptxdesc nvptxcodegen nvptxinfo)
    # §9.5 P4 跨厂商：AMDGPU 后端（交叉编译 GCN code object；无 AMD 硬件时仅验证产出）。
    list(APPEND MYP_LLVM_COMPONENTS amdgpuinfo amdgpudesc amdgpucodegen
         amdgpuasmparser amdgpuutils amdgputargetmca amdgpudisassembler)
endif()
llvm_map_components_to_libnames(myp_llvm_libs ${MYP_LLVM_COMPONENTS})

# ---- CUDA (optional, only when GPU enabled; runtime activation via dlopen) ----
if(MYP_ENABLE_GPU)
    find_package(CUDAToolkit QUIET)
    if(CUDAToolkit_FOUND)
        message(STATUS "Found CUDA Toolkit: ${CUDAToolkit_VERSION} at ${CUDAToolkit_INCLUDE_DIRS}")
        add_compile_definitions(MYP_CUDA_ENABLED=1)
    else()
        message(STATUS "CUDA Toolkit not found — GPU offload disabled (CPU fallback only)")
    endif()
endif()

# ---- Sources ----
set(SOURCES
    src/main.cpp
    src/SourceLocation.cpp
    src/token.cpp
    src/lexer/lexer.cpp
    src/ast/ast.cpp
    src/frontend_dump.cpp
    src/parser/parser.cpp
    src/parser/parser_stmt.cpp
    src/parser/parser_expr.cpp
    src/sema/sema.cpp
    src/sema/sema_expr.cpp
    src/sema/type.cpp
    src/sema/symbol_table.cpp
    src/codegen/codegen.cpp
    src/codegen/codegen_class.cpp
    src/codegen/codegen_stmt.cpp
    src/codegen/codegen_expr.cpp
    src/codegen/codegen_gpu.cpp
    src/codegen/codegen_test.cpp
    src/codegen/myp_passes.cpp
    src/eval/eval.cpp
    src/macro/macro_expand.cpp
    src/fmt/fmt.cpp
)

set(HEADERS
    include/mylang/SourceLocation.h
    include/mylang/DiagnosticEngine.h
    include/mylang/Token.h
    include/mylang/Lexer.h
    include/mylang/AST.h
    include/mylang/Parser.h
    include/mylang/Sema.h
    include/mylang/Type.h
    include/mylang/SymbolTable.h
    include/mylang/CodeGen.h
    include/mylang/MypPasses.h
    include/mylang/Eval.h
    include/mylang/Macro.h
    include/mylang/Fmt.h
)

include_directories(BEFORE ${CMAKE_SOURCE_DIR}/include)

# ---- Compiler Executable ----
add_executable(mypc ${SOURCES})

# ---- DAP debug adapter (bridges VS Code DAP <-> gdb MI) ----
add_executable(myp_debug src/dap/dap_server.cpp)
target_link_libraries(myp_debug PRIVATE pthread)

# Mapping visualizer tool (uses same parser)
add_executable(myp_viz src/myp_viz.cpp src/token.cpp src/lexer/lexer.cpp
    src/ast/ast.cpp src/parser/parser.cpp src/parser/parser_stmt.cpp src/parser/parser_expr.cpp
    src/SourceLocation.cpp)
target_link_libraries(myp_viz PRIVATE ${LLVM_LIBS})

# ---- LSP Server ----
add_executable(myp_lsp
    src/lsp/main.cpp
    src/lsp/lsp_server.cpp
    src/token.cpp
    src/lexer/lexer.cpp
    src/ast/ast.cpp
    src/parser/parser.cpp
    src/parser/parser_stmt.cpp
    src/parser/parser_expr.cpp
    src/sema/sema.cpp
    src/sema/sema_expr.cpp
    src/sema/type.cpp
    src/sema/symbol_table.cpp
    src/SourceLocation.cpp
)

target_include_directories(myp_lsp
    PRIVATE
    ${CMAKE_SOURCE_DIR}/include
    ${LLVM_INCLUDE_DIRS}
)

target_link_libraries(myp_lsp PRIVATE pthread)

target_link_libraries(mypc
    PRIVATE
    ${myp_llvm_libs}
    pthread
)

target_include_directories(mypc
    PRIVATE
    ${CMAKE_SOURCE_DIR}/include
    ${LLVM_INCLUDE_DIRS}
)

# ---- Formatter Executable ----
add_executable(myp_fmt
    src/fmt/main.cpp
    src/fmt/fmt.cpp
    src/token.cpp
    src/lexer/lexer.cpp
    src/SourceLocation.cpp
)
target_include_directories(myp_fmt PRIVATE ${CMAKE_SOURCE_DIR}/include)

# ---- Self-hosted Package Manager (tools/pm, built with mypc) ----
# 用 MYP 语言实现的包管理器（自举工具链）。见 docs/pkg_manager.md / self_hosting.md。
set(MYP_PM_SRC ${CMAKE_SOURCE_DIR}/tools/pm/main.myp)
set(MYP_PM_MODULES
    ${CMAKE_SOURCE_DIR}/tools/pm/meta.myp
    ${CMAKE_SOURCE_DIR}/tools/pm/build.myp
    ${CMAKE_SOURCE_DIR}/tools/pm/install.myp
    ${CMAKE_SOURCE_DIR}/tools/pm/lockfile.myp
    ${CMAKE_SOURCE_DIR}/tools/pm/registry.myp
    ${CMAKE_SOURCE_DIR}/tools/pm/util.myp
)
set(MYP_PM_OUT ${CMAKE_BINARY_DIR}/myp)

add_custom_command(
    OUTPUT ${MYP_PM_OUT}
    # ASAN 版 mypc 的 arena 分配会触发泄漏检测 → 显式关闭（与测试套件一致）
    COMMAND ${CMAKE_COMMAND} -E env ASAN_OPTIONS=detect_leaks=0 $<TARGET_FILE:mypc> ${MYP_PM_SRC} -o ${MYP_PM_OUT} --stdlib ${CMAKE_SOURCE_DIR}/stdlib
    # mypc 会在源码旁生成 main.myp.o，构建后清理保持源码树干净
    COMMAND ${CMAKE_COMMAND} -E remove -f ${CMAKE_SOURCE_DIR}/tools/pm/main.myp.o
    DEPENDS ${MYP_PM_SRC} ${MYP_PM_MODULES} mypc
    COMMENT "Building self-hosted package manager (tools/pm -> myp)"
    VERBATIM
)
add_custom_target(myp_pm ALL DEPENDS ${MYP_PM_OUT})

# ---- Self-hosted Formatter (tools/fmt, built with mypc) ----
# 用 MYP 语言实现的格式化器（自举工具链 T2）。见 docs/self_hosting.md。
# 与 C++ myp_fmt 字节级对拍（stdlib/examples/tools/tests/BNCTDoseEngine 218 文件全一致）。
set(MYP_FMT_SRC ${CMAKE_SOURCE_DIR}/tools/fmt/main.myp)
set(MYP_FMT_MODULES
    ${CMAKE_SOURCE_DIR}/tools/fmt/fmt.myp
    ${CMAKE_SOURCE_DIR}/tools/fmt/lexer.myp
)
set(MYP_FMT_OUT ${CMAKE_BINARY_DIR}/myp_fmt2)

add_custom_command(
    OUTPUT ${MYP_FMT_OUT}
    # ASAN 版 mypc 的 arena 分配会触发泄漏检测 → 显式关闭（与测试套件一致）
    COMMAND ${CMAKE_COMMAND} -E env ASAN_OPTIONS=detect_leaks=0 $<TARGET_FILE:mypc> ${MYP_FMT_SRC} -o ${MYP_FMT_OUT} --stdlib ${CMAKE_SOURCE_DIR}/stdlib
    # mypc 会在源码旁生成 main.myp.o，构建后清理保持源码树干净
    COMMAND ${CMAKE_COMMAND} -E remove -f ${CMAKE_SOURCE_DIR}/tools/fmt/main.myp.o
    DEPENDS ${MYP_FMT_SRC} ${MYP_FMT_MODULES} mypc
    COMMENT "Building self-hosted formatter (tools/fmt -> myp_fmt2)"
    VERBATIM
)
add_custom_target(myp_fmt2 ALL DEPENDS ${MYP_FMT_OUT})

# ---- Self-hosted Mapping Visualizer (tools/viz, built with mypc) ----
# 用 MYP 语言实现的 mapping 可视化器（自举工具链 T3/M4）。见 docs/self_hosting.md。
# 与 C++ myp_viz 字节级对拍（全语料合法文件 220/220 一致）。
set(MYP_VIZ_SRC ${CMAKE_SOURCE_DIR}/tools/viz/main.myp)
set(MYP_VIZ_MODULES
    ${CMAKE_SOURCE_DIR}/tools/viz/viz.myp
    ${CMAKE_SOURCE_DIR}/tools/viz/lexer.myp
)
set(MYP_VIZ_OUT ${CMAKE_BINARY_DIR}/myp_viz2)

add_custom_command(
    OUTPUT ${MYP_VIZ_OUT}
    # ASAN 版 mypc 的 arena 分配会触发泄漏检测 → 显式关闭（与测试套件一致）
    COMMAND ${CMAKE_COMMAND} -E env ASAN_OPTIONS=detect_leaks=0 $<TARGET_FILE:mypc> ${MYP_VIZ_SRC} -o ${MYP_VIZ_OUT} --stdlib ${CMAKE_SOURCE_DIR}/stdlib
    # mypc 会在源码旁生成 main.myp.o，构建后清理保持源码树干净
    COMMAND ${CMAKE_COMMAND} -E remove -f ${CMAKE_SOURCE_DIR}/tools/viz/main.myp.o
    DEPENDS ${MYP_VIZ_SRC} ${MYP_VIZ_MODULES} mypc
    COMMENT "Building self-hosted mapping visualizer (tools/viz -> myp_viz2)"
    VERBATIM
)
add_custom_target(myp_viz2 ALL DEPENDS ${MYP_VIZ_OUT})

# ---- Self-Hosted Compiler skeleton (tools/selfhost, built with mypc) ----
# 全自举编译器（T5，非 GPU）：CLI 骨架。见 tools/selfhost/design.md + roadmap.md。
# F0 仅构建骨架（main.myp）；F1+ 把实现的模块加入 MYP_SELF_MODULES 随构建。
# 依赖 mypc（C++ 编译器作为 stage0 编译本自举编译器）。
set(MYP_SELF_SRC ${CMAKE_SOURCE_DIR}/tools/selfhost/src/main.myp)
set(MYP_SELF_MODULES
    ${CMAKE_SOURCE_DIR}/tools/selfhost/src/token.myp
    ${CMAKE_SOURCE_DIR}/tools/selfhost/src/lexer.myp
    ${CMAKE_SOURCE_DIR}/tools/selfhost/src/ast.myp
    ${CMAKE_SOURCE_DIR}/tools/selfhost/src/parser.myp
    ${CMAKE_SOURCE_DIR}/tools/selfhost/src/diag.myp
    ${CMAKE_SOURCE_DIR}/tools/selfhost/src/sema.myp
    ${CMAKE_SOURCE_DIR}/tools/selfhost/src/ir_emit.myp
    ${CMAKE_SOURCE_DIR}/tools/selfhost/src/codegen.myp
    ${CMAKE_SOURCE_DIR}/tools/selfhost/src/link.myp
)
set(MYP_SELF_OUT ${CMAKE_BINARY_DIR}/myp_self)

add_custom_command(
    OUTPUT ${MYP_SELF_OUT}
    # ASAN 版 mypc 的 arena 分配会触发泄漏检测 → 显式关闭（与测试套件一致）
    COMMAND ${CMAKE_COMMAND} -E env ASAN_OPTIONS=detect_leaks=0 $<TARGET_FILE:mypc> ${MYP_SELF_SRC} -o ${MYP_SELF_OUT} --stdlib ${CMAKE_SOURCE_DIR}/stdlib
    # mypc 会在源码旁生成 main.myp.o，构建后清理保持源码树干净
    COMMAND ${CMAKE_COMMAND} -E remove -f ${CMAKE_SOURCE_DIR}/tools/selfhost/src/main.myp.o
    DEPENDS ${MYP_SELF_SRC} ${MYP_SELF_MODULES} mypc
    COMMENT "Building self-hosted compiler skeleton (tools/selfhost -> myp_self)"
    VERBATIM
)
add_custom_target(myp_self ALL DEPENDS ${MYP_SELF_OUT})

# ---- Runtime Library (C) ----
find_package(PkgConfig REQUIRED)
# SDL2 可选：mypc 本体不依赖 SDL（不链接 myp_runtime，只链 LLVM+pthread）；
# 生成程序只在 `import sdl` 且引用 myp_sdl_* 符号时按需编译链接 sdl_bridge.c
# （src/main.cpp linkObjects 运行时 nm 检测）。此处只影响 myp_runtime 静态库
# （供测试/工具）是否编译 sdl_bridge.c —— 没装 SDL2 的环境也能构建 mypc 全工具链。
pkg_check_modules(SDL2 QUIET sdl2)
if(SDL2_FOUND)
    message(STATUS "SDL2 found (${SDL2_VERSION}) — myp_runtime 含 sdl_bridge.c")
else()
    message(STATUS "SDL2 NOT found — myp_runtime 不含 sdl_bridge.c；生成程序按需链接 SDL 仍可用")
endif()

find_package(PkgConfig)
if(PkgConfig_FOUND)
    pkg_check_modules(HDF5 hdf5)
endif()
set(MYP_RUNTIME_SRCS
    src/runtime/runtime.c
    src/runtime/coro_ctx.S
    src/runtime/myp_stdlib.c
    src/runtime/runtime_gpu.c
    src/runtime/runtime_lib.c
)
if(SDL2_FOUND)
    list(APPEND MYP_RUNTIME_SRCS src/runtime/sdl_bridge.c)
endif()
if(MYP_ENABLE_ROCM)
    list(APPEND MYP_RUNTIME_SRCS src/runtime/runtime_rocm.c)
endif()
add_library(myp_runtime STATIC ${MYP_RUNTIME_SRCS})

target_include_directories(myp_runtime
    PUBLIC
    ${CMAKE_SOURCE_DIR}/include
)
if(SDL2_FOUND)
    target_include_directories(myp_runtime PUBLIC ${SDL2_INCLUDE_DIRS})
    target_link_libraries(myp_runtime PUBLIC ${SDL2_LINK_LIBRARIES})
endif()
target_link_libraries(myp_runtime PUBLIC ${CMAKE_DL_LIBS})

# ---- Tests (optional, requires GoogleTest) ----
option(MYP_BUILD_TESTS "Build unit tests" OFF)

# ---- Tests (optional, requires GoogleTest) ----
option(MYP_BUILD_TESTS "Build unit tests" OFF)
if(MYP_BUILD_TESTS)
    enable_testing()
    add_subdirectory(tests)
endif()
