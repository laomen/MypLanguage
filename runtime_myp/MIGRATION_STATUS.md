# MYP 运行时 myp化 — 迁移状态与剩余计划（MIGRATION_STATUS）

> 生成日期：2026-08-25。目标：**de-gcc 工具链**（mypc → lld → 预编译 runtime →
> runtime 全 MYP 化），把 C 运行时（`libmyp_rt.a` + bridges）逐步替换为
> `runtime_myp/*.myp` 的 MYP 实现（shadow 机制）。
>
> **迁移机制**：MYP 模块 `--shared` 编译（函数外部链接 `define`）→ `.o` 置于
> `libmyp_rt.a` 之前 + `--allow-multiple-definition` → **MYP 定义优先**（shadow）。
> 验证 `runtime_myp/build.sh`（shadow 30/30）；每批跑 bootstrap 16/16 + 全量
> 323/323。

---

## 一、总体进度

| 项 | 数量 |
|---|---|
| C runtime 顶层 `__?myp_*` 函数（runtime.c 415 + gpu 63 + stdlib 10 + lib 2） | **490** |
| 已 shadow（runtime_myp 定义 ∩ C，含部分内部 helper；#41 后重算） | **299**（~61%） |
| 未 shadow（C runtime 剩余） | **191** |
| bridge C 文件剩余 `myp_*`（json/net/uds/sdl/ttf/process/regex/date/hash-md5-sha1） | **122** |
| runtime_myp 模块 | **21** 个 |

已完成的层（#1–#37 里程碑）：字符串 str / 整数 num（含 `myp_str_parse_int_opt`
long-参数 ABI shadow，#31）/ 浮点 float / 内存核心 alloc（含 `myp_diag_arena_*`，#31）+
arc+region+weak / 文件 I/O io / 时间 time / 文件系统 fs(12) / 环境 get / 命令行参数
args / 终端 term / 数学 math(19) / base64 / crc / hash-sha256 / bytes / 汇编原语 asm /
诊断 diag(type_live + fail_alloc，#35) / **协程 coro 全生命周期（#36 Phase A 核心 +
#37 Phase B 事件/等待层 + 帧表 + 诊断 + #38 Phase C channel/future）** /
**同步原语 sync（#39 futex 无 libc：mutex/sem/cond/rwlock/once/barrier）** /
**线程池 pool（#40 clone worker + futex + work-stealing：`myp_pool_*` 8 个核心 API**
（ensure_global/parallel_for/worker_id/thread_count/set_threads/worker_count/
is_active/destroy）**）** /
**@thread 生命周期 + 事件系统（#41 event.myp：`myp_thread_*` 8 + `myp_event_*` 7 +
`myp_timer_*` 3 + C 内部 helper 4；每线程 futex 队列 + 跨线程路由深拷贝）**。

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
- ✅ **已做（#35 diag.myp）**：`myp_live_object_count_by_type` + `myp_type_live_inc/dec`
  （per-type 计数，挂 MYP alloc/free）+ `myp_fail_alloc_enable/disable/get/check`
  （确定性注入，enable/env 双路径）。`myp_diag_arena_reserved/used`（#31）、
  `myp_diag_get/set_strict`（#33）、`myp_diag_region_reserved/used`（region.myp）。
- **`diag` 剩余(10)**：`myp_diag_coro_slots/capacity/free_slots`、
  `myp_diag_stack_pool_count/capacity/bytes/max_bytes`、`myp_diag_retired_count/bytes`
  ——**读协程运行时状态 → 依赖包 D**，协程迁移后影（C 版仍生效）。
- 内部 `alloc_list`/`free_*`/`make_*`/`arr_*`（~20）：分配器/数组内部 helper——C 的
  侵入链表 node 本就被 MYP 跳过（alloc.myp 注），其余可随 alloc/region 内部重构移除或
  MYP 化。
- **难度**：中低。验证：rt_diag_test + 注入探针（enable/env 双路径 SIGABRT）。

### 包 C：异常机制（exception 5 + throw 2 + error 3 + strict 2 = 12）
- ✅ **已做（#33 exception.myp）**：`myp_throw`/`myp_throw_object`/`myp_get_error`/
  `myp_error_setup/is_active/clear`/`myp_exception_push/pop/get_jmpbuf/get_type/
  get_object`/`myp_try_escape`/`myp_release_slot`/`__myp_longjmp`/`myp_diag_get/
  set_strict`。**setjmp 用 libc**（`call @setjmp`，非 myp_* 不影）；`__myp_longjmp`
  经 ffi 调 libc longjmp（ABI 兼容）。**协程 Phase A 的异常边界前置已具备**（剩余
  前置 = 线程创建 pthread）。验证：rt_exception_test（字符串/嵌套/类型化/finally）。
- **`error`(3)**：`myp_error_setup/is_active/clear` 已影（#33）；`myp_get_error` 已影。

### 包 D：协程运行时（`__myp_coro_*` 49 + `channel` 13 = 62）
- ✅ **已做（#36，Phase A 生命周期核心）**：create/set_entry/yield/resume/set_result/
  result/status/is_active/count/current_handle/request_cancel/cancel_requested/
  cancel_clear/destroy/scheduler/trampoline + set_entry_arg/get_entry_arg（20 个）。
  协程表 @static 并行数组 + 代际句柄 + 空闲槽复用；栈 mmap + 退役列表；trampoline
  异常边界 = MYP try/catch；scheduler 合作式推进 ready。`myp_ctx_switch`（#25/#26）。
- ✅ **已做（#37，Phase B 事件/等待层 + 帧表 + 诊断）**：
  - **事件/等待层**：wait_event(_timeout)/wait_any/wait_any_of/sleep/wait_fd +
    `__myp_coro_event_notify`（C myp_event_dispatch 调用，已去 static 可 shadow）。
    等待表 `@static CoroW` 并行数组（kind/eventId/fd/fdEvents/handle/deadline/
    waitIndex，定容 1024）；scheduler 加 压缩 + `myp_event_process_all()`（C 队列
    → dispatch → MYP notify）+ 截止期过期（waitTimeout 标记）+ fd 批量 poll
    （syscall 7）。语言级 `await evt`/`await evt timeout N` 全路径验证
    （coro_timeout/coro_any 输出逐字一致）。
  - **帧表**：frame_set/frame_clear 真实现 + coroReleaseFrame（`myp_release(
    __myp_addr_to_str(obj))`）→ destroy/未捕获异常路径 release 帧 ARC
    （coro_frame_arc 输出逐字一致：destroy/异常零泄漏）。
  - **诊断**：myp_diag_coro_slots/slot_capacity/free_slots + retired_count/bytes
    （栈池无 → 恒 0，Phase C）。
  - **测试**：`bench/freestanding/rt_coro_wait_test.myp`（事件到达/超时/waitAny/
    sleep/waitAnyOf 总体超时/waitFd-pipe，6 项）。
- ✅ **已做（#38，Phase C channel + future）**：
  - **Channel**：`myp_channel_create/destroy/send/recv/try_send/try_recv/size/close`
    + 内部 wake_one（同步交接）/wake_ready（close/try 只 ready）。`@static Chan` 并行
    数组（buf 环形缓冲/head/count/capacity/closed + 256×256 recvW/sendW FIFO waiter）。
    同步交接：协程调用方内联 resume 对端一步（深度守卫 64）；非协程只 ready=1。
  - **Future**：`myp_future_create/set/get/destroy` + `myp_coro_wait_future/wake_future/
    am_i_coro`。协程 get 未 ready → park（不阻塞线程）；同线程 set 唤醒。非协程未
    ready → 自旋 + sleep 回退（MYP 无 pthread_cond；主用例 set 先于 get）。
  - **⚠️ CoroT.current 初始化 bug（#38 发现+修复）**：`--shared` 模式下 @static 属性
    默认值不生效（全局 zeroinitializer）→ `CoroT.current = -1` 实际从 0 起，恰等于
    首个协程槽号 → main（非协程）在 spawn 后被误判为「在协程 0」→ channel/wait 走
    协程分支（内联 resume/误 park）。修复：`coroEnsureInit()` 一次性显式置
    current=-1，挂在全部公共 API 入口（26 处）。
  - **exec worker**：`myp_coro_file_read_line/all` 已 #17 同步化（io.myp）——保留
    （同步读语义，非阻塞 await 未做）。
  - **测试**：`bench/freestanding/rt_coro_chan_future_test.myp`（channel 基本/
    producer park/consumer park/close + future ready/协程 park，6 项）。
- **剩余（Phase C）**：release_frame/cleanup/thread_cleanup、栈池、
  `myp_coro_file_*` 非阻塞 exec worker。
- **难度**：高（已突破核心+事件层+通道/未来，剩栈池/cleanup/非阻塞 exec）。

### 包 E：同步原语（mutex + cond + sem + rwlock + once + barrier = 30；future 已在 #38）
- ✅ **已做（#39 sync.myp，futex 无 libc）**：Mutex（票号锁 serving/next + futex，
  recursive 用 owner(tid)+depth 由票号锁守护）/ Semaphore（原子计数可负 + futex）/ 
  CondVar（waiters 计数 + seq，wait 释放关联 Mutex、唤醒后重取）/ RWLock（state：
  0 空闲 / >0 读者 / <0 写者）/ Once（done + 自带票号锁，**不复用 Mutex 表**——
  once_create 在全局分配锁临界区内调 myp_mutex_create 会自死锁，曾踩）/ Barrier
  （arrived 计数 + seq）。全部 futex syscall(202)（WAIT=128/WAKE=129）+ 原子
  `__myp_atomic_*_addr`，**不依赖 libc/pthread**；表 @static 进程级（clone 线程共享
  地址空间 → futex 跨线程有效）。⚠️ once_create 嵌套分配锁自死锁是首版 bug。
- **测试**：`bench/freestanding/rt_sync_test.myp`（单线程 API 全 + 2 worker 跨线程
  互斥累加 200）；`tests/sync`（4 @thread worker mutex_count=400 + condvar=42 +
  各 API）用 MYP shadow 链接**输出逐字一致**。
- **难度**：中高（futex + 无 CAS 的 add/sub 设计；once 嵌套锁坑）。

### 包 F：并发/线程层（thread 11 + pool 11 + event 10 + work 5 + queue 7 = 44）
- **✅ 地基（#34）**：`myp_thread_spawn`（`thread.myp`）—— clone syscall 直建
  线程（不保留 C/pthread），mmap 新栈 + CLONE_VM|FS|FILES|SIGHAND|THREAD|SYSVSEM +
  子跑共享入口后 syscall 60 退出。关键：**子分支不声明局部**（子 RSP=新栈顶无
  frame，正偏移栈写越界）→ 只传实参给 helper `myp_thread_child_entry` 建帧 +
  capture entry → `done_read=1` 确定性握手。无 CLONE_SETTLS；无 join API。
- **✅ `@parallel for` 已做（#40 pool.myp，futex 无 libc）**：全局单池 `@static
  PoolTab`；ensure_global 首次建池（sched_getaffinity=204 数 CPU → clone N worker，
  spawn 握手 = 父设共享 spawnTid + 子写 workerReady，gettid=186 查表得 worker_id）；
  parallel_for 分 ≤nThreads*4 块入各 deque（票号锁）→ workSeq futex 广播唤醒 →
  barrier 等 doneCount==totalChunks；worker pop 自家 → steal 他人 → 空则 futex 等。
  8 个 `myp_pool_*` shadow C 版（ensure_global/parallel_for/worker_id/
  thread_count/set_threads/worker_count/is_active/destroy）。worker 并发跑
  parallel body（**body 不得分配 arena**，与 thread.myp 同限制）。
- **测试**：`bench/freestanding/rt_pool_test.myp`（2 worker：int/long @parallel
  1000 累加=499500、10×100 累加、isActive/workerCount/threadCount）；`tests/@test/
  manual_ch9_myp.myp` 用 MYP shadow 链接 **3 tests / 11 assertions 全过**。
- **✅ `@thread` 生命周期 + 事件系统已做（#41 event.myp，futex 无 libc/TLS）**：
  `myp_thread_create/run_loop/stop/destroy/self/is_current/associate_instance/
  post_event`（8）+ 事件系统 `myp_event_register/push_scope/pop_scope/fire/
  process_all/process_one/route_to_instance`（7）+ 定时器 `myp_timer_create/check/
  cancel_all`（3）+ C 内部 helper（dispatch/route_to_thread/timer_next_delay_ms/
  thread_current）。机制：
  - **每线程事件队列** = arena 环形缓冲（256）+ 每队列票号锁 + futex seq 字；
    「当前线程」= gettid(186) 扫线程表（clone 子线程共享父 TLS → MYP 不能靠 TLS）。
  - 线程槽 @static EvTab（slot 0=主线程）；create 返回 slot（编译调用方当不透明
    ptr）；spawn 握手 = 父设 spawnSlot → myp_thread_spawn（thread.myp clone）→
    子写 ready；子事件循环跑 startup → while(running){ process_all + qWait(下个
    timer 截止) } → done=1 → syscall 60；destroy 轮询 done。
  - **跨线程事件路由**（BUG-005）：dispatch 两遍（先路由他线程同目标只投一份 +
    深拷贝载荷 mmap→munmap；再跑本线程 handler）→ `__myp_coro_event_notify`。
  - handler 表/定时器/实例映射共用全局 futex 票号锁；timer 载荷用每线程 tPval 缓冲。
  - ⚠️ @thread 子线程 @startup/body 不并发分配共享 arena（bump 无锁，同 pool）。
- **测试**：`bench/freestanding/rt_threadpool_test.myp`（@thread + 4 worker
  @threadpool 打印 "wwww"）；`tests/threadpool` 与 `tests/sync`（4 @thread worker
  mutex_count=400）shadow 链接**逐字一致**；coro_event/coro_timer/startup/
  multi_event/mapping_chain/scope_mapping/lambda_mapping shadow 逐字一致。
- **顺带修复**：alloc.myp `myp_diag_arena_reserved` 潜伏 bug——从 `Arena.head`
  （最旧 chunk）正向走链只统计首 chunk；evInit 一次 ~550KB 大分配首暴。改为从
  `Arena.cur`（最新）走 next 链（与 C 版一致）。
- **`@thread` 剩余**：`myp_thread_entry/current/for_instance` 已由 MYP 内部 helper
  覆盖；`myp_thread_post_event` 已做；work 任务队列 / exec worker 未做。
- **难度**：最高（TLS 缺失 + 事件路由耦合 myp_handlers）。做完后并发层基本 de-gcc。

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
2. ✅ **包 B 诊断**（#35 diag.myp：type_live + fail_alloc；coro 诊断读协程状态待
   包 D）
3. ✅ **包 C 异常机制**（#33 exception.myp，协程前置之一）
4. ✅ **包 D 协程**（#36/#37/#38 coro.myp 全生命周期）
5. ✅ **包 E 同步原语**（#39 sync.myp futex 无 libc）
6. ✅ **包 F pool + @thread + 事件系统**（#40 pool.myp；#41 event.myp
   thread/event 路由全做；剩余 work/queue/exec worker 小项）
7. **包 G GPU**（`__myp_indirect` 就绪，可并行推进）
8. **包 H bridge**（按库分批）

> 注：包 A–C 无硬前置可立即做；包 D–F 相互依赖（D→E→F）；G 独立可随时做；
> H 里纯算法类（date/regex/json/hash）可随时做。
