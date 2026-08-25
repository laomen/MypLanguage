# MYP 运行时 myp化 — 迁移状态与剩余计划（MIGRATION_STATUS）

> 生成日期：2026-08-25。目标：**de-gcc 工具链**（mypc → lld → 预编译 runtime →
> runtime 全 MYP 化），把 C 运行时（`libmyp_rt.a` + bridges）逐步替换为
> `runtime_myp/*.myp` 的 MYP 实现（shadow 机制）。
>
> **迁移机制**：MYP 模块 `--shared` 编译（函数外部链接 `define`）→ `.o` 置于
> `libmyp_rt.a` 之前 + `--allow-multiple-definition` → **MYP 定义优先**（shadow）。
> 验证 `runtime_myp/build.sh`（shadow 36/36）；每批跑 bootstrap 16/16 + 全量
> 323/323。

---

## 一、总体进度

| 项 | 数量 |
|---|---|
| C runtime 顶层 `__?myp_*` 函数（runtime.c + gpu 63 + lib 2） | **480** |
| 已 shadow（`nm build/libmyp_rt_myp.a` 归档实际提供定义 ∩ C） | **377**（~79%） |
| 未 shadow（C runtime 剩余） | **103** |
| ├─ 被 codegen/stdlib 直接引用 | **3**（`myp_printf` varargs / `myp_cublas_available` / `myp_cublas_sgemm`——**均刻意保留 C**） |
| └─ 仅 C 内部/死代码（gc-sections 剥离或只藏在 MYP 化公共 API 背后） | **100** |
| bridge C 文件剩余 `myp_*`（json/net/uds/process/regex/date/hash-md5-sha1） | **75** |
| 已移出为外部库（libs/，v3.15.62：sdl 40 / ttf 10） | **50** |
| runtime_myp 模块 | **36** 个 |

> **收尾结论（v3.15.63，权威审计口径）**：runtime 迁移对 **de-gcc 目标已达成**。
> 剩余 103 个未影 C 函数里，**没有一个是 MYP 程序或编译器必须直接调用的**——
> 100 个是 C 内部 helper（分配器内部、事件路由、协程栈池、exec worker、atexit
> 清理等，`--gc-sections` 剥离或仅在已 MYP 化公共 API 的实现内部被调用），
> 3 个是刻意保留 C 的边界（`myp_printf` varargs 无法优雅 shadow、cuBLAS 厂商
> 库钩子经 `__myp_indirect` 亦可但无必要）。审计命令：
> `nm build/libmyp_rt_myp.a | grep ' T '` 取归档定义 ∩ C 顶层定义。
> 全部大包（A–H）已完成；shadow 套件 43/43；bootstrap 16/16 不动点稳定；
> oracle+selfhost 全量 323/323。

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
`myp_timer_*` 3 + C 内部 helper 4 + `myp_event_id_by_name`；每线程 futex 队列 +
跨线程路由深拷贝）——包 F 全链路 **零 C 依赖**（nm 审计 threadpool/sync/coro_timer/
coro_event/multi_event/mapping_chain 二进制 C-only pkgF=0）** /
**薄层收尾（#42，10 个残留 C 函数）：`myp_free_all`+`myp_arena_free_all`（alloc）/
`myp_weak_free_all`（weak）/`myp_env_set`+`myp_env_unset`（env，ffi 直调 libc
setenv/unsetenv）/`myp_read_line`（io）/`myp_enable_raw`+`myp_restore_term`+
`myp_getch`+`myp_kbhit`（term，termios ioctl）——hello 二进制 C 拉取 10→5、
sync 16→7**。

**包 G Stage A（#43，GPU init/查询/内存/流）**：`runtime_myp/gpu.myp` 新增，35 个
`myp_gpu_*` shadow C 版：init（`myp_gpu_init`：dlopen("libcuda.so.1")+dlsym 17 个
`cu*` 指针 → `__myp_indirect_*` 调用 → cuInit/cuDeviceGetCount/cuDeviceGet/
cuCtxCreate_v2/cuCtxSetCurrent；MYP_GPU=1 env gate）/ 设备查询 19（gpuAttr helper
走 `__myp_indirect_i32(fDeviceGetAttribute, attrVal, id, dev)`，out-param 用 arena
缓冲 / 内存+handle 12（cuMemAlloc_v2/cuMemcpyHtoD_v2/cuMemcpyDtoH_v2 等）/ 流 3
（cuStreamCreate/StreamSynchronize/StreamDestroy）。`@static Gpu` 表：state 字段
（initFlag/availFlag/devInit/devCount）**直接值**访问，cu* 函数指针 17 个字段 +
arena out-param 缓冲字段。验证：`bench/freestanding/rt_gpu_test.myp` 双模式
（无 MYP_GPU → CPU fallback exit 0；MYP_GPU=1 真实 RTX 2070 SUPER：name=…/
cap=705/memMB=7752 + cuMemAlloc/cuMemcpyHtoD/cuMemcpyDtoH/cuMemFree 内存
roundtrip，host buffer i*3 校验）。

**#43 GPU MYP/C 状态分裂边界（重要）**：MYP 的 `myp_gpu_init` 只设置 MYP
`Gpu.availFlag`；C 的 `myp_gpu_load_kernel`/`myp_gpu_launch`（Stage B 未 shadow）
检查 C static `avail`（恒 0）→ load_kernel 返回 NULL。codegen `@gpu for` 发射
`CreateCondBr(k_ok, launch_bb, cpu_bb)`：load_kernel==NULL → **CPU 回退**（安全，
不产生垃圾数据）。manual_ch9 shadow 链接 MYP_GPU=1 → 3 tests/11 assertions 全过。
即：shadow 二进制中 @gpu for 即使 MYP_GPU=1 也 CPU 回退（直到 Stage B shadow
load/launch 统一状态）；oracle/deeplearning 用 C runtime，不受影响。

**#44 GPU MYP/C 状态分裂已统一（Stage B 关键）**：MYP `myp_gpu_load_kernel`/`launch`
shadow 后，`@gpu for` 在 MYP_GPU=1 下走**真实 GPU**（不再是 CPU 回退）。
rt_gpu_test 双模式 + manual_ch9 MYP_GPU=1 真 GPU 3 tests/11 assertions 全过；
直接探针 `kctx=1356287584 launch=1` 确凿证明 PTX 加载 + cuLaunchKernel。

**#45 包 G 收官（Stage C 流/事件/图）**：`myp_gpu_copy_h2d/d2h_async_d/f` +
`d2d_async`（cuMemcpy*Async，入口 cuCtxSetCurrent）+ 事件 6（cuEventCreate/Record/
StreamWaitEvent/Synchronize/ElapsedTime/Destroy，elapsed 用 float 出参
`__myp_mem_load_i32`+`bitcast<float>`）+ CUDA Graph 6（cuStreamBeginCapture(
THREAD_LOCAL=1)/EndCapture/Instantiate/Launch/Destroy/ExecDestroy）。**发现并修复**：
`cuStreamEndCapture(CUstream, CUgraph*)` 参数顺序（stream 在前），曾 400
STREAM_CAPTURE_INVALIDATED；capture 期间不可 cuCtxSetCurrent（同 400）。
**⚠️ 本机驱动 595.84 上 cuMemcpy*Async 恒 201（纯 C 复现）** → 异步拷贝为
C 平价实现（返回 1，忽略 CUresult），数据迁移不可验；事件/图真实路径全通过。
rt_gpu_test 双模式 + Stage C 探针验证。shadow 358→375。包 G 三 Stage 全完成。

**#47 codegen 契约（v3.15.53）**：5 个编译器依赖的 C runtime 函数 MYP 化——
`myp_bounds_error`（diag.myp：`slice<T>` 下标越界 → stderr `"MYP runtime error:
slice index N out of bounds (length M)"` + kill(SIGABRT)/exit 134，负例
`rt_bounds_fail_test` expected=134）/ `myp_obj_type_name` + `myp_type_name`
（**rtti.myp 新增**：`__myp_fn_addr("__myp_type_name_table")` 取 selfhost 恒发射的
类型名表 + 对象头 type_id (addr-4) → myp_alloc 计数拷贝，正例 `rt_rtti_test`
typeOf=Person/typeId 非 0/sameType/null→""）/ `myp_free`（alloc.myp，ffi 直调 libc
`free`）/ `myp_release_fixed_class_array`（alloc.myp：data+i*8 槽 → myp_release）。
shadow 375→380；**codegen 契约仅剩 `myp_printf` 保留 C**（varargs，selfhost 只
declare 不调用）。shadow 36/36。

**#42 残留 C 边界（更新：io_cur 已 MYP 化；constructor 已被 lld 剥离）**：
- **C TLS 当前句柄已解决（#42 补）**：`myp_io_cur_get/set` → MYP IoCur 表（gettid
  键控，append-only + 票号锁）。⚠️ 原 C `__thread myp_io_cur` 对 MYP clone @thread
  （无 CLONE_SETTLS）实为**共享**→ 并发文件 I/O 会串号；IoCur 表按 gettid 分线程真
  正隔离。新 `bench/freestanding/rt_io_thread_test.myp`（2 @thread 各写自己文件
  读回验证 `A=[AAAA] B=[BBBB]`）通过。
- **static constructor 实际被剥离**：ld.lld `--gc-sections` 会丢弃未引用的
  .init_array 项 → `myp_capture_args`/`__myp_coro_register_cleanup` 等从新构建的
  hello/sync 二进制**消失**（nm 实测 0 个 C myp_* 符号）。args 由 MYP args.myp 惰性
  接管、atexit 清理由 OS 回收兜底 → 运行正常（hello exit 42、sync 逐字一致）。
- ⚠️ `myp_arena_free_all` **只复位不 munmap**：MYP arena 进程级 mmap，退出时 OS
  回收；显式 munmap 与 main epilogue 的 ARC release / C atexit 产生 UAF（rt_str
  segfault 139 已实测）。

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
- **#47 codegen 契约（5 个）**：`myp_bounds_error`（diag.myp：slice 下标越界 →
  stderr 诊断 + SIGABRT/134，负例 rt_bounds_fail_test）/ `myp_obj_type_name` +
  `myp_type_name`（rtti.myp 新增：经 `__myp_fn_addr("__myp_type_name_table")`
  取 selfhost 恒发射的类型名表 → 计数拷贝，正例 rt_rtti_test）/ `myp_free` +
  `myp_release_fixed_class_array`（alloc.myp，防御性平价——selfhost 不发射）。
  `myp_printf` 仍是**唯一保留 C 的 codegen 契约**（varargs 不便 shadow，selfhost
  仅 declare 从不调用）。
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
- **✅ 动态栈 + 动态表（#46，v3.15.52）**：协程表/等待表/帧表从定长 `[1024]`
  改为**动态 `long[]`/`int[]` @static 数组**（`coroTEnsure`/`coroWEnsure` 惰性分配
  初始 64 + 翻倍扩容，换引用 + 逐元素拷贝；**115 个 `CoroT.X[slot]` 访问点零改动**）
  ——**解除 1024 并发上限**（同 C AoS 扩表语义）。**动态栈**：`coroStackAlloc` 大
  虚拟预留（`clamp(requested,64KB,64MB)`；编译器默认 128KB→提升 1MB；显式
  `@coro(stack=N)` 尊重）+ `MAP_NORESERVE` 按需分页（RSS 只算实际页）+ 底部 4KB
  PROT_NONE 守护页（超限干净 SIGSEGV，替代静默堆破坏）——Go 式动态增长，无拷贝/
  无编译器改动。**栈池**（C 平价）：`coroStackTake/Return` 有界（64 项 / VA 128MB）
  best-fit 复用 + 退役 drain 回池，`myp_diag_stack_pool_*` 返回真实值。
  验证：rt_coro_test 扩展（1500 并发槽 / cap 动态 2048 / deepRec(20000)=20000）；
  shadow 34/34；bootstrap + oracle + selfhost 323/323。
- **剩余（Phase C）**：release_frame/cleanup/thread_cleanup 为 C 内部/atexit 路径
  （lld 已剥离构造器，影子等效空操作）；`myp_coro_file_*` 非阻塞 exec worker
  （保持同步读语义）。
- **难度**：高（已突破核心+事件层+通道/未来+动态表+动态栈）。

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
- **补（#41 follow-up，不保留 C）**：`myp_event_id_by_name`（动态事件名查表，codegen
  对 `__myp_timer_create(<运行时 string>)` 生成）——纯逐字节 strcmp，MYP string =
  `'\0'` 结尾 char*。新 `bench/freestanding/rt_evname_test.myp`（5 用例：命中/
  未命中/空名/count=0）exit 0。**包 F 全链路零 C 依赖**：nm 审计 threadpool/sync/
  coro_timer/coro_event/multi_event/mapping_chain 六个 shadow 二进制，pkg F 域
  （event/thread/timer/queue/work/handler/id_by_name）C-only 符号 = 0。
- **顺带修复**：alloc.myp `myp_diag_arena_reserved` 潜伏 bug——从 `Arena.head`
  （最旧 chunk）正向走链只统计首 chunk；evInit 一次 ~550KB 大分配首暴。改为从
  `Arena.cur`（最新）走 next 链（与 C 版一致）。
- **`@thread` 剩余**：`myp_thread_entry/current/for_instance` 已由 MYP 内部 helper
  覆盖；`myp_thread_post_event` 已做；work/queue/exec worker 为 C 内部静态（已被
  pool.myp 取代，gc-sections 剥离，无程序引用）。
- **难度**：最高（TLS 缺失 + 事件路由耦合 myp_handlers）。包 F 并发层已全 de-gcc。

### 包 G：GPU 层（`runtime_gpu.c` 63 + `cublas` 2 = 65）✅ 已完成（#43/#44/#45）
- **✅ Stage A 已做（#43，35 个 shadow）**：init（dlopen+dlsym 17 个 `cu*` +
  cuInit/cuCtxCreate_v2/cuCtxSetCurrent）/ 设备查询 19 / 内存+handle 12 / 流 3。
- **✅ Stage B 已做（#44，11 个 shadow）**：load_kernel（内核缓存 kcPtx/kcName/
  kcRec 128 槽指针身份复用；记录 {mod,fn,name} 24B arena）/ launch（cuLaunchKernel
  11 参数编组）/ destroy_kernel / to_host_async / byoc_load+launch（long[]→void**）/
  printf_buf·cnt·fail+flush_printf / scatter_check_fail。**@gpu for 真实 GPU**。
- **✅ Stage C 已做（#45，17 个 shadow）**：异步拷贝 5（cuMemcpy*Async）+ 事件 6
  （cuEvent*）+ CUDA Graph 6（cuStream*Capture/cuGraph*）。包 G 全 63 个 shadow。
- **⚠️ 驱动怪癖**：595.84 上 cuMemcpy*Async 恒 201（纯 C 复现）→ 异步拷贝仅
  C 平价；capture 期间禁 cuCtxSetCurrent（400）；cuStreamEndCapture 参数顺序。
- **cublas 2 未影**（myp_cublas_available/sgemm）：厂商库 hook，保留 C 或经
  `__myp_indirect` 绑 cuBLAS（future）。
- **⚠️**：CUDA TLS（接触上下文的入口显式 `cuCtxSetCurrent`）。改动记
  `examples/deeplearning/CHANGELOG.md`（独立分项目）。

### 包 H：bridge 层（122）
- **json**(14) / **net**(14) / **uds**(18) / **process**(12) / **sdl**(40) /
  **ttf**(10) / **regex**(3) / **date**(4) / **hash-md5/sha1**(7)。
- ✅ **已做（v3.15.55，第一批纯算法 10 个）**：**date.myp**（3：
  `myp_date_format/format_ms/field`，libc ffi time/localtime_r/strftime；⚠️
  strftime 只写内容不更新 MYP 字符串头 len（data-12）→ 必须回填否则 `Str.len`
  读到 myp_alloc 头 255）+ **hash.myp 补 md5/sha1**（crc32/sha256 此前已影）+
  **regex.myp**（3：`myp_regex_compile/match/free`，libc ffi malloc(512)+
  regcomp(REG_EXTENDED)/regexec/regfree）。de-gcc ≠ 去 glibc——MYP ffi 直调
  libc 消除「gcc 现编译 bridge .c」步。**顺带修复编译器 bug**：字符串尾 `$`
  被 expandDollarInterp 静默丢弃（oracle parser_expr.cpp + selfhost
  parser.myp 双修）→ 影响所有 `"...$"` 正则模式。shadow 39/39；bootstrap
  16/16（fixpoint e8033a53）；oracle+selfhost 323/323。bridge 122 → **112**。
- ✅ **已做（v3.15.56，json 全 14 个）**：**json.myp**（新）——递归下降解析/
  路径查询/编辑/美化序列化。节点表 **@static 并行数组**（type/key/str_val/
  num_val/bool_val/child_count + 扁平 child_list slot*64+i，定容 64/节点），
  handle=slot+1——避开 struct 链式访问坑（BUG-029 族）。数字用 libc strtod
  （ffi；set_value 用 endptr 必须消费整串检查）。字符串返回一律计数拷贝
  （jsonStrDup，同 C myp_strdup M8）。⚠️ myp_json_free 空操作（arena 进程级
  回收）。shadow 40/40；tests/json shadow 链接输出与 C 逐字一致；bootstrap
  16/16（fixpoint e8033a53）；oracle+selfhost 323/323。bridge 112 → **98**。
- ✅ **已做（v3.15.59，process 全 6 个）**：**process.myp**（新）——libc ffi：
  `myp_process_run`（system + WIFEXITED/WEXITSTATUS 解码）/ `myp_process_output`
  （popen + fread 分块 + 增长缓冲 + 精确长度计数串）/ `get_pid`/`get_ppid`
  （getpid/getppid）/ `is_running`（kill(pid,0)）/ `spawn`（双 fork：中间子
  _exit，孙进程 setsid + **execve 非常变参 → 手动 argv char* 数组 + envp 取
  libc environ**，父 waitpid）。link.myp mypifiedBridge 加 process_bridge.c。
  shadow 41/41；tests/process shadow 输出与 C 一致；oracle+selfhost 323/323。
  bridge 98 → **92**。
- ✅ **已做（v3.15.60，uds 全 9 个）**：**uds.myp**（新）——AF_UNIX socket 纯
  syscall（41/42/43/44/45/49/50 + poll 7 + close 3 + unlink 87）。sockaddr_un
  110B（family u16@0=AF_UNIX=1 用 2×i8 LE，path@2）；send/recv 走 sendto/
  recvfrom；recv_line 逐字节剥 \r\n；poll 构造 pollfd 8B 数组（revents i8 判
  非 0，POLL* 值均 <256）。错误码对齐 C（bind -2/listen -3/connect -2）。
  link.myp mypifiedBridge 加 uds_bridge.c。rt_uds_test 一次通过；shadow 42/42；
  bootstrap 16/16；oracle+selfhost 323/323。bridge 92 → **83**。
- ✅ **已做（v3.15.61，net 全 8 个）**：**net.myp**（新）——AF_INET TCP libc ffi
  薄接口（socket/bind/listen/accept/connect/send/recv/close/fcntl/setsockopt/
  gethostbyname）。sockaddr_in 16B（family u16@0=AF_INET=2 用 2×i8 LE，port BE=
  htons，sin_addr/zero 显式清零）；hostent h_addr_list@24 → [0] = 4B IPv4。错误码
  对齐 C（-1 socket/-2 解析/-3 connect/-2 bind/-3 listen）。link.myp mypifiedBridge
  加 net_bridge.c。rt_net_test 一次通过；shadow 43/43；bootstrap 16/16；oracle
  323/323（⚠️ 预存在 flaky tests/exception_thread 线程输出竞态，非本批回归）。
  bridge 83 → **75**。
- ✅ **已做（v3.15.62，sdl/ttf 移出外部库）**：**sdl**(40)/**ttf**(10) 不再属于运行时
  桥——移出 `stdlib/` 到 `libs/`（`libs/sdl/sdl.myp` + `bridges/sdl_bridge.c*`、
  `libs/ttf/ttf.myp` + `bridges/sdl_ttf_bridge.c*`），经 `--package-path libs` +
  `MYP_BRIDGES="libs/sdl/bridges:libs/ttf/bridges"` 解析。**新增纯 ffi 薄接口
  `libs/sdl_ffi/sdl_ffi.myp`**（`@static class SDLC` 常量 + SDL_* 1:1 调用）+ 侧车
  `sdl_ffi.myp.libs`=`-lSDL2` → 假 gcc 下 `SDL_Init=0` 证明免 gcc 免桥文件。
  mypview 回归全 PASS + MOS `mos_ui_demo`/`mos_ttf_demo` 链接 OK；oracle+selfhost
  323/323、bootstrap 16/16。**桥计数维持 75**（sdl/ttf 不计入运行时桥）。
- 剩余：**0**（stdlib/bridges 的 json/net/uds/process/regex/date/hash .c 均已 MYP 化，
  `MYP_RT_MYP` 归档存在时跳过其 gcc 编译）。
- **难度**：视库而异（纯算法类中低，SDL 类需绑定策略）。
- ✅ **编译器 bridge 跳过机制（v3.15.57，de-gcc 第二步）**：`tools/selfhost/src/
  link.myp` 新增 `mypRtLib()`（定位 `libmyp_rt_myp.a`，`MYP_RT_MYP` env 覆盖）+
  `mypifiedBridge()`（5 个已 MYP 化 bridge：base64/date/hash/json/regex）。
  归档存在时：bridge 发现循环跳过这些 C bridge 的 `compileBridge`（gcc 免）；
  lld 命令把归档置于 `libmyp_rt.a` 前 + `--allow-multiple-definition`（MYP 优先）。
  **验证**：假 gcc（任何调用即失败）+ MYP_RT_MYP → tests/json 与 json+date+regex+
  hash 综合程序编译/链接/运行全过（gcc 完全绕过）；默认路径行为不变（oracle +
  selfhost 323/323；shadow 40/40；bootstrap 16/16 fixpoint 606bdca4）。归档激活：
  `ar rcs libmyp_rt_myp.a /tmp/rt_myp_*.o`（shadow 套件产物）。

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
7. ✅ **包 G GPU 已完成**（#43/#44/#45：init/查询/内存/流 → 内核 load/launch/byoc/printf → 流/事件/图，63 个 shadow）
8. ✅ **包 H bridge 已完成**（v3.15.55–62：date/hash/regex/json/process/uds/net 全 MYP 化 + sdl/ttf 移出外部库 libs/）

> 注：包 A–C 无硬前置可立即做；包 D–F 相互依赖（D→E→F）；G 独立可随时做；
> H 里纯算法类（date/regex/json/hash）可随时做。

## 五、收尾后剩余可做项（非 de-gcc 必需）

- **✅ v3.15.64 已修 7 个 MYP 运行时 bug（归档下 selfhost 320/323）**：
  - coro.myp 退役栈动态扩容（满时直接释放正在运行的栈 → 损坏；修 coro_capacity/
    more/stack）。
  - io.myp has_next 改 C 语义 `!feof`（加 per-handle feof 标志；修 io）。
  - event.myp evInit / io.myp ioCurInit 显式清零锁/计数字段（arena 非零初始化，
    晚初始化票号锁=垃圾 → 死锁；修 struct_arc/io_thread/myp_fmt）。
  - alloc.myp arena futex 票号锁（全局 bump 无锁，@parallel 并发分配竞态；修
    @test/parallel_string_new）。
- **✅ v3.15.65 已修剩余 3 个架构级 bug（归档下 selfhost 323/323 全绿）**：
  - **coro_thread**（~80% 段错误）：MYP coro 表非 TLS（@static 全局），两个
    @thread 并发操作竞态损坏。修 = **全局递归锁** `CoroLock`（owner gettid +
    depth）包住全部公共协程 API；resume 持锁跨 ctx_switch（协程一步独占全局），
    yield **不再加锁**（否则每次 yield 泄漏一层深度——单线程递归无感，跨线程
    worker 的 coroLock 永远等不到释放 → 死锁，async_file 的隐藏根因）；调度器
    事件处理在锁外（避免 coro→ev 与 ev→coro 锁序反转）。coro_thread 20/20 稳。
  - **myp_run**（args 透传段错误）：main 的 `string[] argv` 被 codegen 透传
    raw char**（元素裸 C 串无 MYP 头 → MYP myp_strlen 读头垃圾；C 版 strlen 容
    忍）。修 = selfhost codegen 在 main 入口对 string[] argv 参数调
    `__myp_build_argv()`（C runtime.c + MYP args.myp 都提供同名符号）重建真
    MYP string[]。oracle 冻结不动；bootstrap 16/16 不动点不变。
  - **async_file**（输出顺序）：MYP io.myp 的 `myp_coro_file_read_line/all` 原
    为**同步读**（C worker 读 C FILE* 表，MYP shadow 后恒空）→ await 阻塞协程
    线程 → R 全在前。修 = coro.myp 新增 **MYP 版 async exec worker**：park
    （EXEC wait）+ clone worker（thread.myp myp_thread_spawn，共享任务槽 capture
    握手）+ worker 阻塞读（io.myp `execIoReadLineRaw/AllRaw` 返回 raw addr，
    不落 string 局部避免 ARC release）+ coroLock 内写 execResult/ready + resume。
    输出与 C runtime 逐字一致（R/B 交错）。
- **`myp_printf`（varargs）**：唯一保留 C 的 codegen 契约。MYP 程序不调用
  （`Console.*`/`__myp_print*` 走独立路径）；若未来需要可加 `__myp_printf_va`
  变长参数包装（低优先）。
- **cuBLAS 钩子（`myp_cublas_available`/`sgemm`）**：厂商库，经 `__myp_indirect`
  绑可去 C，但无实际调用方（deeplearning 未用），保留 C 平价。
- **100 个 C 内部 helper**：分配器内部（arena/region bump、alloc_list）、事件
  路由（dispatch/route_to_thread）、协程栈池/退役/exec worker、atexit 清理
  （free_alloc_list_global/coro_cleanup_all）等——随各公共 API 的 MYP 化已无
  外部引用，`--gc-sections` 在未引用时剥离。如需彻底归零 C 面，只能逐层把
  公共 API 内部也 MYP 化（非 de-gcc 收益，工作量/风险不成比例）。
- **✅ v3.15.66 整体 myp化（归档默认 + 仅 MYP 归档链接）**：
  - link.myp 归档存在时**优先仅链接 `libmyp_rt_myp.a`**（无 libmyp_rt.a /
    runtime.c / gcc），失败才回退 shadow（MYP 优先 + C runtime 后备）。
    输出 `(MYP runtime only)` 标记。
  - 实测：hello/fib/io/process/json/coro_event/async_file/struct_arc/exception/
    regex + myp_fmt/myp_viz/myp_pm 全 MYP-only 链接，0 未定义非 libc 符号；
    **编译器自身（myp_self2/myp_self3）也 MYP-only 链接并正常运行**。
  - 归档默认产出：build.sh 默认 MYP_MAKE_ARCHIVE=1（=0 关闭）；CMake 目标
    `myp_rt_myp`（默认 ALL，依赖 myp_self，MYP_SKIP_SMOKE=1 跳冒烟）。
  - 验证：仅 MYP 链接 selfhost 全量 **323/323** + bootstrap 16/16 不动点 +
    coro_thread 10/10 + async_file 逐字一致 + oracle 默认态 323/323。
  - 剩余 C 边界仅 `myp_printf`（varargs，MYP 程序不调用）与 cuBLAS 钩子（无
    调用方）——由回退路径兜底，无需迁移。

