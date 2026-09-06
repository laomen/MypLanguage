# tests/CMakeLists.txt — 把 MYP 脚本测试套件暴露给 ctest
#
# 此前 `add_subdirectory(tests)` 指向不存在的本文件 → ctest / VS Code CMake
# Tools 看不到任何 MYP 测试（roadmap T7）。脚本套件本身不依赖 CMake，这里只是
# 注册：每个测试以 bash 运行对应脚本，工作目录 = 源码根，MYPCC 显式指向本构建
# 树的 mypc（使构建目录名与 ./build 无关）。
#
# 用法：
#   cmake -B build                     # MYP_BUILD_TESTS 默认 ON（仅注册、无额外编译）
#   ctest --test-dir build             # 跑聚合主套件
#   ctest --test-dir build -R myp_self_bootstrap   # 只跑自举专项

# 显式传 MYPCC 到脚本环境：脚本默认 ./build/mypc，仅当构建目录恰为 "build" 时
# 才正确；cmake -E env 使任意构建目录名都可用。
function(myp_add_script_test name script)
    add_test(
        NAME ${name}
        COMMAND ${CMAKE_COMMAND} -E env
                "MYPCC=${CMAKE_BINARY_DIR}/mypc"
                bash "${CMAKE_CURRENT_SOURCE_DIR}/${script}"
    )
    set_tests_properties(${name} PROPERTIES WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}")
endfunction()

# 聚合主套件 = run_tests.sh 全部 10 部分（回归/负测试/测试框架/无崩溃/包管理/
# 格式化/可视化/mypc run/LSP）——代表整套 500+ 用例，默认跑这个即可。
myp_add_script_test(myp_suite run_tests.sh)

# 独立专项（不在 run_tests.sh 主套件内的独立价值用例）：
myp_add_script_test(myp_self_bootstrap test_myp_self.sh)   # 自举 3 代 + MD5 门禁
myp_add_script_test(myp_multifile test_multifile.sh)       # §12 多文件编译
myp_add_script_test(myp_pm test_myp_pm.sh)                 # 包管理器自举（独立回归）
