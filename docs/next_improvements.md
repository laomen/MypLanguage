# MYP 下一步完善清单（审评稿）

> 状态：**审评中**（v3.9.0 期间归档，2026-08-05）
> 目的：归档本会话识别的"需完善"项，供评审拍板后实施。
> 变更策略：语言规格 v1.0 冻结——凡新增语法 / stdlib 均为 **additive**（非破坏），
> 可在当前规格下陆续引入，无需等待 2.0。

---

## 一、发布收尾（需拍板）

| # | 事项 | 状态 | 待决定 |
|---|------|------|--------|
| 1 | 创建 git tag `v3.9.0` | 暂缓 | 是否创建（v3.8.0 亦无 tag） |
| 2 | 推送 14 个提交到 gitee / GitHub | 暂缓 | 是否推送 |

> 注：稳定性已过 `-O0`/`-O2`/ASAN/TSan/fuzz/多线程 10 次重复/整库 3 连跑（123/123）。
> 若想更充分再打 tag，可先补 §二-1、§二-2。

## 二、测试 / 稳定性强化

| # | 事项 | 说明 | 工作量 | 优先级 |
|---|------|------|--------|--------|
| 1 | **字节级精确 expected** | `run_tests.sh` 用 `$(...)` 捕获会剥尾部换行，导致 `parallel_for`/`coro_thread` 的程序原始输出（末尾多一个空行）与 expected 不精确一致。清掉测试里 `writeFloat`/`writeLong` 后冗余的 `Console.writeString("\n")`（二者自带换行），使 expected 与原始字节精确一致 | 小 | P1 |
| 2 | **并行测试长 soak** | 线程/池类测试（`pool`/`parallel_for`/`barrier`/`coro_thread`/`future`）跑 100 次重复，确认零抖动 | 小 | P2 |
| 3 | **多点模糊测试** | 多 seed 多轮 `fuzz_test.py`，扩大畸形输入覆盖（当前 100 迭代 0 崩溃） | 小 | P2 |

## 三、语言机制增强——类型系统（最高优先）

| # | 事项 | 现状 | 价值 / 影响 | 工作量 | 优先级 |
|---|------|------|-------------|--------|--------|
| 1 | **`Option<T>` / 空安全** | `null` 裸引用，解引用不保证防护 | 消除最大一类运行时错误；系统性最强 | 大 | **P0** |
| 2 | **元组 + 解构** | 多返回值只能靠自定义 struct；`match` 仅限枚举 | 表达力质变（多值返回/模式解构） | 中 | P1 |
| 3 | **一等函数 / 闭包** | lambda 语法在（`(x)=>{}`），但**无函数类型**，无法存储/传递/返回 | 高阶函数（map/filter/reduce 可落地） | 中 | P1 |
| 4 | **类型别名 `type X = ...`** | 无 | 可读性 | 小 | P2 |
| 5 | **trait 默认实现 / 关联类型** | 有 `interface` + `where T:Interface` | 泛型能力 | 中 | P2 |
| 6 | **泛型方法推断** | 泛型仅类级 | 泛型函数 | 中 | P2 |

## 四、语法 / 表达层

| # | 事项 | 说明 | 优先级 |
|---|------|------|--------|
| 1 | **默认参数 / 命名参数** | `Param ::= Type [Identifier]` 无默认值 | P1 |
| 2 | **for-in / 迭代器协议** | 仅索引式 `for (i=0;...)`，集合遍历靠手写索引 | P1 |
| 3 | **扩展方法** | 无；内建类型只能靠静态类工具函数 | P2 |
| 4 | **多行 / raw 字符串** | 无 `"""..."""` / `r"..."` | P2 |

## 五、机制 / 运行时

| # | 事项 | 说明 | 优先级 |
|---|------|------|--------|
| 1 | **确定性资源管理** | 无析构器；arena + 进程退出兜底。BNCT 等长跑进程会累积句柄/内存 | **P0** |
| 2 | **同步原语 stdlib** | 只有 `Atomic`/`Barrier`；缺 `Mutex`/`RWLock`/`CondVar`/`Semaphore`/`Once`（pthread 底层已有，低成本） | P1 |
| 3 | **错误类型分层 / `Result<T,E>`** | 只有 `try/catch`，无自定义错误类型体系 / 值式错误传播 | P1 |
| 4 | **反射 / RTTI** | 无运行时类型查询 | P2（远期） |
| 5 | **异步 IO 统一抽象** | `await` 仅限事件，未覆盖文件/网络/睡眠 | P2（远期） |

## 六、平台 / 生态（远期）

| # | 事项 | 说明 | 优先级 |
|---|------|------|--------|
| 1 | **Windows 移植** | 前端已可移植（纯 C++17+LLVM）；难点：`runtime.c` POSIX 子系统（pthread/TLS/termios/socket/regex/process）+ `linkObjects` gcc 硬编码 + DAP 对 gdb/fork/pipe 的依赖 | P2（**用户已明确推迟**） |
| 2 | **包管理器** | 无依赖解析/模块版本 | P2 |
| 3 | **文档生成** | 无 doc-comment → API 文档 | P2 |
| 4 | **stdlib 缺口** | crypto/hash、HTTP 客户端、SQL、时区、压缩、`sprintf` 格式化（仅插值）、随机分布 | P2 |

## 七、官方 roadmap（design.md §11，已有规划）

- 🔜 Event-driven Pool（方案 B）
- 🔜 自举（用 MYP 实现编译器前端）
- 🔜 JIT
- 🔜 神经形态后端

## 八、本会话已完成的完善项（归档备查）

1. ✅ **线程池完整 API**：`Parallel` 扩展为 `threadCount`/`workerCount`/`workerId`/`isActive`/`setThreads` + 运行时 3 个新函数 + `tests/pool/`（`0465d71`）
2. ✅ **`myp_fmt` 版本号同步** `2.0.0 → 3.9.0`（`96b3061`）
3. ✅ **过时 docs 状态头修订**：slice / operators / metaprogramming / optimization_debugging / exceptions + CHANGELOG v3.8.0 补 P4 条目（`b5235ae`, `8584bfd`）
4. ✅ **稳定性验证矩阵全绿**：-O0/-O2/ASAN 123/123、TSan 12/12 无竞态、fuzz 0 崩溃、pass 6/6、debug 6/6、DAP 15/15、整库 3 连跑、线程测试 10 次稳定
5. ✅ **包管理器 v1（自举）**：`tools/myp.myp` 重写 Python 版（init/build/install/run/legacy）+ runtime 补 `myp_fs_mkdir_p`/`myp_fs_remove_recursive` + `myp_process_run` 改真实退出码（WEXITSTATUS）+ `tests/test_myp_pm.sh`（9 断言）+ 集成进 run_tests.sh（124/124）
6. ✅ **包管理器 v2 形态**：模块化 `tools/pm/*.myp`（main/meta/build/install/util）+ CMake 自定义 target `myp_pm`（build/myp）
7. ✅ **包管理器 v2 功能**：registry（纯 git 子目录）+ `myp.lock` + `add`/`remove`/`update`/`list` + `myp build` 缺失依赖自动安装；`tests/test_myp_pm.sh` 14/14；-O0/ASAN 124/124
8. ✅ **修复运行时 bug**：`myp_io_read_line` 共享缓冲（`static char buf[4096]`）→ 每次返回新分配字符串，EOF 返回空串；补 `Str.toInt`（`myp_str_to_int`）

## 九、自举发现的语言 bug（需修复）

| # | Bug | 现象 | 影响 | 状态 |
|---|-----|------|------|------|
| 1 | **函数返回定长数组共享存储** | 函数返回 `string[N]`（如 `Fs.listDir`/`Str.split`）后，嵌套调用再返回数组会**覆写外层数组内容**（同层连续调用不冲突，跨嵌套边界才触发） | 所有"先取数组再递归"的模式（含 `copyTree`）出错；自举工具已踩中 | ⚠️ 未修（MYP 层快照规避；codegen 修复待定） |
| 2 | **io 单一全局文件句柄** | `__myp_io_*` 用 `static FILE* myp_io_fp`，不能同时开两个 File | 同时读写两文件互相覆盖（`copyFile` 曾踩中） | ⚠️ 设计约束（文档化；多文件需分时/FFI） |
| 3 | ~~**`myp_io_read_line` 共享缓冲**~~ | 原 `static char buf[4096]`，多次 readLine 结果存数组全指向最后一行 | 存多行数组出错（lockfile/registry 踩中）；`tests/io` expected 曾编码旧 bug | ✅ **已修复**（每次返回 `myp_strdup` 新分配，EOF 返回空串） |
| 4 | ~~**stdlib `StringBuilder` 定长越界**~~ | `parts_` 固定 `string[256]` 且 `append` **无边界检查**，超 256 次追加越界写坏堆（段错误） | 长文件/逐字符追加崩溃（T2 格式化器踩中，两个实锤：extractComments 按字符追加、readFile 按行 ×2 追加） | ✅ **已修复**（改 `string[]` 动态扩容，参考 `ArrayList`；`tests/text` 加 1000 片段用例；T2 格式化器已改回用 StringBuilder 并通过全量对拍） |
| 5 | **`@thread` 协程 stdout 缓冲 bug** | `@startup @thread` 协程里 `Console.write(int)`（`printf("%d\n")`）的换行在**后续大分配/`sb.toString()`** 后丢失或行为不稳（首个 write 的 `\n` 有时消失），且随对象布局扰动而变化 | 协程+输出代码输出不稳定；`tests/text` 原 @thread 版在 StringBuilder 扩容改动后暴露 | ⚠️ 未修（测试已改 pmRun 非协程模式规避；协程输出路径待查） |

---

## 待评审决策点

- **D1**：是否先做 §二-1（字节级精确）再考虑打 tag？
- **D2**：§三-1（`Option`/空安全）与 §五-1（确定性资源管理）是否纳入下一里程碑？
- **D3**：§三/§四/§五 各 P1 项的实施顺序（建议：三-1 → 五-1 → 五-2 → 四-2 → 三-2/三-3）？
- **D4**：Windows 移植是否正式立项（当前为"后续再说"）？
