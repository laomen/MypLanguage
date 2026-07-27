cmake_minimum_required(VERSION 3.20)
project(myp-compiler
    VERSION 0.1.0
    LANGUAGES C CXX
)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

# ---- LLVM ----
find_package(LLVM REQUIRED CONFIG)
message(STATUS "Found LLVM: ${LLVM_DIR} (${LLVM_PACKAGE_VERSION})")

include_directories(${LLVM_INCLUDE_DIRS})
add_definitions(${LLVM_DEFINITIONS})

# Get LLVM library list for linking
llvm_map_components_to_libnames(myp_llvm_libs
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

# ---- Sources ----
set(SOURCES
    src/main.cpp
    src/SourceLocation.cpp
    src/token.cpp
    src/lexer/lexer.cpp
    src/ast/ast.cpp
    src/parser/parser.cpp
    src/sema/sema.cpp
    src/sema/type.cpp
    src/sema/symbol_table.cpp
    src/codegen/codegen.cpp
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
    include/mylang/Fmt.h
)

include_directories(BEFORE ${CMAKE_SOURCE_DIR}/include)

# ---- Compiler Executable ----
add_executable(mypc ${SOURCES})

# Mapping visualizer tool (uses same parser)
add_executable(myp_viz src/myp_viz.cpp src/token.cpp src/lexer/lexer.cpp
    src/ast/ast.cpp src/parser/parser.cpp
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
    src/sema/sema.cpp
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

# ---- Runtime Library (C) ----
add_library(myp_runtime STATIC
    src/runtime/runtime.c
    src/runtime/myp_stdlib.c
)

target_include_directories(myp_runtime
    PUBLIC
    ${CMAKE_SOURCE_DIR}/include
)

# ---- Tests (optional, requires GoogleTest) ----
option(MYP_BUILD_TESTS "Build unit tests" OFF)
if(MYP_BUILD_TESTS)
    enable_testing()
    add_subdirectory(tests)
endif()
