# MYP 运行时 myp化 — 迁移状态与剩余计划（MIGRATION_STATUS）

> 生成日期：2026-08-25。目标：**de-gcc 工具链**（mypc → lld → 预编译 runtime →
> runtime 全 MYP 化），把 C 运行时（`libmyp_rt.a` + bridges）逐步替换为
> `runtime_myp/*.myp` 的 MYP 实现（shadow 机制）。
>
> **迁移机制**：MYP 模块 `--shared` 编译（函数外部链接 `define`）→ `.o` 置于
> `libmyp_rt.a` 之前 + `--allow-multiple-definition` → **MYP 定义优先**（shadow）。
> 验证 `runtime_myp/build.sh`（shadow 18/18）；每批跑 bootstrap 16/16 + 全量
> 323/323。

---

## 一、总体进度

| 项 | 数量 |
|---|---|
| C runtime 顶层 `__?myp_*` 函数（runtime.c 415 + gpu 63 + stdlib 10 + lib 2） | **490** |
| 已 shadow（runtime_myp 定义 ∩ C，含部分内部 helper） | **137**（~28%） |
| 未 shadow（C runtime 剩余） | **353** |
| bridge C 文件剩余 `myp_*`（json/net/uds/sdl/ttf/process/regex/date/hash-md5-sha1） | **122** |
| runtime_myp 模块 | **20** 个 |

已完成的层（#1–#31 里程碑）：字符串 str / 整数 num（含 `myp_str_parse_int_opt`
long-参数 ABI shadow，#31）/ 浮点 float / 内存核心 alloc（含 `myp_diag_arena_*`，#31）+
arc+region+weak / 文件 I/O io / 时间 time / 文件系统 fs(12) / 环境 get / 命令行参数
args / 终端 term / 数学 math(19) / base64 / crc / hash-sha256 / bytes / 协程上下文
切换 coro(myp_ctx_switch) / 汇编原语 asm。

**编译器内建层（无需 shadow，编译器直发 LLVM）**：Atomic（`__myp_atomic_*` →
`atomicrmw`/load/store）、raw-memory（`__myp_mem_*`/`__myp_syscall`/`__myp_memcpy`）、
数学（`__myp_math_*` → `llvm.*` 标量 intrinsic）、通用间接调用 `__myp_indirect_*`（#30）。

**审计注意（2026-08-25 修正）**：数字按"返回类型含 `[]`/`*` 也计入"重算；`myp_str_
to_bytes` 早已在 bytes.myp；`myp_str_cat/cpy/fmt/len` **无 MYP 调用方**（C 内部/
死代码，不影）；**print 层与 @test 捕获耦合**（见包 A）。

---

## 二、剩余工作包（按依赖与建议顺序）

### 包 A：残留薄层（~30 个，快赢，无前置）
- ✅ **已做（#31/#32）**：`myp_str_parse_int_opt`（num.myp，**long 参数 ABI 兼容**
  shadow 编译器 `(ptr,ptr)` 调用）；`myp_diag_arena_reserved/used`（alloc.myp，
  `Arena.total_used`）；**console+test 框架包（#32 output.myp + test.myp）**——
  print 族 + @test 捕获（`myp_test_capture_*`）+ 断言（`myp_assert*`）+ 报告
  （`myp_test_report/summary/fail_msg/set_msg`）。**关键语义**：@test 按退出码验证、
  断言失败走 stderr（不经 stdout 捕获）、`myp_test_summary` 返回 fail>0、curMsg
  仅失败消费。
- **`print` 族**：✅ 全影（output.myp；`myp_print_float` 已在 float.myp 走
  `__myp_print` 自动接捕获）。`myp_printf`（varargs）保留 C（MYP 程序不调用）。
- **`str` 残留**：`myp_str_to_bytes` 已影（bytes.myp）；`myp_str_cat/cpy/fmt/len`
  **无 MYP 调用方**（C 内部/死代码，不影）。
- **`io` 残留(6)**：`myp_io_cur_get/set`（**保留 C TLS**，io_thread 每线程句柄）、
  `myp_io_init`、`myp_io_lock_current/handle`、`myp_io_unlock_handle`。
- **`env`(2)**：`myp_env_set/unset`——**保留 C**（setenv/unsetenv 无 syscall 等价物，
  已文档化）。
- **`out`(2)/`release`(2)/`capture`(2)/`asan`(2)/`weak` 内部(8)**：`myp_out_write(_n)`、
  `myp_release_fixed_class_array`/`release_slot`、`myp_capture_args`（C 构造器）、
  `myp_asan_start/finish_switch`（ASAN 专用）、`myp_weak_*_locked` 等 C 内部 helper
  （weak.myp 已实现逻辑，这些可随 alloc/weak 内部重构移除或 MYP 化）。
- **难度**：低。**价值**：进一步减 C 面，特别是 `myp_str_parse_int_opt` 补齐编译器
  依赖。

### 包 B：诊断/内存统计（~20 个，读已迁移内部状态）
- **`diag`(13)**：`myp_diag_arena_reserved/used`、`myp_diag_coro_slots/...`、
  `myp_diag_stack_pool_*`、`myp_diag_retired_*`、`myp_diag_get/set_strict`。
- **`live`(1)**：`myp_live_object_count_by_type`。
- 内部 `alloc_list`/`free_*`/`type_live`/`make_*`/`arr_*`（~20）：分配器/数组/类型计数
  内部 helper——多数可并入 alloc.myp/region.myp 内部重构。
- **难度**：中低。读 arena/region/coro 内部计数（MYP 已管），重写为 MYP 状态读取。

### 包 C：异常机制（exception 5 + throw 2 + error 3 + strict 2 = 12）
- ✅ **已做（#33 exception.myp）**：`myp_throw`/`myp_throw_object`/`myp_get_error`/
  `myp_error_setup/is_active/clear`/`myp_exception_push/pop/get_jmpbuf/get_type/
  get_object`/`myp_try_escape`/`myp_release_slot`/`__myp_longjmp`/`myp_diag_get/
  set_strict`。**setjmp 用 libc**（`call @setjmp`，非 myp_* 不影）；`__myp_longjmp`
  经 ffi 调 libc longjmp（ABI 兼容）。**协程 Phase A 的异常边界前置已具备**（剩余
  前置 = 线程创建 pthread）。验证：rt_exception_test（字符串/嵌套/类型化/finally）。
- **`error`(3)**：`myp_error_setup/is_active/clear` 已影（#33）；`myp_get_error` 已影。

### 包 D：协程运行时（`__myp_coro_*` 49 + `channel` 13 = 62）
- **coro**：create/set_entry/yield/resume/result/is_active/request_cancel/destroy/
  status/scheduler/wait_event(_timeout)/wait_any(_of)/sleep/wait_fd/frame_set/clear/
  release_frame/current_handle/count/trampoline/cleanup/... + 栈池/退役/槽位代际。
- **channel**：create/send/recv/try_send/try_recv/close/destroy/size/wake_one/...
- **策略**：CORO_DESIGN Phase A 已规划。`myp_ctx_switch`（#25/#26）就绪。前置 =
  **包 C 异常边界** + **线程创建**。
- **难度**：高。并发层地基。

### 包 E：同步原语（mutex 7 + cond 6 + sem 6 + rwlock 8 + once 5 + barrier 3 + future 4 = 39）
- **策略**：futex syscall(202) 实现互斥/条件变量；或 `__myp_indirect` 调 pthread。
  `future` 与协程 wait/wake 联动（`myp_coro_wait_future`）。
- **难度**：中高。依赖包 D（future 联动）。

### 包 F：并发/线程层（thread 11 + pool 11 + event 10 + work 5 + queue 7 = 44）
- **✅ 地基已就绪（#34）**：`myp_thread_spawn`（`thread.myp`）—— clone syscall 直建
  线程（不保留 C/pthread），mmap 新栈 + CLONE_VM|FS|FILES|SIGHAND|THREAD|SYSVSEM +
  子跑共享入口后 syscall 60 退出。已验证：探针 + 3 并发子线程测试（8/8 稳定）。
  关键：**子分支不声明局部**（子 RSP=新栈顶无 frame，正偏移栈写越界）→ 只传实参给
  helper `myp_thread_child_entry` 建帧 + capture entry → `done_read=1` 确定性握手
  （父等 done_read 才覆盖 entry，无竞态）。无 CLONE_SETTLS（共享父 TLS，符合 MYP
  @static 非 TLS 模型）；无 join API（靠共享标志/未来 futex）。
- **`@parallel for`**：`myp_pool_create/ensure_global/parallel_for/destroy/worker_id/
  thread_count/set_threads/worker_count/is_active` + work-stealing 队列（work 5）+
  任务队列（queue 7）。
- **`@thread`**：`myp_thread_create/self/stop/destroy/is_current/associate_instance/
  post_event/run_loop/entry/current/for_instance` + 事件路由（event 10：
  `myp_event_route_to_thread`、dispatch/fire/process_*/register/scope）。
- **难点**：pthread_create 已替换为 clone 原语；事件路由深耦合 C 的
  myp_handlers。依赖包 D/E。
- **难度**：最高。做完才能真正 de-gcc 掉并发层。

### 包 G：GPU 层（`runtime_gpu.c` 63 + `cublas` 2 = 65）
- init（dlopen+dlsym 44 个 `cu*` 指针）/ 设备查询 13 / 内存+handle / 内核
  load+launch+byoc / printf / 流/事件/图（cuStream*/cuEvent*/cuGraph*）。
- **使能已就绪**：`__myp_indirect_*`（#30）+ `ffi long dlopen/dlsym`（libc 导出）。
- **Stage**：A 查询/内存/handle → B 内核（cuModuleLoadDataEx+PTX 缓存+launch 参数
  编组）→ C 流/事件/图（deeplearning）。无卡环境只验「初始化失败→CPU 回退」。
- **⚠️**：CUDA TLS（接触上下文的入口显式 `cuCtxSetCurrent`）；改动记
  `examples/deeplearning/CHANGELOG.md`（独立分项目）。

### 包 H：bridge 层（122）
- **json**(14) / **net**(14) / **uds**(18) / **process**(12) / **sdl**(40) /
  **ttf**(10) / **regex**(3) / **date**(4) / **hash-md5/sha1**(7)。
- 部分纯 MYP 可实现（date 格式化、regex 引擎、json 解析器、process 的 fork/exec
  可经 `__myp_syscall` 的 clone/execve）；sdl/ttf 需 C 库（SDL2），MYP 只能经
  `__myp_indirect` 绑 SDL2 或保留 bridge。
- **难度**：视库而异（纯算法类中低，SDL 类需绑定策略）。

---

## 三、编译器中立项（能力使能清单）

| 能力 | 状态 |
|---|---|
| `__myp_indirect_*`（通用间接调用，dlopen 后调任意签名 C API） | ✅ #30 |
| `__myp_asm`/`__myp_asm_r`（内联汇编） | ✅ #26 |
| `__myp_fn_addr` / `__myp_rtable_addr` / `__myp_call_ptr` | ✅ #25/#24 |
| raw-memory / syscall / 原子内建 | ✅ |
| `ffi long dlopen/dlsym` | 🔲 待加（libc 导出，`-ldl` 已链） |
| MYP `ptr` 参数类型（`myp_str_parse_int_opt` 等） | 🔲 待加（mypc 未冻结） |
| setjmp 异常边界（协程前置） | 🔲 包 C |

---

## 四、建议推进顺序

1. ✅ **包 A 残留薄层**（#31/#32：str_parse_int_opt / diag_arena / console+test）
2. **包 B 剩余诊断**（`myp_fail_alloc_*` 注入 + alloc 接入；diag_coro/stack 等读协程
   状态 → 待包 D）
3. ✅ **包 C 异常机制**（#33 exception.myp，协程前置之一）
4. **包 D 协程 Phase A**（`myp_ctx_switch` 就绪 + 异常边界已具备；线程创建已用 clone
   直建 #34，不再依赖 pthread）
5. **包 E 同步原语**（futex 或 pthread 间接调）
6. **包 F 线程/pool**（pthread_create OS 边界，最难）
7. **包 G GPU**（`__myp_indirect` 就绪，可并行推进）
8. **包 H bridge**（按库分批）

> 注：包 A–C 无硬前置可立即做；包 D–F 相互依赖（D→E→F）；G 独立可随时做；
> H 里纯算法类（date/regex/json/hash）可随时做。
