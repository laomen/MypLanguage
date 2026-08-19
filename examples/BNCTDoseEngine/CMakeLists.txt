cmake_minimum_required(VERSION 3.20)
project(BNCTDoseEngine C)
enable_language(ASM)

# ---- Find HDF5 (serial) ----
find_package(PkgConfig REQUIRED)
pkg_check_modules(HDF5 REQUIRED IMPORTED_TARGET hdf5-serial)

# ---- Find mypc compiler ----
find_program(MYPC mypc REQUIRED
    HINTS /home/xlkj/code/MYPLanguage/build
)

# MYP compiler paths
set(MYP_INCLUDE_DIR /home/xlkj/code/MYPLanguage/include)
set(RUNTIME_DIR  /home/xlkj/code/MYPLanguage/src/runtime)

# ---- MYP source files (entry point + imported files for dependency tracking) ----
set(MYP_SOURCES
    mapping_demo.myp
)
set(MYP_IMPORTED_SOURCES
    hdf5.myp
    physics.myp
    xs_loader.myp
    cross_section_db.myp
    transport.myp
    tally.myp
)

# ---- Compile MYP with mypc ----
# mypc 会把 .o 生成在源码旁, 编译后移到 build/
set(MYP_COMBINED_O "${CMAKE_CURRENT_BINARY_DIR}/mapping_demo.myp.o")

add_custom_command(
    OUTPUT ${MYP_COMBINED_O}
    COMMAND ${MYPC} --package-path ${CMAKE_CURRENT_SOURCE_DIR}
            ${CMAKE_CURRENT_SOURCE_DIR}/mapping_demo.myp
            -o ${CMAKE_CURRENT_BINARY_DIR}/sim || true
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
            ${CMAKE_CURRENT_SOURCE_DIR}/mapping_demo.myp.o
            ${MYP_COMBINED_O}
    COMMAND ${CMAKE_COMMAND} -E remove -f
            ${CMAKE_CURRENT_SOURCE_DIR}/mapping_demo.myp.o
    DEPENDS ${MYP_SOURCES} ${MYP_IMPORTED_SOURCES}
    COMMENT "Compiling MYP sources"
)

# ---- Build hdf5_bridge ----
add_library(hdf5_bridge OBJECT hdf5_bridge.c)
target_include_directories(hdf5_bridge PRIVATE
    ${MYP_INCLUDE_DIR}
    ${HDF5_INCLUDE_DIRS}
)

# ---- Build runtime from MYP compiler ----
# coro_ctx.S provides myp_ctx_switch on x86-64 (the ucontext fallback in
# runtime.c is only used on non-x86-64); without it linking fails with
# "undefined reference to myp_ctx_switch".
add_library(myp_runtime OBJECT
    ${RUNTIME_DIR}/runtime.c
    ${RUNTIME_DIR}/coro_ctx.S
    ${RUNTIME_DIR}/runtime_gpu.c
)
target_include_directories(myp_runtime PRIVATE ${MYP_INCLUDE_DIR})

# ---- Link executable ----
add_executable(sim
    ${MYP_COMBINED_O}
    $<TARGET_OBJECTS:hdf5_bridge>
    $<TARGET_OBJECTS:myp_runtime>
)

target_link_libraries(sim PRIVATE
    PkgConfig::HDF5
    m
    pthread
)

# ---- Clean ----
set_directory_properties(PROPERTIES ADDITIONAL_CLEAN_FILES "*.myp.o")

# ---- Install ----
install(TARGETS sim DESTINATION bin)
