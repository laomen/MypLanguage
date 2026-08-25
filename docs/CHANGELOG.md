# MYP 变更日志 (CHANGELOG)

> 语言规格版本：**1.0**（自 `docs/grammar.md` 发布起语法冻结）
> 编译器版本与语言规格版本分离：
> - **编译器版本**（`mypc --version`）跟踪实现进度，如 `v2.4.0`。
> - **语言规格版本**（`Language Spec`）只在**破坏性语法/语义变更**时递增。

## 版本策略（冻结约定）

自语言规格 v1.0 起：

- **破坏性变更**（删除/重命名语法、改变语义、改关键字）**仅允许在规格主版本
  递增时引入**（如 1.0 → 2.0）。此类变更必须在此文件显式记录"破坏性变更"条目。
- **非破坏性变更**（新增语法糖、新增标准库、新增注解、新增内置函数）可在任意
  编译器版本引入，不改变规格主版本。
- 每次提交必须保持 `docs/grammar.md` 与实现一致；新增/修改语法后需同步更新规格。

---

## 语言规格

| 规格版本 | 发布日期 | 说明 |
|---------|---------|------|
| **1.0** | 当前 | 语法冻结基准。正式发布 `docs/grammar.md` EBNF。语法自此受变更策略约束。 |

---

## 编译器版本历史

### v3.15.41 — runtime myp化 #37：包 D 协程 Phase B（事件/等待层 + 帧表 + 诊断）

**非破坏性**。协程事件/等待层 + ARC 帧表 MYP 化——shadow C 的 `__myp_coro_*`
事件契约（CORO_DESIGN Phase B 切片）：

- **事件/等待层（coro.myp）**：`__myp_coro_wait_event(_timeout)`/`wait_any`/
  `wait_any_of`/`sleep`/`wait_fd` + `__myp_coro_event_notify`。等待表 `@static CoroW`
  并行数组（kind/eventId/fd/fdEvents/handle/deadline/waitIndex，定容 1024）；等待 =
  注册 + park（ready=0）+ yield，唤醒由 scheduler 驱动。
- **调度器增强**：等待表压缩（drop inactive，防 O(N²) 累积）+ `myp_event_process_all()`
  （C 事件队列 → dispatch → MYP notify）+ 截止期过期（waitTimeout 标记区分超时 vs
  到达）+ fd 批量 poll（syscall 7，pollfd 8B 布局）。
- **C 桥**：`__myp_coro_event_notify` 去 static（runtime.c）——MYP 版 shadow 后可截获
  C `myp_event_dispatch` 的协程唤醒。
- **帧表 ARC 镜像**：`frame_set`/`frame_clear` 真实现（每协程 ≤32 槽，扁平数组；
  obj 为堆指针，`myp_release(__myp_addr_to_str(obj))`）+ `coroReleaseFrame` 挂进
  destroy（三路径）+ trampoline 收尾 → 强杀/未捕获异常零泄漏。
- **诊断**：`myp_diag_coro_slots/slot_capacity/free_slots` + `retired_count/bytes`
  （栈池无 → 恒 0，Phase C）。
- **验证**：新 `bench/freestanding/rt_coro_wait_test.myp`（事件到达/超时/waitAny/
  sleep/waitAnyOf 总体超时/waitFd-pipe，6 项 exit=0）；**shadow 26/26**；语言级
  `await evt`/`await evt timeout N` + `Coro.waitAny` + 帧 ARC 对拍——
  `tests/coro_timeout`/`coro_any`/`coro_frame_arc` 用 MYP shadow 链接**输出逐字一致**；
  bootstrap 16/16（fixpoint `9f5cf25b`，runtime 重链编译器逻辑不变）；全量 323/323。
- **未做（Phase C）**：通道、future（wait/wake_future）、exec worker、栈池、
  thread_cleanup/cleanup_all。

### v3.15.42 — runtime myp化 #38：包 D 协程 Phase C（channel + future + current 初始化修复）

**非破坏性**。协程 Channel + Future MYP 化——shadow C 的 `myp_channel_*`/
`myp_future_*`/`myp_coro_{wait,wake}_future`/`myp_coro_am_i_coro`（CORO_DESIGN
Phase C 切片）：

- **Channel**：`myp_channel_create/destroy/send/recv/try_send/try_recv/size/close`
  + 内部 `wake_one`（同步交接：协程调用方内联 resume 对端一步，深度守卫 64）/
  `wake_ready`（close/try 只 ready）。`@static Chan` 并行数组（环形缓冲
  head/count/capacity/closed + 256×256 recvW/sendW FIFO waiter）。协程 send 满/
  recv 空 → park；非协程满/空 → -1（不挂起）。
- **Future**：`myp_future_create/set/get/destroy` + `myp_coro_wait_future/
  wake_future`/`am_i_coro`。协程 get 未 ready → park（不阻塞线程）；同线程 set
  唤醒；非协程未 ready → 自旋 + sleep 回退（MYP 无 pthread_cond）。
- **⚠️ CoroT.current 初始化 bug（本里程碑发现+修复）**：`--shared` 库模式下
  @static 属性默认值不生效（全局 zeroinitializer）→ `CoroT.current = -1` 实际从
  0 起，恰等于首个协程槽号 → main（非协程）在 spawn 后被误判为「在协程 0」→
  channel/wait 走协程分支（内联 resume 改变输出时序/误 park）。修复：
  `coroEnsureInit()` 一次性显式置 current=-1，挂在全部公共 API 入口（26 处）。
  该 bug 亦影响此前 Phase A/B（被「首协程恰为槽 0」掩盖）。
- **验证**：新 `bench/freestanding/rt_coro_chan_future_test.myp`（channel 基本/
  producer park/consumer park/close + future ready/协程 park，6 项 exit=0）；
  **shadow 27/27**；`tests/coro_channel`/`coro_future`/`future`/
  `channel_multi_consumer` 用 MYP shadow 链接**输出逐字一致**；
  `tests/stress/channel_stress`（cap=1/8，sum=199990000）PASS；bootstrap 16/16
  （fixpoint 不变）；全量 323/323。
- **未做（Phase C 尾）**：非阻塞 exec worker、栈池、cleanup_all/thread_cleanup。

### v3.15.43 — runtime myp化 #39：包 E 同步原语（sync.myp，futex 无 libc）

**非破坏性**。同步原语 MYP 化——shadow C 的 `myp_mutex_*`/`myp_sem_*`/`myp_cond_*`/
`myp_rwlock_*`/`myp_once_*`/`myp_barrier_*`（30 个），**不依赖 libc/pthread**：

- **futex 实现**（syscall 202，FUTEX_WAIT=128/WAKE=129 PRIVATE）+ 原子
  `__myp_atomic_add/sub/load/store_i32_addr`（atomicrmw，无 CAS）。
- **Mutex**：票号锁（serving/next 两字）+ futex；unlock 广播唤醒。recursive 用
  owner(tid via gettid 186) + depth，由票号锁守护。tryLock 用票号归还（安全：
  未持有票号 serving 不会越过）。
- **Semaphore**：原子计数（可负=等待者数）+ futex wait/wake；tryWait 归还。
- **CondVar**：waiters 计数 + seq（经典 futex seq 条件变量）；wait 释放关联
  Mutex、唤醒后重取；`while(!cond) wait()` 模式安全。
- **RWLock**：state（0 空闲 / >0 读者 / <0 写者）+ futex；tryRead/WriteLock 原子尝试。
- **Once**：done + **自带票号锁**（不复用 Mutex 表——once_create 在全局分配锁
  临界区内调 myp_mutex_create 会**嵌套分配锁自死锁**，首版 bug 已修）。
- **Barrier**：arrived 计数 + seq + futex（可复用，最后到达者换代唤醒全部）。
- **全局槽分配锁**：所有 create 用单一把 futex 票号锁互斥扫描空闲槽。
- **验证**：新 `bench/freestanding/rt_sync_test.myp`（单线程 API 全 + 2 worker
  跨线程互斥累加 200）；**shadow 28/28**；`tests/sync`（4 @thread worker
  mutex_count=400 + condvar=42 + tryLock/recursive/rwlock/sem/once API）用 MYP
  shadow 链接**输出逐字一致**；bootstrap 16/16（fixpoint 不变）；全量 323/323。
- **未做**：`myp_sem_getvalue` 等 C 内部 helper；future 已在 #38。

### v3.15.44 — runtime myp化 #40：包 F `@parallel for` 线程池（pool.myp，futex 无 libc）

**非破坏性**。`@parallel for` 线程池 MYP 化——shadow C 的 `myp_pool_*` 8 个核心
API（`ensure_global`/`parallel_for`/`worker_id`/`thread_count`/`set_threads`/
`worker_count`/`is_active`/`destroy`），**不依赖 pthread/libc**：

- **全局单池** `@static PoolTab`（arena 缓冲字段 + 直接值字段分层；--shared
  默认值不生效 → 显式 lazy 分配）。首次 `ensure_global` 建池：`sched_getaffinity`
  （syscall 204）数硬件并发 → `myp_thread_spawn`（clone，#34）建 N worker。
- **spawn 握手**：父设共享 `spawnTid` + 子读后写 `workerReady`（父轮询等 ready
  才 spawn 下一个，无竞态）；worker_id 用 `gettid`（syscall 186）查表。
- **parallel_for**：`[start,end)` 按 step 分 ≤ nThreads*4 块，round-robin 推入
  各 worker 的 work-stealing deque（票号锁守护）→ `workSeq` futex 广播唤醒 →
  barrier 等 `doneCount==totalChunks`（futex seq 字）。worker：pop 自家底部 →
  steal 他人顶部 → 空则 futex 等 workSeq。
- **并行 body 经 `__myp_indirect_void` 调用**（body 4 参 + fn 地址 = 5 实参），
  arg = 捕获的局部变量结构体。⚠️ worker 并发跑 body——**body 不得分配 arena**
  （thread.myp 同限制）；Atomic/数组读写安全。
- **修复**：首版 `myp_pool_set_threads` 段错误（139）——`inited`/`nThreads` 等
  标量**值字段**被 `__myp_mem_load_i32(field)` 当**地址**解引用（值为 0 → NULL）。
  正解：值字段直接 `PoolTab.X` 访问；共享/原子字（running/spawnTid/workSeq/
  barSeq/doneCount/totalChunks）arena 分配后经地址访问。
- **验证**：新 `bench/freestanding/rt_pool_test.myp`（2 worker：int/long
  @parallel 1000 累加=499500、10×100 累加、isActive/workerCount/threadCount）；
  **shadow 29/29**；`tests/@test/manual_ch9_myp.myp`（含 @parallel for）用 MYP
  shadow 链接 **3 tests / 11 assertions 全过**；bootstrap 16/16（fixpoint
  `9f5cf25b` 不变，编译器未改）；全量 323/323（oracle + selfhost）。
- **未做**：`@thread`/`@threadpool` 事件路由（event 10）、`myp_pool_create/once/
  init_global` 等 C 内部旧 API、exec worker、任务队列。

### v3.15.45 — runtime myp化 #41：包 F `@thread` 生命周期 + 事件系统（event.myp，futex 无 libc/TLS）

**非破坏性**。`@thread`/`@threadpool` 全生命周期 + 事件系统 MYP 化——shadow C 的
`myp_thread_*` 8 个（create/run_loop/stop/destroy/self/is_current/
associate_instance/post_event）+ `myp_event_*` 7 个（register/push_scope/
pop_scope/fire/process_all/process_one/route_to_instance）+ `myp_timer_*` 3 个
（create/check/cancel_all）+ C 内部 helper 4 个（dispatch/route_to_thread/
timer_next_delay_ms/thread_current），**不依赖 pthread/TLS/libc**：

- **每线程事件队列** = arena 环形缓冲（256）+ 每队列票号锁 + futex seq 字。
  「当前线程」= `gettid`(186) 扫线程表（clone 子线程共享父 TLS → MYP 无 TLS，须
  自建）。线程槽 `@static EvTab`（slot 0=主线程保留；create 返回 slot，编译调用方
  当不透明 ptr，ABI i64/ptr 同寄存器 + ld.lld 不查签名）。
- **spawn 握手**：父设共享 spawnSlot → `myp_thread_spawn`（thread.myp clone 原语）
  → 子读后写 ready[slot]=1 → 父等 ready 才返回（逐线程 spawn 无竞态）。
- **子事件循环**：跑 `startup_fn(startup_arg, NULL)` → `while(running){ process_all +
  qWait(下个 timer 截止) }` → done=1 → syscall 60 退出；destroy 轮询 done（无 join）。
- **跨线程事件路由（BUG-005）**：dispatch 两遍——先路由他线程 handler（同目标只投
  一份，per-slot 去重缓冲；载荷 mmap 深拷贝 → munmap 释放，不用共享 arena——子线程
  不可并发 bump）→ 再跑本线程 handler → `__myp_coro_event_notify`（coro.myp）。
- **handler 表/定时器/实例映射**共用全局 futex 票号锁；timer 载荷用每线程 tPval
  缓冲（fire + process_one 立即处理，同 C 语义）。
- **顺带修复（潜伏 bug）**：alloc.myp `myp_diag_arena_reserved` 从 `Arena.head`
  （最旧 chunk）正向走 next 链只统计首 chunk——多 chunk 时 reserved 低估 → used >
  reserved 误报。evInit 一次 ~550KB 大分配首次暴露（rt_pkgA_test exit 12）。
  改为从 `Arena.cur`（最新）走 next 链（与 C 版一致）。
- **验证**：新 `bench/freestanding/rt_threadpool_test.myp`；**shadow 30/30**；
  `tests/threadpool`（@thread + 4 worker @threadpool "wwww"）与 `tests/sync`
  （4 @thread worker mutex_count=400 + condvar=42）shadow 链接**逐字一致**；
  coro_event/coro_timer/startup/multi_event/mapping_chain/scope_mapping/
  lambda_mapping shadow 逐字一致；manual_ch9（@parallel + @gpu）3/11 通过；
  bootstrap fixpoint `9f5cf25b` 不变（编译器未改）；全量 323/323（oracle+selfhost）。
- **未做**：`myp_event_id_by_name`（纯字符串查表，保留 C）、work 任务队列 / exec
  worker、@thread 子线程共享 arena 并发分配（文档化限制）。

### v3.15.46 — runtime myp化 #41 收尾：包 F 全链路零 C 依赖（不保留 C）

**非破坏性**。#41 补完 `myp_event_id_by_name`（codegen 对 `__myp_timer_create(
<运行时 string>, ...)` 生成调用的动态事件名查表），使包 F（`@thread`/`@threadpool`/
事件系统/定时器/每线程队列）**不再保留任何 C 实现**：

- **event.myp 新增** `myp_event_id_by_name(name, names, ids, count)`——逐字节
  strcmp（MYP string = `'\0'` 结尾 char*；names 是 char* 的 i64 数组，ids 是 i32
  数组），未命中/空名/count=0 → -1。
- **零 C 依赖证明**：`nm` 审计 6 个 shadow 链接二进制（threadpool / sync /
  coro_timer / coro_event / multi_event / mapping_chain），包 F 域（event/thread/
  timer/queue/work/handler/id_by_name）C-only 符号均为 **0**——`--gc-sections` 下
  C 的 myp_work_deque_*/myp_queue_*/myp_timer_wake_target 等内部静态函数无引用被
  剥离，不进入二进制。
- **验证**：新 `bench/freestanding/rt_evname_test.myp`（命中 beta=20/gamma=30、
  未命中 nope=-1、空名 -1、count=0 -1）；**shadow 31/31**；6 个 pkg F 测试二进制
  shadow 链接**逐字一致**；bootstrap fixpoint `9f5cf25b` 不变；全量 323/323
  （oracle + selfhost）。
- **未做**：@thread 子线程共享 arena 并发分配（文档化限制）；C 内部静态
  work/queue/exec worker（无程序引用，已死代码）。

### v3.15.39 — runtime myp化 #36：包 D 协程核心（coro.myp，Phase A 生命周期切片）

**非破坏性**。协程运行时核心 MYP 化——shadow C 的 `__myp_coro_*` 编译器契约 20
个（CORO_DESIGN Phase A 切片：单线程生命周期核心）：

- **coro.myp**：`__myp_coro_create` / `set_entry` / `set_entry_arg` /
  `get_entry_arg` / `yield` / `resume` / `set_result` / `result` / `status` /
  `is_active` / `count` / `current_handle` / `request_cancel` /
  `cancel_requested` / `cancel_clear` / `destroy` / `scheduler` / `trampoline`
  （+ `frame_set`/`frame_clear` stub）。
- **关键机制**：
  - **协程表** `@static CoroT` 并行数组（ctx/retCtx 是**裸 arena 缓冲** `base+slot*8`
    ——ctx_switch 需要槽的**地址**写入保存的 rsp）；槽/代际句柄
    `{generation<<32|slot}`（陈旧句柄失效）+ 空闲槽复用。
  - **栈**：mmap 每协程；trampoline 完成 → 退役列表（不能就地 munmap，正运行其
    上）→ 下次 create/scheduler drain munmap。无栈池（Phase C）。
  - **trampoline 异常边界 = MYP try/catch**（编译器发射 setjmp + myp_exception_push，
    每协程进入 push 自己的 jb；单线程同时只跑一个协程 → LIFO 栈顶恒为当前协程
    边界）。
  - **scheduler**：合作式——每个 ready 协程推进一步（plain `await` = yield 且
    ready 保持）。
  - **entry args**：`@static long[16]` 共享槽（同 C 线程本地共享，只在 entry 读
    一次）。
- **两个编译器/语法发现（顺带规避）**：
  - MYP 顶层 `const int X = N;` 在 selfhost 下把 X 解析成 function 类型 → 常量
    内联字面量。
  - selfhost 模块内**直接调用 `__myp_*` 前缀函数**会丢 `__` 前缀 + 字面量参数
    错型（IR `call @myp_coro_resume(i32 0)`）→ 抽非 `__myp_` 前缀的
    `coroResumeIdx` helper 供内部调用。
- **验证**：`rt_coro_test`（eager 完成 20000 次槽复用 + 代际句柄失效 +
  status/is_active/count + 多参数 entry + await+scheduler 推进）。shadow **26 项**
  （25 exit0 + fail 探针 exit1）、bootstrap 16/16（fixpoint `1e6d4f7` 不变，编译器
  未改）、全量 323/323。
- **未做（Phase B/C，回落 C runtime 或 stub）**：`wait_event`/`sleep`/`wait_fd`/
  `wait_any`/`wait_any_of`（事件层）、`frame_set/clear`（ARC 帧表镜像 stub）、通道/
  未来、exec worker、栈池。

### v3.15.40 — 编译器修复：裸 const 标识符折叠 + selfhost `__myp_*` 去前缀豁免（BUG-050）

**非破坏性**。两个编译期解析缺口（runtime myp化 #36 暴露）双编译器修复：

- **裸 const 标识符折叠**：顶层 `const int CAP = 1024;` 解析为零参 const-decl 函数
  ——此前只有 `CAP()` 显式调用能折叠；裸引用 `CAP`（如 `CAP * 8`）报
  `expected numeric type, got 'function'`（selfhost）/'() -> int'（mypc）。修复：sema
  把 const-decl 零参函数的**裸引用**改判为其返回类型，codegen 发射隐式零参调用
  `call i32 @CAP()`（双编译器一致）。
  - **关键守卫**：`CAP()` 的 callee **不折叠**——`visitCall` 解析 callee 时置
    `in_call_callee_`，否则 `const string A; ... A()` 被误折叠成值类型 →
    `'A' is not callable`。
- **selfhost `__myp_*` 去前缀豁免**：codegen 通用 callee 路径的一刀切
  `__myp_`→去掉前缀 误伤真 `__myp_coro_*`/`__myp_destroy_*` 符号（未定义符号 +
  字面量实参不提升 i64）。修复：去前缀排除 `__myp_coro_`/`__myp_destroy_` 前缀
  （对齐 C++ oracle 显式 `intrinsic_map_` 的别名语义）。
- **验证**：裸 const `/tmp/const_bare5.myp` 双编译 exit=0（IR `call i32 @CAP()` 两处）；
  shadow 25/25（rt_coro_test 直调 `__myp_coro_resume` IR 全名 + i64 字面量）；
  bootstrap 16/16（新 fixpoint `091d2204`，编译器改）；全量 323/323（含
  const_string/eval 回归）。BUGLIST 记 BUG-050。

### v3.15.38 — runtime myp化 #35：包 B 诊断/统计（diag.myp：type_live + fail_alloc）

**非破坏性**。诊断/统计层 MYP 化——`Memory.liveObjectCountByType` 的 per-type
计数 + `Memory.failAlloc*` 确定性分配失败注入：

- **`diag.myp`**：
  - **type_live per-type 计数**：`myp_type_live_inc/dec`（挂 MYP alloc/free：
    `myp_alloc_object` inc + `myp_free_object` dec，按对象头 type_id 索引
    `@static TLive.counts[1024]` 定长数组）+ `myp_live_object_count_by_type`。
    替代 C 的 TLS 每线程数组（MYP 版进程级共享，非 TLS——符合 @static 模型）。
  - **fail_alloc 注入**：`myp_fail_alloc_enable/disable/get` + `myp_fail_alloc_check`
    （挂 `myp_arena_alloc_ex` 顶部注入点）。支持 `Memory.failAllocEnable(N)` 与
    `MYP_FAIL_ALLOC=N` 环境变量（首次检查读一次，经 `myp_env_get` + `myp_str_to_long`）。
    第 N 次分配到达 → stderr 稳定诊断 + `kill(pid,SIGABRT)`（同 C abort()）。
- **关键技巧**：
  - **注入 fire 前必须先禁用**（`FailA.at=0`）：fire 分支构造消息（
    `myp_to_string_u64`/`+` concat）会分配 → `myp_arena_alloc_ex` → 本 check 再
    触发 → seen 已 >= at → 无限递归栈溢出 139。C 版 fprintf 不经 MYP 分配器无此
    问题。
  - **MYP 版 vs C 版计数差异**：C 的 type_live 是 TLS 每线程；MYP 版全进程累计
    （文档化限制）。C 版 `myp_live_object_count_by_type` 因 MYP `myp_alloc_object`
    shadow 了 C 的分配路径而恒 0 → per-type 计数必须 MYP 侧自持（inc/dec 挂点）。
- **验证**：`rt_diag_test`（Node 对象按 type 计数增/减/回零、live 计数 churn 回基线、
  arena 诊断非负、fail_alloc 启→读→禁）；注入探针（`enable(1)`→`#1`+SIGABRT、
  `MYP_FAIL_ALLOC=3` 纯 env→`#3`+SIGABRT、极大 N 不误触发）。shadow **25 项**（24
  exit0 + fail 探针 exit1）、bootstrap 16/16（fixpoint `1e6d4f7` 不变，编译器未改）、
  全量 323/323。
- **未做（依赖包 D）**：`myp_diag_coro_*`/`stack_pool_*`/`retired_*` 读协程运行时
  状态 → 包 D 协程迁移后影。

### v3.15.37 — runtime myp化 #34：线程创建 clone 直建（thread.myp，不保留 C）

**非破坏性**。线程创建 MYP 化——**不保留 C/pthread**：`myp_thread_spawn` 用
clone syscall 直建线程，是**包 F 线程/pool 的地基原语**（用户指令"不保留 C"，
与既有"不要留胶水"策略一致）：

- **`thread.myp`**：`myp_thread_spawn(entryAddr)` —— mmap 1MB 新栈（16 对齐栈顶）
  + clone=56（CLONE_VM|FS|FILES|SIGHAND|THREAD|SYSVSEM）+ 子线程跑共享入口后
  syscall 60 退出本线程。
- **关键机制/技巧**（探针 + 3 线程测试反复验证）：
  - **子线程只依赖 RAX(=0) 判定 + 共享全局**：父栈局部（alloca）在新栈不可见；
    entry 经 `@static Thr` 全局传（`__myp_indirect_void` 间接调用）。
  - **子分支不能声明局部**：子 RSP=新栈顶、无 prologue 帧 → codegen 的 `0x28(%rsp)`
    正偏移在栈顶之上越界（段错误）。修复：子分支只把 entry 经实参（rdi 寄存器）传
    进 helper `myp_thread_child_entry`（普通函数有自己的帧），helper 内 capture 后
    置 `Thr.done_read=1`。
  - **entry 共享槽竞态修复（确定性握手）**：多 spawn 复用同一 `Thr.entry` 会竞态
    （子未读父就覆盖 → 3 线程全跑最后一个 entry）→ helper 先 capture entry（寄存器/
    帧内）再置 `done_read=1`，父自旋等 `done_read` 才返回；子线程仍并发。
  - **@static 类属性初始化器在 --shared 模块下不生效**（`stackSize=1048576` 读出
    为 0 被钳到 65536）→ spawn 内**显式设置** `Thr.stackSize`。
  - **无 CLONE_SETTLS** → 子线程共享父 TLS（MYP @static 全局本就共享，符合当前
    "非 TLS"模型）；C TLS 变量未为子线程建立，子线程勿调依赖 C TLS 的代码。
  - 无 join API（靠共享标志/未来 futex）；arena bump 无锁，子线程勿并发分配。
- **顺带修复 BUG-049（selfhost codegen）**：`&&`/`||` 结果槽原来内联 `alloca i1`
  ——在循环体内每轮执行，LLVM 发射动态 `mov rsp; lea; mov rsp` 不恢复 → 无限循环
  RSP 跌破栈底崩溃。修复：改用 `entryAlloca("i1")`（提升到 entry 块，零初始化，
  `||` 默认 true 显式 store）。C++ oracle 本就正确（PHI 方案）。回归：rt_thread_test
  自旋改用 `||` 直写（原 `int done` 规避移除）。
- **验证**：`rt_thread_test`——3 并发子线程各写独立 @static 共享槽（独立入口函数
  消除编号竞态 + entry 握手确定性），父自旋校验全部完成且值正确，8/8 稳定。shadow
  **24 项**（23 exit0 + fail 探针 exit1）、bootstrap 16/16（新 fixpoint `1e6d4f7`，
  编译器改）、全量 323/323。

### v3.15.36 — runtime myp化 #33：包 C 异常机制（exception.myp）

**非破坏性**。异常运行时（try/catch/throw 的 jmp_buf 处理栈 + 错误消息 + 类型化
载体 + release_slot）MYP 化——**协程 Phase A 的 setjmp 异常边界前置之一**：

- **`exception.myp`**：`myp_throw`/`myp_throw_object`（记录消息/对象+类型）、
  `myp_get_error`（catch (string e) 取消息）、`myp_error_setup/is_active/clear`、
  `myp_exception_push/pop/get_jmpbuf/get_type/get_object`（处理栈）、`myp_try_escape`
  （IR 逃逸屏障）、`myp_release_slot`（异常 longjmp 路径释放 ARC 槽物理内存）、
  `__myp_longjmp`（ASan 感知包装，MYP 版跳过 ASan hook 直接调 libc longjmp）、
  `myp_diag_get/set_strict`（strict 标志 API）。
- **关键语义/技巧**：
  - **`setjmp` 用 libc**（生成代码发射 `call i32 @setjmp(ptr)` + ReturnsTwice，非
    myp_* 符号不影）；`__myp_longjmp` 经 ffi 调 libc `longjmp`（i64/i32 与
    ptr/i32 ABI 兼容，链接按名解析）。
  - 处理栈 @static Exc.handlerBufs（arena 64×8）+ depth；`myp_exception_get_jmpbuf`
    无 handler 时 stderr 打印未捕获 + exit(134)（abort 语义）。
  - 错误消息拷贝到 arena 缓冲（C 用静态 buf；MYP 拷贝保证抛出的字符串释放后仍可
    读）。
  - `myp_release_slot`：读 ARC 槽首 8 字节（类 ptr / 接口·函数胖指针 data 都是
    槽首）→ `myp_release(__myp_addr_to_str(obj))`（`__myp_addr_to_str` 是
    inttoptr 地址→ptr 重解释，非拷贝）。
- **验证**：`rt_exception_test`——字符串异常 catch 绑定消息 / 正常 try / 嵌套
  （内层捕获+外层捕获）/ 类型化（throw new MyErr + catch (MyErr e)）/ finally
  正常 + 异常路径。shadow **22 项**（21 exit0 + fail 探针 exit1）、bootstrap
  16/16（fixpoint `c998455d` 不变，编译器未改）、全量 323/323。

### v3.15.35 — runtime myp化 #32：console+test 框架包（print + @test 捕获/断言/报告）

**非破坏性**。包 A 收官：把 print 输出路径与 @test 框架（捕获/断言/报告）一起 MYP
化（之前因 C 内部 `myp_out_write`→`myp_capture_write` static 不可影、只影 print 会
与 C 捕获缓冲不一致而推迟）。新增 2 个模块：

- **`output.myp`**（print + 捕获）：`myp_print/println/print_int/print_long/
  print_bool/flush` + `myp_test_capture_start/stop/get/contains/eq`。统一出口
  `outWrite`：捕获态（`@static Cap.on`）追加 MYP 捕获缓冲（arena 增长），否则
  `write(fd=1)` 直接写 stdout（镜像 C myp_out_write → capture / fputs）。
  stdout 无缓冲（直接 syscall）→ `myp_flush` 空操作。格式化复用 num.myp
  （`myp_to_string_i32/i64`）。**`myp_print_float` 已在 float.myp**（经 `__myp_print`
  → 本模块 `myp_print`，自动接捕获，无需改）。
- **`test.myp`**（断言 + 报告）：`myp_assert/assert_msg/assert_eq/assert_neq/
  assert_long_eq/assert_long_neq/assert_str_eq/assert_str_neq/assert_float_neq/
  assert_null/assert_not_null/assert_abort` + `myp_test_set_msg/fail_msg/report/
  summary`。语义镜像 C：**@test 按退出码验证**（run_tests.sh 查 exit）、断言失败走
  **stderr**（fd=2，不经 stdout 捕获）、待消费消息（`myp_test_set_msg`）**仅失败
  消费并清空**（通过时遗留，同 C 行为）、`myp_test_report/summary` 直接 stdout
  （不经捕获，同 C printf）、`myp_test_summary` 返回 fail>0 ? 1 : 0（@test main
  退出码）。`myp_assert_null/not_null` 用 long 参数（指针地址，ABI 兼容）。
- **验证**：`rt_pkgA2_test`（pass 路径：print+捕获拼接/eq/contains/断言通过/
  summary=0）+ `rt_pkgA_fail_test`（fail 路径：故意失败断言 → stderr `1 != 2`/
  `custom msg`/空 detail + summary=1）。build.sh 加**按测试期望退出码**逻辑
  （rt_pkgA_fail_test 期望 exit 1）。shadow **21 项**（20 exit0 + fail 探针 exit1）、
  bootstrap 16/16（fixpoint `c998455d` 不变，编译器未改）、全量 323/323。

### v3.15.34 — runtime myp化 #31：包 A 残留薄层（str_parse_int_opt + diag_arena）

**非破坏性**。包 A 第一批（无前置快赢），新增 3 个 MYP shadow + 审计修正：

- **`myp_str_parse_int_opt`**（num.myp）：编译器内建 `parseIntOpt(s)` 发射
  `call i32 @myp_str_parse_int_opt(ptr, ptr)`——MYP 版用 **long 参数**（指针地址
  承载）**ABI 兼容 shadow**（ptr 与 i64 同为 64 位、rdi/rsi 传参，链接按名解析），
  **无需改编译器**。base-0 语义（空白/符号/0x 十六进制/前导 0 八进制/十进制）+
  有效性 `*ok`（区分合法 0 与失败；前导 '0' 八进制时 '0' 本身算数字 → "0"/"077"
  判有效）。
- **`myp_diag_arena_reserved/used`**（alloc.myp）：诊断读 arena。**发现**：MYP
  alloc.myp 只维护 `Arena.cur_used`（当前 chunk）+ 新增 **`Arena.total_used` 累计
  bump 计数器**（不写每 chunk 头 `used@16`，C 版逐 chunk 读字段不适用）→
  `myp_diag_arena_reserved` 遍历 chunk 链（next@0→cap@8）累加，`used` 直接返回
  total_used。bump 永不归还，total_used 即当前已用。
- **验证**：`rt_pkgA_test`——str_parse_int_opt 11 项（"123"/"0"合法零/"-42"/"+7"/
  "0x10"/"077"八进制/"abc"失败/""失败/"  42"前导空白/"12abc"截断）；diag_arena
  （分配后 used 增、reserved≥used）。shadow **19/19**、bootstrap 16/16（fixpoint
  `c998455d` 不变，编译器未改）、全量 323/323。
- **审计修正（MIGRATION_STATUS.md）**：正则漏 `ubyte[]` 等返回类型 → 实为 C 490
  个、已影 **137**、未影 353；`myp_str_to_bytes` 早已在 bytes.myp 迁移；`myp_str_
  cat/cpy/fmt/len` **无 MYP 调用方**（C 内部/死代码，无需影）；**print 层与 @test
  捕获（myp_capture_*）耦合**（C 内部 myp_capture_write 被 printf/assert 调用，
  迁移须连 test 框架一起，归入"console+test 框架"包，文档化推迟）。

### v3.15.33 — runtime myp化 #30：通用间接调用内建 `__myp_indirect_*`（GPU 迁移地基）

**非破坏性**。自举编译器新增**通用间接调用内建**（自举独有，同 `__myp_asm`/
`__myp_call_ptr`/`__myp_rtable_addr` 模式）——运行时 `dlopen`+`dlsym` 解析函数
地址后，按**任意签名**间接调用 C API（CUDA 驱动 / LLVM C API 绑定地基）：

- **sema/codegen**：`__myp_indirect_i32(addr, ...args)` → int / `_i64` → long /
  `_double` → double / `_void` → void。addr 是 i64 函数指针地址（dlsym /
  `__myp_fn_addr` 所得）；实参按调用点 MYP 实参的 LLVM 类型原样传（long 承载
  指针）。发射 `inttoptr i64 addr to ptr` + `call RET %fn(<类型化实参>)`。
- **用途**：GPU 层（`runtime_gpu.c` 65 个 `myp_gpu_*`）迁移的关键使能——CUDA
  是运行时 dlopen（`libcuda.so` 可能不存在、MYP_GPU 未设时 CPU 回退），不能
  静态 ffi 链 cu* 符号；44 个 `cu*` 函数指针签名各异（cuLaunchKernel 11 参等），
  现可经 `__myp_indirect_*` 调用。也为将来绑 LLVM C API 等复用。
- **验证**：`rt_indirect_test` 覆盖 i32/i64/double/void 四种返回 + 0/1/2/3 实参
  + 混合类型 ABI——i32(`myp_strlen` 1 个 ptr 实参)、i64(`myp_now_ms` 0 实参)、
  double(`myp_math_pow` 2 个 double / `myp_math_sqrt` 1 个)、void(用户 `myNop`)、
  用户函数 `myAdd`(2 个 i32)、`myMix`(i64+i32+double 混合) 全对。shadow **18/18**、
  bootstrap 16/16（新 fixpoint `c998455d`）、全量 323/323。IR 确认：`inttoptr
  i64 → to ptr` + `call <RET> %fn(<类型>)` 正确发射。
- **注意**：`__myp_fn_addr("name")` 只对**已声明/已定义**符号有效（preamble 里的
  runtime 函数或模块内定义；`myp_mfence` 等 runtime_myp 模块函数未在 preamble
  声明会报 "use of undefined value"）——CUDA 地址来自 dlsym（运行时 long），不
  受影响。

### v3.15.32 — runtime myp化 #29：math 层补齐（pow/atan2/sinh/cosh/tanh）

**非破坏性**。把 #28 遗留的 5 个 math 函数全部 MYP 化，`myp_math_*` 达 **19/19**
（零数值手写实现——**LLVM 21 原生支持全部 5 个 intrinsic**，实测确认）。

- **关键发现**：`llvm.pow.f64` / `llvm.atan2.f64` / `llvm.sinh.f64` /
  `llvm.cosh.f64` / `llvm.tanh.f64` 在 LLVM 21 全部存在（降级到与 C 相同的
  libm → 位级一致），且 LLVM **自动声明** intrinsic（无需 ir_emit declare）。
  sinh/cosh/tanh 因此也无需 #28 设想的 exp 组合数值实现。
- **自举 codegen 变更**：`genPolyMathIntrinsic` 扩展处理**二元** `__myp_math_pow`/
  `__myp_math_atan2`（两实参统一提 double → `llvm.pow.f64`/`llvm.atan2.f64`）；
  调度点移除 pow/atan2 排除（abs_int 仍走通用 FFI）。**顺带根治潜伏自递归**——
  原二元 `__myp_math_pow` 落通用路径发射 `call @myp_math_pow`，MYP shadow 版会
  自递归；现直发 llvm → `myp_math_pow` 内部 `__myp_math_pow` 安全。
- **`math.myp`** 19/19：新增 `myp_math_pow/atan2/sinh/cosh/tanh` 一行式
  （`return __myp_math_*(...)` → LLVM intrinsic，不递归）。oracle（mypc）不改：
  对 `__myp_math_*` 仍发射 `call @myp_math_*` → 链接到 MYP 定义 → 内部走 llvm.*。
- **验证**：`rt_math_test` 扩展 18 项断言（22-39）——pow 精确值（2^10=1024、
  2^-1=0.5、0^0=1）+ sqrt(2) 容差；atan2 四象限（0/π/2/π/4/-3π/4）；sinh/cosh
  已知值 + **溢出边界**（sinh/cosh(1000) > 1e300）；tanh 已知值 + **大参 ±1**
  （tanh(±20)=±1）。shadow **17/17**、bootstrap 16/16（新 fixpoint `e7075fb8`）、
  全量 323/323。反汇编：math.o 定义 19 个 `myp_math_*` T 符号；math.myp 内部
  仅 5 个 llvm intrinsic、**0 次 `call @myp_math_*`（无自递归）**。

### v3.15.31 — runtime myp化 #28：fs/env/args/term/math 薄层迁移

**非破坏性**。把五组"薄层" C runtime 符号 MYP 化（shadow 机制），新增 5 个
`runtime_myp` 模块 + 5 个验证程序：

- **`term.myp`**（2/2）：`myp_term_width/height`——`ioctl=16`(TIOCGWINSZ=0x5413)
  写 8B winsize 缓冲（ws_row@0/ws_col@2 u16），失败回退 80x24（同 C）。
- **`fs.myp`**（12/12）：`exists/is_dir/is_file/file_size/modified_time` 用
  `newfstatat=262`（AT_FDCWD=-100；内核 struct stat 144B：st_mode@24、
  st_size@48、st_mtime@88；S_IFDIR=0x4000/S_IFREG=0x8000）；`list_count/
  list_get` 用 `getdents64=217`（linux_dirent64：d_reclen@16 u16、d_name@19，
  跳过 . / ..）；`mkdir_p`（mkdir=83，0755=493，EEXIST=17 忽略，逐前缀）；
  `remove_recursive`（lstat=newfstatat+AT_SYMLINK_NOFOLLOW=0x100，getdents64
  递归 + rmdir=84/unlink=87，ENOENT 视为成功）；`dirname/basename/join` 纯字节
  操作。
- **`args.myp`**（2/2）：`myp_args_count/get` **惰性**读 `/proc/self/cmdline`
  （NUL 分隔，argc=NUL 数），@static 8B argc+8192B 原始缓冲缓存——免 C 构造器
  全局依赖。越界返回空串。
- **`env.myp`**（get 迁移；set/unset 保留 C）：**关键发现**——`/proc/self/
  environ` 只反映进程**启动时**的环境内存区（内核 mm->env_start），libc setenv
  使 environ 缓冲 realloc 移动后**看不到**新变量（实测：export 后 grep 不到）。
  正解：MYP `myp_env_get` 直接**遍历 libc 的 `environ` 弱符号**（char**，
  `environ@@GLIBC_2.2.5`）：`__myp_fn_addr("environ")` 发射 `ptrtoint ptr
  @environ to i64` 取址（复用 #25 内建），**自举 ir_emit preamble 加一行
  `@environ = external global ptr`**（声明非定义，未引用无害）→ 与 C getenv
  完全一致（含 setenv/unsetenv 实时修改）。`myp_env_set/unset` 无 syscall 等价
  物（libc 进程级 environ 操作）→ 保留 C，文档化。
- **`math.myp`**（14/19）：一元实数（sqrt/abs/floor/ceil/trunc/sin/cos/tan/
  asin/acos/atan/exp/log）+ `abs_int`——经 `__myp_math_*` 内建 → 自举 codegen
  发 **LLVM 标量 intrinsic**（llvm.sqrt.f64 等，与 libm 位级一致、不递归）。
  **未影（保留 C，另立里程碑）**：`myp_math_pow/atan2`（二元无对应一元 llvm
  直发路径，自举 `__myp_math_pow` 发射 `call @myp_math_pow` 会自递归）、
  `myp_math_sinh/cosh/tanh`（LLVM 无对应 intrinsic，需 exp 组合 + 大参数特殊
  处理 + 精度对拍）。
- **验证**：shadow **17/17**（新增 rt_term/rt_fs/rt_args/rt_env/rt_math；args 特例
  以 `alpha beta gamma` 运行验 argc=4；env 用 C setenv→MYP get 一致性验同 environ
  源）、bootstrap 16/16（新 fixpoint `7f88f563…`，ir_emit 改）、全量 323/323。
  反汇编确认：fs.o 定义 12 个 `myp_fs_*` T 符号；env.o `myp_env_get` 有
  `R_X86_64_REX_GOTPCRELX environ` 重定位（真走 libc environ）。
- **工程修复**：`build.sh` 测试循环 `set -e` 会在二进制返回非 0 时**静默退出**
  （`exit=$code` 打印前被 errexit 杀死）→ 用 `set +e` 包运行取退出码再 `set -e`。

### v3.15.30 — runtime myp化 #27：精选汇编原语标准库 `runtime_myp/asm.myp`

**非破坏性**。在 #26 通用 `__myp_asm` 内建之上，封装**精选 x86-64 汇编原语**为
类型安全的标准库，供运行时基础设施使用（计时/自旋锁/内存屏障），不暴露裸 asm：

- 新建 `runtime_myp/asm.myp`：
  - `myp_rdtsc()` → long——x86 时间戳计数器（纳秒级、恒定频率）。**缓冲法规避
    `$` 立即数**：`rdtsc; movl %eax, (%rdi); movl %edx, 4(%rdi)` 写 8B arena 缓冲
    （`@static TscBuf.addr` 惰性分配），MYP 用 `__myp_mem_load_i64` 读回完整 64 位。
  - `myp_pause()` / `myp_mfence()` / `myp_lfence()` / `myp_sfence()`——自旋锁提示
    与内存屏障，统一 `~{memory}` clobber。
- 新建 `bench/freestanding/rt_asm_test.myp`：rdtsc 非零 + 单调（100 采样）、
  pause/三种 fence 可执行；接入 `runtime_myp/build.sh` 测试循环。
- **实测踩坑记录（重要）**：
  - LLVM 内联 asm 把 `$N` 当**操作数引用**——AT&T `$32` 立即数报 `Invalid $
    operand number`，与 MYP 字符串 `$` 插值双重重叠。→ asm 一律不写 `$` 立即数，
    需要立即数/返回值走寄存器或缓冲。
  - `\$` 转义**无效**：lexer 虽能生成 `\`，但 `parser.myp expandDollarInterp`
    会对字符串值重新扫描 `$`（char 36）→ 仍被当插值前缀。**三引号原始串
    `"""..."""`（raw=1）才是字面 `$` 的正确机制**（跳过展开）。
  - 曾临时给 lexer 加 `\$` 转义（2 处）——**已回滚**，净零变更（git diff 为空）。
- 验证：shadow **12/12**（新增 rt_asm_test）、bootstrap 16/16（fixpoint
  `dfe0f3b6…`，与 #26 相同，确认 lexer 回滚干净）、全量 323/323。

### mypview 框架变更 → 见 `mypview/CHANGELOG.md`

mypview 框架（控件/布局/UIX/AppRunner/示例/测试）的变更记录迁移到
`mypview/CHANGELOG.md`（v3.12.58 AppRunner 框架化 / v3.12.59 JsonEditor /
v3.12.60 UixDesigner 所见即所得设计器等）。本文件继续记录编译器/运行时/stdlib
变更；mypview 专用 stdlib 扩展（如 json bridge 编辑 API）在主 changelog 与
mypview changelog 双向引用。

### v3.15.29 — runtime myp化 #26：通用内联汇编内建 + 标准库封装 myp_ctx_switch

**非破坏性**。把 #25 的硬编码 `__myp_ctx_switch` builtin 重构为「**通用内联汇编内建 +
标准库抽象**」（用户建议）：

- **通用内建**（自举 codegen/sema）：`__myp_asm(asmStr, consStr, ...args)` → void /
  `__myp_asm_r(...)` → long——asm/constraints 为编译期常量串（`constStrVal` 递归求
  字面量/拼接），后续实参按各自 LLVM 类型作 asm 操作数，发射 `call asm sideeffect`。
  具体汇编用途**不再在 codegen 写死**。`__myp_fn_addr` 保留。
- **标准库封装**：新建 `runtime_myp/coro.myp`，`myp_ctx_switch(save, load)` 用通用
  `__myp_asm` 实现（配方同 §8：`;` 分隔 / `{rdi}{rsi}` / caller-saved clobber /
  `1f` 数字标签落空 epilogue / 不发 ret）。
- 删除硬编码 `__myp_ctx_switch` builtin（sema/codegen）；`rt_ctx_probe` 改调标准库
  `myp_ctx_switch`（FFI）。
- 验证：shadow **11/11**（rt_ctx_probe 走标准库）、bootstrap 16/16（新 fixpoint
  `dfe0f3b6…`）、全量 323/323。`CORO_DESIGN §8.1` 更新架构说明。

### v3.15.28 — runtime myp化 #25：`__myp_ctx_switch` 内联汇编上下文切换探针通过

**非破坏性**。协程核心可行性验证：自举编译器新增两个内建——`__myp_fn_addr("name")`
（`ptrtoint ptr @name to i64` 取函数地址，设协程入口帧用）与 `__myp_ctx_switch(save,
load)`（内联汇编上下文切换，镜像 `coro_ctx.S myp_ctx_switch`，零 syscall）：

- **内联汇编配方（实测踩坑记录）**：指令分隔用 `;`（`\n` 不被 LLVM 内联 asm 解析、
  `%=` 标签不被该版本处理）；寄存器用 `%reg`（`%0` 报 invalid register name）；
  操作数用 `{rdi}/{rsi}` 硬编码约束（不能 `$0/$1`——MYP 字符串里 `$` 是插值前缀）；
  恢复地址 `leaq 1f(%rip), %rax` + `jmpq *%rax` + 数字标签 `1:` 落在 asm 块末尾 →
  resume 落空到 LLVM epilogue 正常返回（**不发 ret**）；**clobber 必须列全部
  caller-saved**（上下文切换只保存 callee-saved，worker 破坏 caller-saved 如 run()
  的 rdx 活值 → 不列会段错误）；ctx 是 8B 槽（存 rsp），入口地址≠rsp（实测把入口当
  rsp 会跳进代码段崩溃）。
- **探针通过**（`bench/freestanding/rt_ctx_probe.myp`）：main→worker（4KB arena 栈）→
  main 双切换 + entryHit 标记，exit=0。**协程 yield/resume/调度可全 MYP 化，消除
  coro_ctx.S 依赖**；剩余前置 = 异常边界 setjmp + 线程创建（OS 边界）。
- 验证：shadow **11/11**（新增 rt_ctx_probe）、bootstrap 16/16（新 fixpoint
  `5f8fe4db…`）、全量 323/323。`CORO_DESIGN.md §8` 记录探针结论与内联汇编配方。

### v3.15.27 — runtime myp化 #24：类对象 release 分发全 MYP 化（无 C 胶水）

**非破坏性**。**策略变更（用户定）**：mypc 不再冻结、**不留 C 胶水**——运行时↔程序生成
代码的硬边界也要全 MYP 化。`myp_release_class_obj_ex`（类对象 rc→0 后查程序生成的
`__myp_release_table` 分发 destroy stub）不再委托 C helper：

- **自举编译器新增两个内建**（runtime_myp 模块由自举编译；mypc 不用改——它已用
  `ExternalLinkage` 发射同名 `@__myp_release_table`）：
  - `__myp_rtable_addr()` → long：发射 `ptrtoint ptr @__myp_release_table to i64`。
    运行时模块自身也定义同名表，但**程序 .o 链接在前 + `--allow-multiple-definition`
    → 程序表胜出**，引用解析到程序真实表（含 destroy stub 地址）。
  - `__myp_call_ptr(long addr, string obj)` → void：发射 `inttoptr` + 间接
    `call void %fn(ptr %obj)`（LLVM 21 opaque ptr 下合法）。
- **`runtime_myp/alloc.myp` 实现 MYP `myp_release_class_obj_ex`**：weak 通知 →
  `__myp_rtable_addr` 取表 → `__myp_mem_load_i64(table + tid*8)` 读 stub →
  `__myp_call_ptr` 间接调 destroy stub（级联释放引用字段 + `myp_free_object`）；
  无 stub（tid<=0）→ 直接 `myp_free_object`。删除原 `ffi` 委托。
- 验证：新增 `bench/freestanding/rt_cls_release_test.myp`（Outer 强持 Inner 字段，
  释放 Outer → destroy stub 级联释放 Inner，Live 对象计数回基线；200 轮循环无泄漏）。
  shadow **10/10**、bootstrap 16/16（新 fixpoint `8eca1f53…`，编译器源码变更）、
  全量 323/323。反汇编确认 MYP 分发（`lea @__myp_release_table` + `call *reg` 间接
  调用 destroy stub）。

### v3.15.26 — runtime myp化 #23：时间层 myp_now_ms / myp_now_realtime_ms / myp_sleep_ms

**非破坏性**。新增 `runtime_myp/time.myp` shadow C runtime 时间函数（Time.nowMs /
Process.sleep 及 C 内部通道/协程/定时器广泛依赖），纯 raw syscall：

- `myp_now_ms`/`myp_now_realtime_ms`：`clock_gettime`（syscall 228，CLOCK_MONOTONIC=1
  / CLOCK_REALTIME=0）写 16B timespec 缓冲 → `sec*1000 + nsec/1000000`。
- `myp_sleep_ms`：`nanosleep`（syscall 35，req/rem 双缓冲）；EINTR（返回<0）时内核
  把剩余时间写进 rem → 拷回 req 重试（保持近似 ms 语义，同 C 版）。
- 缓冲 arena 一次性分配 + `@static` 缓存地址（io.myp/region.myp 同模式；⚠️ 非 TLS，
  多线程并发共享缓冲有纳秒级竞态，当前测试全单线程，文档化限制）。
- 验证：新增 `bench/freestanding/rt_time_test.myp`（realtime epoch 量级 / 单调 100
  采样 / sleep 50ms+1050ms 实际睡眠，跨秒验 nanosleep rem 路径）；shadow **9/9**、
  bootstrap 16/16（fixpoint 不变）、全量 323/323。反汇编确认 MYP 版（含
  `timeBufAddr` 符号 + inline syscall）。
- 附带清理：删除 str.myp 过时 TODO（`myp_str_split_get` 早已 MYP 化，上版误判）。

### v3.15.25 — runtime myp化 #22：字符串拼接层 myp_str_append（arena 原地扩展）

**非破坏性**。`runtime_myp/str.myp` 新增 `myp_str_append`（`s = s + x` 快路径），
`runtime_myp/alloc.myp` 增加 `last_base`/`last_aligned` 追踪 + `myp_arena_alloc_ex`
（grow 变体）+ `myp_alloc_str_grow`：

- **MYP 版**：`s = s + x`（owned 局部双字符串）经 codegen 发射 `@myp_str_append` +
  普通 store（消耗 s）。s 唯一（rc==1）且是 arena 最后一次分配 → **bump 原地扩展**
  （O(1) 均摊，写 x + 更新 len 字段 + rc 归零由 return-retain 补 1）；否则回退手动
  拷贝（`myp_alloc_str_grow` 2x 增长头）+ release(s)。
- **消除 C 版 latent bug**：C `myp_str_append` 对 rc==1 字符串读假头
  `node=data-sizeof(node)` 再 `realloc` —— shadow 下内存是 arena → realloc 假头/
  崩溃（此前 shadow 用例未触发因快路径需 owned 局部）。
- **ARC 约定（实测确认）**：`return <owned 局部>` = retain + releaseArcSlots →
  净 rc 不变；`return <参数>` = 仅 retain → 返回前须置 rc=0 让 retain 补成 1。
- **修复 shadow 内存爆炸**：回退若用普通 `myp_alloc`（chunk 恰好够），字符串每涨
  16B 就回退重拷整串 → 500k 累积实测 **7.5GB RSS**。`myp_alloc_str_grow` 给新
  chunk 2x 头 → 回退点几何递增（64K/128K/256K/512K）→ **O(n) 摊销**：500k 累积
  wall=0.00s、RSS=3MB、原地命中 499996/500000。
- 验证：`bench/freestanding/rt_str_test.myp` 新增 `RtAppendProbe`（2000 小累积 +
  自拼接 + 40000 跨 chunk 大串 + 空串起始）；shadow 8/8、bootstrap 16/16
  （fixpoint md5 不变）、全量 323/323。

### v3.15.24 — 浮点精确路径：%.f 大数 / %g·%e 高精度（uint32 大整数精确十进制展开）

**非破坏性**。`runtime_myp/float.myp` 新增**精确十进制展开**（uint32 小端词大整数，
≤40 词覆盖 subnormal 1074 位），解除 #19 的精度上限：

- **%.f |v|>=2^53**：double 反复 /10 会丢低位 → 改用大整数 `m<<e` 反复 /10 出
  精确整数位（2^63、1e100 等逐位正确）。
- **%.f prec>15**：精确小数展开 `rem/2^r`（分母仅 2 因子 → 十进制有限），每步
  **先 rem*=10 再取高 r 位**（digit=floor(rem*10/2^r)）——%.60f 的 0.1 给出完整
  55 位精确展开。
- **%g/%e P>15**：精确 P 位有效数字 + round-half-even（%g 仍去尾零），支持到
  prec=60。
- **发现 C runtime 缺陷**：`myp_fmt_double_f` 用 `char buf[160]` + snprintf →
  %.f 大数（如 1e308，309 位）被**截断到 159 位**；MYP 精确路径给出**完整精确值**
  （与 Python `format(1e308,'.0f')` 逐位一致）——**MYP 更正确**。
- 重构：`fmtSciBody`/`fmtFixedG` 尾部抽为 `assembleSci`/`assembleFixed`（快/精两
  路径共用），dispatch：%.f 在 `expf>=1076`（|v|≥2^53）或 prec>15、%g 在 prec>15、
  %e 在 prec>14 走精确路径。
- 验证：`rt_float_prec_test.myp` 24 断言（大整数 9 + 高精度小数 5 + %g P>15 7 +
  %e 4）C/shadow 双跑全过；次正规数 5e-324（r=1074）对拍一致；shadow 8/8、
  bootstrap 16/16、全量 323/323。

### v3.15.23 — 浮点层性能基准（MYP shadow vs libc，~1.1-1.26x）

**非破坏性**。新增 `bench/freestanding/rt_float_bench.myp` + `run_float_bench.sh`
复现脚本：`myp_atof`（strtod 解析）+ `myp_to_string_double`/`myp_fmt_double_f/e/g`
（%g/%e/%f 格式化）各 160 万次，C runtime（libc）vs MYP shadow（float.myp）对比，
`psum`/`fsum` 作位一致校验。实测（rounds=200000，8 操作/轮）：

| 操作 | C runtime (libc) | MYP shadow (float.myp) | 倍数 |
|------|------------------|------------------------|------|
| atof 解析 | ~30 ns/op | ~33-35 ns/op | **~1.1-1.17x** |
| fmt 格式化 | ~94-95 ns/op | ~117-118 ns/op | **~1.23-1.26x** |

- **位精确等价**：`psum=2e+105`、`fsum=11800000` 两模式完全一致。
- 反汇编确认 shadow 生效：shadow 二进制含 `myp_pow10`/`myp_math_pow`
  （MYP float.myp 特有符号），C 二进制无（`myp_atof` 直接 libc `atof`）。
- 结论：纯 MYP 重实现 strtod + %g 距 glibc 高度优化版仅 ~10-26%（`__myp_math_pow`
  小整数指数有 glibc 快路径 + LLVM 原生代码）；checksum 一致性同时佐证 #19 位精确。

### v3.15.22 — runtime myp化 #19：浮点层（strtod 解析 + %g/%e/%f 格式化）

**非破坏性**。新增 `runtime_myp/float.myp` shadow C runtime 的浮点解析与格式化
（去 libc `strtod`/`snprintf %g` 依赖，纯 MYP + `__myp_math_pow/floor/log` 内建）：

- **解析**：`myp_atof` / `myp_str_to_double` / `myp_str_to_float`（strtod 语义：
  跳过空白/可选符号/小数点/指数 `e/E`、`0x` 十六进制浮点（`p` 二进制指数）、
  `inf/infinity/nan`；无有效数字回 0；溢出→±inf、下溢→0）。
- **格式化**：`myp_to_string_double`/`myp_to_string_float`（`%g` 默认精度 6）、
  `myp_fmt_double_f/e/g`（`%.*f/e/g`）、`myp_print_float`（Console.writeFloat
  路径）。算法：十进制指数 `X=floor(log10|v|)`（log 粗估 + pow10 比较校正）；
  `%g` 在 X<-4 或 X>=prec 走 %e 风格否则 %f 风格；有效数字 `|v|/10^(X-P+1)`
  缩放到 `[10^(P-1),10^P)` 后 **round-half-even** 取整（P<=15 精确），%g 去尾零。
  `%.*f` 整数部分反复 /10 + 小数部分反复 *10 并按第 prec+1 位 half-even 进位。
- **验证**：`rt_float_test.myp` 111 断言（解析 20 + %g 17 + %f 12 + %e 8 + %g 11 +
  myp_print_float 3），期望值 = glibc strtod/snprintf 输出——C runtime 与 MYP
  shadow 双跑全过（字节对拍一致）。shadow 全套 7/7、bootstrap 16/16、全量 323/323。
- 已知边界：%g/%e 有效数字 P 截到 15（超出近似，实际极少用）；`%.*f` 对
  |v|>=2^53 的整数部分取位为近似；幂缩放用 libm `pow`（正确舍入 <1ulp）。

### v3.15.21 — 字符串头 len 字段：myp_strlen O(1)（根治 __strlen_evex 热点）

**非破坏性**。字符串 ABI 布局升级：计数字符串头从 8B `{rc, type_id}` 扩为
**12B `{len, rc, type_id}`**（`len` 在 data-12，不含 NUL；`rc/type_id` 仍在
data-8/-4 与类对象同偏移 → `myp_retain`/`myp_release`/`__myp_obj_type_id` 分发
零改动，仅字符串分配/释放/原地 realloc 路径用 `MYP_STR_HEADER_SIZE=12`）。

- **`myp_strlen` → O(1)**（C runtime + MYP shadow `str.myp` 都改读 len 字段）。
  旧版 C 走 libc strlen（perf 自举 67% 热点）、MYP 版逐字节扫描，均每次 O(n)。
  实证：200KB 串 × 200 万次 `Str.len` = **0.012s**（旧 O(n) 需扫 ~200GB）。
- **写 len 点全覆盖**：C `myp_alloc_str`、MYP shadow `myp_alloc`（12B 头）、两
  编译器字面量发射（`{i32 len, i32 rc, i32 type_id, [N x i8]}`，GEP 到元素 3）、
  C `myp_str_append` 原地 realloc 同步更新 len。
- **顺带修复 len 字段暴露的 3 处 bridge 潜伏 bug**（预分配 cap 缓冲直接返回 →
  len 字段=cap 而非实际长度；旧 strlen 扫描掩盖）：`myp_net_recv`（超时/EOF 空
  串 len=0、成功按实收 n 构建）、`myp_uds_recv`（同）、`myp_process_output`（按
  实际输出长度构建）。`async_socket` 超时路径 `timeout_len=0` 恢复正确（此前实测
  len=10 的 NUL 串）。
- **构建顺序（bootstrap 一致性）**：先改自举 codegen 字面量 → 用旧编译器双重建
  myp_self2（v1 新发射/旧内部、v2 新发射/新内部）→ 再切 C runtime + C++ 编译器
  → 重建 libmyp_rt.a/mypc → 用 v2 构建 v3（新内部 + 新 runtime）为最终 myp_self2。
- 验证：bootstrap 16/16（新定点 md5 `1def2c4e…`）、全量 323/323、shadow 6/6
  （str/num/alloc/region/weak/io）、自举自编译 16.2s 持平。

### v3.15.20 — runtime myp化 #17：异步文件读接管（myp_coro_file_read_line/all 同步读）

**非破坏性**。`runtime_myp/io.myp` 接管 C 的协程文件读入口
（`File.readLineAsync/readAllAsync` → `myp_coro_file_read_line/all`）：

- **修复 readAll shadow 破坏**：C 版 `myp_coro_file_read_all` 经
  `myp_io_lock_handle(io_handle)` 读 C 的 `FILE*` 表——MYP 表接管后该表恒空，
  shadow 下 readAll 返回空串（自举编译器 `tools/selfhost/src/link.myp`、
  `main.myp` 都依赖 readAll）。
- `myp_io_read_line` 重构为 handle 参数化 `ioReadLineHandle(handle)`（pending
  字节优先 + 逐字节到 \n/EOF + arena 4096 scratch），当前句柄读变包装。
- 新增 `ioReadAllHandle(handle)`：lseek(SEEK_CUR/SEEK_END) 取大小后回位分块读，
  lseek 失败（流式）自动退化为逐字节；字节级保真（含 \n，不剥）。
- **已知限制**：MYP 版为**同步**读（数据正确）；C 版协程 parking + exec
  worker 的非阻塞 await 语义未 MYP 化（依赖 C FILE* 表，shadow 后不适用）。
  323 套件的 `async_file` 测试走 C runtime 不受影响。
- 验证：bootstrap 16/16，全量 323/323，`rt_io_test` 新增 readAll 全文件读用例
  全部 exit=0。

### v3.15.19 — runtime myp化 #16：文件 I/O 层（myp_io_* raw syscall 实现）

**非破坏性**。`runtime_myp/io.myp` shadow C runtime 的整个文件 I/O 层
（File 类经 `__myp_io_*` 内建 → `myp_io_*`），**纯 raw syscall、无 libc stdio**：

- **open=2 / read=0 / write=1 / lseek=8 / close=3**；mode 串 → open flags
  （r/w/a + `+` 加 RDWR，新建权限 0644）；fd 表 64 槽（每槽 16B `[fd][pending]`，
  fd=-1 空闲）+ **add-原子自旋锁**（复用 `__myp_atomic_add_i32_addr`）。
- **`myp_io_cur`（当前句柄）留在 C TLS**（io_thread 并发 File 依赖每线程独立
  当前句柄），经新增 C helper `myp_io_cur_get/set` 读写——MYP 管 fd 表+操作，
  C 管每线程当前句柄，二者正确分工。
- 覆盖：fopen/current_handle/select/fclose/read_line/write/write_line/has_next/
  read_byte/read_i32be/read_double/write_byte/write_i32be/write_double/seek。
  `has_next` 用预读 1 字节存 pending（feof 语义）；`read_line` 逐字节到 \n/EOF；
  读写用 arena scratch 缓冲（I/O 量小，bump 不归还可接受）。
- 已知限制：`myp_io_table`/locks 从 C 的 FILE*+pthread mutex 换成 MYP fd+自旋锁
  （语义等价）；arena scratch 每次分配（不回收）。

验证：shadow 测试新增 **rt_io_test**（写文本/行/字节/大端 int/double → 读回 →
EOF 检测）exit=0；真实 io 测试用 MYP shadow 全过——**io_multi**（多文件交替读写
alt=A1B1A2B2）、**io_thread**（两个 @thread 并发 File 各写 200 行，a=200 b=200
wrong=0，验证 TLS 当前句柄 + 自旋锁）。bootstrap 16/16、全量 323/323。build.sh
循环新增 rt_io_test。

### v3.15.18 — runtime myp化 #15：类 slice 清理链 + @weak 弱引用注册表

**非破坏性**。补齐内存核心的最后两块（此前评估为"C 无法触及/死代码"的部分）：

**① `myp_alloc_class_slice` + `myp_release_class_slices_from_depth`**（region.myp）：
slice<类> backing 分配时按当前 `Region.depth` 注册进 @static 清理链
（node `[next@0][data@8][depth@16]`）；`myp_arena_release` 入口先释放 depth >=
当前值的注册 backing（保留外层 region 的），再回卷 arena（镜像 C）。当前无 codegen
调用方（slice backing 走 ref-counted `myp_alloc_slice_backing`），完整覆盖。

**② `@weak` 弱引用注册表**（新 `runtime_myp/weak.myp`）：
shadow C 的 `myp_weak_store`/`myp_weak_load`/`myp_weak_clear`。@static 全局单链表
注册表（C 用 64 桶哈希；weak 稀少线性扫描即可）+ **add-原子自旋锁**（用已有的
`__myp_atomic_add_i32_addr`——atomicrmw add 返回旧值，0=获取；无需新内建）。
`myp_alloc` 的 `myp_release` 类分支先调导出的 `myp_weak_notify_death`（null 本注册
表观察此对象的弱槽；并发 weak_load 在锁内重 bump rc → 返回 0 不释放），再委托
`myp_release_class_obj_ex`（其对 C 注册表做冗余 notify——MYP shadow 全部弱入口后
C 注册表恒空，不干扰）。弱槽地址经 raw 内存 `__myp_mem_load/store_i64` 读写。

验证：shadow 测试新增 **rt_weak_test**（弱存储不 retain / 弱读升级 rc+1 / 目标销毁
自动置空 / 持有者销毁注销）exit=0；真实 weak 测试用 MYP shadow 全过——weak_cycle
（断环+自动置空）、weak_multi_sub（多订阅者）、**cross_thread_arc（多线程 @weak，
验证自旋锁）**。bootstrap 16/16、全量 323/323。build.sh 循环新增 rt_weak_test。

### v3.15.17 — runtime myp化 #14：@region 层（myp_region_alloc + mark/release + 诊断）+ 修复 #13 类数组级联 bug

**非破坏性**。`runtime_myp/region.myp` shadow C runtime 的 @region 两级内存 region
层：

- **`myp_region_alloc`**：depth>0 用 MYP region bump，否则落到 MYP 进程 arena
  （`myp_arena_alloc`）。当前自举 codegen/stdlib 无调用方（slice/数组 backing 全走
  ref-counted `myp_alloc_slice_backing`），shadow 以完整覆盖。
- **`myp_arena_mark`/`myp_arena_release`**（codegen 在 @region 出入口发射）：mmap
  chunk 水位跟踪——mark materialize 一个 64KB chunk 并返回水位，release munmap 比
  水位新的 chunk 并回卷 used；嵌套 depth 计数。`myp_region_free_all` + region 字节
  诊断。
- **状态存 `@static class Region` 全局**（非 C 的 TLS）：@region 多线程并发会共享
  region 状态——现有测试全部单线程 @region，多线程场景暂未使用（文档化限制）。
- **修复 #13 潜伏 bug（alloc.myp `myp_release` 数组分支偏移）**：`pad` 应在
  **data-12**（非 -16，-16 是 elem_size）、`elem_size` 在 data-16（非 -20）。此前
  pad 读成 elem_size → `slice<Node>`（elem_size=8）pad 读 8 → CLASS/SLICE 逐元素
  级联永不执行 → 类数组/类 slice 元素泄漏。region_slice_class_arc 的 `live=128`
  暴露。rt_alloc_test 新增 **testClassArray** 级联回归。

验证：shadow 测试 rt_str_test/rt_num_test/rt_alloc_test/rt_region_test 全 exit=0；
region_slice_class_arc（live=0）与 region/test.myp 用 MYP shadow 输出正确；
bootstrap 16/16、全量 323/323。build.sh 循环新增 rt_region_test。

### v3.15.16 — runtime myp化 #13：内存核心——mmap bump arena + 分配/释放集群全量 MYP 化

**非破坏性**。核心分配器/ARC 层完整 MYP 化（`runtime_myp/alloc.myp` shadow C
runtime 的 `myp_alloc`/`myp_alloc_object`/`myp_alloc_class_array`/
`myp_alloc_slice_backing`/`myp_release`/`myp_free_object` + M9 存活计数）：

- **mmap bump arena**：`myp_arena_alloc` 用 `__myp_syscall`(mmap) 取块，16 对齐，
  chunk 头 32B（next/cap/used），超大分配独占块，bump 不归还（进程退出 OS 回收）。
  状态存 `@static class Arena` 全局（--shared 库模式验证可用）。
- **对象头布局与 C 一致**：字符串/类对象 8B 头 `{rc,type_id}` 在 data-8/-4；数组
  24B 头 `{count:u64,elem_size:u32,pad:u32,rc:u32,type_id:u32}`，rc/type_id 仍
  data-8/-4。跳过 C 的 16B 侵入链表 node（C 链表恒空，exit 清理无害）。
- **关键修复（ARC ABI 对齐）**：自举 codegen 对 `return __myp_addr_to_str(...)`
  （非 fresh 内建）发射 retain-on-return(+1)。MYP 分配器内部 rc 须设 **0**，让该
  retain 补成 1（等价正常代码 new 后 return 的净 rc=1）。此前设 1 → 返回 rc=2 →
  调用方 release 到 1 不归零 = 每字符串泄漏 1 引用，并引发巨型 main() 的
  `!= ""` 槽装载错乱（live 计数失控 → 内存布局偏移）。
- **myp_release 全分发**：rc→0 按 type_id——数组（pad 0=类逐元素 release / 2=slice
  胖指针逐元素）、字符串（计数-1）、类对象委托新增 C helper
  **`myp_release_class_obj_ex`**（weak 通知 + 每程序 `__myp_release_table` 分发 +
  `myp_free_object`——weak 注册表是 C 静态、release 表是生成代码产物，MYP 无法
  触及）。`myp_free_object` 经 --allow-multiple-definition interpose 回 MYP。
- **计数**：`Live.strings/objects/arrays` @static 全局（替代 C TLS），shadow
  `myp_live_*_count`。
- 已知限制：strict 头校验/逐类型计数/mmap 块 exit 回收未迁移（诊断性，非正确性）。

验证：**rt_str_test（260 字符串检查+ArcProbe）exit=0**、**rt_num_test exit=0**、
**新增 rt_alloc_test（字符串/数组/类对象/大块分配+release+计数）exit=0**；
bootstrap 16/16、全量 323/323。build.sh 循环新增 rt_alloc_test。

### v3.15.15 — runtime myp化 #12：核心 ARC/分配器层第一步（raw-address 原子内建 + myp_retain + myp_obj_type_id）

**非破坏性**。核心层（分配器/ARC）最难的障碍之一是**任意地址原子操作**——现有
`__myp_atomic_*` 内建是数组+下标寻址（GEP），不适用 rc 字段。自举新增 4 个
**raw-address 原子内建**（long 地址 → inttoptr → `atomicrmw`/`load atomic`/`store
atomic` seq_cst）：

- `__myp_atomic_add_i32_addr` / `__myp_atomic_sub_i32_addr` /
  `__myp_atomic_load_i32_addr` / `__myp_atomic_store_i32_addr`（sema + codegen）。

`runtime_myp/arc.myp` 迁移核心层第一步：

- **`myp_retain`**：对象头 `{_Atomic rc; type_id}` 在 data-8/data-4，`atomicrmw add`
  at (obj-8)。
- **`myp_obj_type_id`**：读 (obj-4)（RTTI，字符串=0xFFFFFFFE/-2）。

**核心层完整迁移的障碍评估**（`myp_release`）：rc→0 后按 type_id 分发——数组逐元素
release + `myp_free_class_array`、字符串 unlink 侵入分配链表 + free、类对象走
**每程序生成的 `__myp_release_table[tid]`**（codegen 产物，MYP 模块无法引用程序全局）
+ weak 表通知。完整 MYP 化需：raw-address 原子（本轮已加）+ mmap 自建分配器 +
程序全局访问机制——另立里程碑。

验证：bootstrap 16/16、全量 323/323、runtime_myp shadow PASS（ArcProbe：retain→rc+1、
type_id=-2）。

### v3.15.14 — runtime myp化 #11：SHA-256 摘要

**非破坏性**。`runtime_myp/hash.myp` 新增 `myp_hash_sha256`（bridge hash_bridge.c
纯函数，返回 64 位小写 hex）。全 `uint`（i32）算术——加法自然回绕、`uint >>` 发射
逻辑右移（SHA-256 的 Σ/σ 用 `>>>` 语义）、`rotr` 内建。K/H 常量用 `uint(0x…)`（hex
大字面量解析为 long 再截断）。固定数组 `uint[64] w/K`、`uint[8] hv` 作局部（传参会
触发 GEP 类型错，仅局部可用）。

标准向量全过：`""`→e3b0c442…b855、`"abc"`→ba7816bf…15ad、`"hello"`→2cf24dba…9824、
`"The quick brown fox…"`→d7a8fbb3…e592。验证：runtime_myp shadow PASS。

### v3.15.13 — runtime myp化 #10：CRC-32 校验和 + shadow 加固边缘测试

**非破坏性**。`runtime_myp/crc.myp` 新增 `myp_crc32`（zlib CRC-32，位运算法无查表，
多项式 0xEDB88320，返回 32 位位型）。标准向量验证：`""`→00000000、`"a"`→e8b7be43、
`"hello"`→3610a686、`"123456789"`→cbf43926（经典校验值）。

**shadow 加固**：shadow 机制修复（#9）后 MYP 实现真正执行——rt_str_test 加 11 条、
rt_num_test 加 7 条边缘用例（空 old 替换、纯空白 trim、尾分隔符 split、空前缀、
大写 hex、u64 2^63 等）全部通过，未发现新 bug。

验证：runtime_myp shadow PASS（str+num，MYP 版本真实执行）。

### v3.15.12 — runtime myp化 #9：base64 层 + **shadow 机制修复**（此前测试走 C runtime）+ 2 个真实 bug

**非破坏性**。`runtime_myp/base64.myp` 新增 `myp_base64_encode`/`myp_base64_decode`
（bridge 纯函数，shadow 验证；rt_str_test 加 12 条断言含 round-trip）。

**⚠️ shadow 机制修复（关键）**：此前 `runtime_myp/build.sh` 编译 MYP 模块**未加
`--shared`** → 函数是 `define internal`（局部符号 `t`），无法 shadow libmyp_rt.a 的
同名全局符号 → **之前所有 "shadow PASS" 实际测试的是 C runtime，MYP 版本从未被真正
调用**。同时 build.sh 模块循环复用同一 `/tmp/rt_myp_m.o` 路径，后编译模块覆盖前面的。
修复：`--shared`（库模式，函数外部链接 `define`）+ 每模块独立 `.o`。反汇编确认
`myp_strlen` 现为 MYP 的 charcode 扫描循环（非 C `call strlen`）。

**shadow 生效后抓到的 2 个真实 bug（均修复）**：
1. **`myp_charcode` 自递归**：MYP 版内部用 `__myp_charcode` 内建（发射 `call @myp_charcode`）
   → 无限递归段错误。改用 raw-memory 直接读字节 `__myp_mem_load_i8(ptr(s)+i)`。
2. **`myp_str_replace_all` 空 old_str 死循环**：`oldl==0` 检查放在 count 循环之后——
   oldl=0 时 `j<oldl` 循环不跑 → found=k=i → i 不前进 → 死循环（perf 定位 99.9%
   在该函数）。C 版对 `!*old_str` 提前返回拷贝，对齐修正。perf 定位：完整测试在
   多次分配后堆布局下触发，gdb/ASAN 布局不同不触发（掩蔽性极高）。

验证：runtime_myp shadow PASS（str+num 全断言，MYP 版本真实执行）。

### v3.15.11 — runtime myp化 #8：热字符串助手 myp_ord/charcode/chr/strdup

**非破坏性**。`runtime_myp/str.myp` 补 4 个高频小函数（shadow C runtime 验证，
rt_str_test 扩 9 断言）：

- `myp_ord` / `myp_charcode`：O(1) 取字符码（自举 lexer 等按字符扫描热路径）。
- `myp_chr`：码点 → 1-4 字节 UTF-8 计数字符串（非法码点 → 0xFFFD 替换符）。
- `myp_strdup`：拷贝计数串（含 NUL）。

**性能剖析结论（perf 自举编译自身）**：myp_* 函数自身耗时很小（最高 myp_release
1.25%）；真正的性能大头是 **`__strlen_evex` 67%**（libc strlen）——字符串无长度
字段，每次操作 O(n) 重扫。根治 = 字符串头加 len 字段（另立任务，影响 runtime 布局
+ 两编译器字面量 GEP）。**按静态调用点，剩余最多的是分配器/ARC
（myp_release 22k / retain 4k / alloc_object 862）——最难且 self 耗时小，另评。**

### v3.15.10 — runtime myp化 #7：bytes 层（bytes()/str(bytes)/bytesOf 三转换）

**非破坏性**。`runtime_myp/bytes.myp` 新增（codegen 直接发射的三个 bytes 转换，
shadow C runtime 验证）：

- **`myp_str_to_bytes`**：string → `ubyte[]` backing（`new ubyte[n]` + 逐字节拷贝，
  返回类型 `ubyte[]` 即 LLVM ptr，匹配 codegen `call ptr`）。
- **`myp_bytes_to_str`**：`ubyte[]` backing → string（count 在 data-24 头字段读长度）。
- **`myp_uint_to_bytes`**：位向量按小端 → `ubyte[]`（§5.1 `bytesOf`，nbytes 钳 1..8）。

**技巧**：`__myp_str_ptr(数组值)` 内部就是 `ptrtoint ptr %x to i64`，对任意 ptr 值
（含数组 backing）成立——拿数组地址无需新内建。`bitcast` 内建**不能** ptr→i64
（LLVM 需 ptrtoint）。

验证：runtime_myp shadow PASS（rt_num_test 加 7 条 bytes 断言：round-trip
`str(bytes("hello"))`=="hello"、空串、`bytesOf(0x1234)`→[52,18,0,0] 小端）。

### v3.15.9 — runtime myp化 #6：浮点位型 myp_f64_bits_hex / myp_f32_bits_hex

**非破坏性**。`runtime_myp/num.myp` 补浮点位型十六进制——自举 codegen 发射 double
常量时调用 `myp_f64_bits_hex`（LLVM 文本 IR 浮点常量 `0x + 16 大写 hex`）：

- **`myp_f64_bits_hex(double)`**：用 `bitcast<T,U>(x)` 内建（LLVM bitcast 指令）
  取 64 位位型 → 逐 nibble 大写 hex。
- **`myp_f32_bits_hex(float)`**：float 先精确拓宽为 double 再取 64 位（对齐 C 版）。
- `bitcast` 内建验证：`bitcast<long>(1.0)` 正确发射 `bitcast double to i64`。

验证：runtime_myp shadow PASS（rt_num_test 加 7 条位型断言，如
`myp_f64_bits_hex(-0.0)`→"0x8000000000000000"、`pi`→"0x400921FB54442D18"）。

### v3.15.8 — runtime myp化 #5：通用拼接 myp_strcat + 进制格式化 myp_fmt_u64_base

**非破坏性**。补两个自包含转换（shadow C runtime 验证，str+num 测试扩断言）：

- `myp_strcat`（str.myp）：通用字符串拼接——自举 codegen 对 `s + t` 发射
  `@myp_strcat`（非 `s=s+x` 快路径），MYP 化后所有拼接走 MYP 版本。
- `myp_fmt_u64_base`（num.myp）：32 位位型按无符号在 2..16 进制输出（upper 控制
  hex 大小写），`Fmt.u/x/X/o/b` 全部走这里（`Fmt.x(-1)`→"ffffffff"）。

float/double 解析（strtod/atof）与格式化（%g、myp_fmt_double_f/e/g）依赖 libc，
另立里程碑（TODO）。验证：runtime_myp shadow PASS（str+num 全断言）。

### v3.15.7 — runtime myp化 #4：数字层（整数解析+格式化，9 函数）+ 自举 FFI 实参转换修复

**非破坏性**。`runtime_myp/num.myp` 新增（shadow C runtime 验证，`runtime_myp/build.sh`
现在同时运行 rt_str_test + rt_num_test）：

- **整数解析**（strtoll/strtoull base-0：空白/符号/0x 十六进制/0 八进制/十进制）：
  `myp_str_to_long` / `myp_str_parse_int` / `myp_str_to_uint` / `myp_str_to_ulong`。
- **整数/布尔格式化**（itoa 2 位查表法，INT32/64_MIN、u32/u64 最大值安全）：
  `myp_to_string_i32` / `myp_to_string_i64` / `myp_to_string_u32` /
  `myp_to_string_u64` / `myp_to_string_bool`。`ulong` 除法发 udiv/urem 处理位模式。
- float/double 解析（strtod/atof）与格式化（%g）依赖 libc，另立里程碑（TODO）。

**自举编译器 bug 修复（对拍 parity）**：`funcParamLts` 只查顶层函数、不查 FFI 声明
→ FFI 调用整数实参不提升（`myp_to_string_i64(0)` 传 `i32 0` 进 `i64` 参数，高 32 位
垃圾；oracle mypc 正确 sext）。补 FFI 分支后实参经 `convertValueU` 正确提升。此缺陷
此前被 ABI"低 32 位恰好在低位"掩盖（小值侥幸正确）。验证：bootstrap 16/16、
全量 323/323、runtime_myp shadow PASS。

### v3.15.6 — runtime myp化 #3：字符串层全部 MYP 化（22 个函数）+ 自举 O(N²) 修复

**非破坏性**。自举编译器（`tools/selfhost/src/*.myp`，mypc 冻结）推进运行时 MYP 化——
`runtime_myp/str.myp` 补齐字符串层全部函数（shadow C runtime 验证：
`runtime_myp/build.sh` + `bench/freestanding/rt_str_test.myp`，63 断言）：

- **新增 8 个**：`myp_str_split_get` / `myp_str_replace` / `myp_str_replace_all` /
  `myp_str_repeat` / `myp_str_pad_left` / `myp_str_pad_right` / `myp_str_reverse` /
  `myp_str_join`。至此 stdlib `text.myp` 的 18 个 `myp_str_*` FFI **全部 MYP 化**
  （22 个，含内部 len/eq/cmp/hash）。
- `myp_str_join` 用 `string[]` 数组参数——自举 codegen `varElemType` 已支持动态数组
  参数（为 `vecAdd` 等数组 @op 所加），无需改编译器。

**性能修复（自举编译自身 2m46→16s，10x）**：
- preamble declare 剔除从 `splitGet` 逐行（O(N²)：`myp_str_split_get` 每次从串头
  strstr 数到 index）改单遍 charcode 扫描 + 组合串判定（commit 3e74d3a）。
- `link.myp` `nmSymbols`/`nmDynSymbols` 同款 O(N²) → 新增 `Link.splitLines` 单遍切行
  （commit 6ef50ad）。`runtime_myp/str.myp` 预留 split_get 带偏移/split_all 的 TODO。

### v3.15.5 — 自举编译器同步 P1 缩放：sema/codegen 热路径 O(N) 扫描 O(1) 索引化

**非破坏性（性能，行为不变）**，自举（selfhost，`tools/selfhost/src/*.myp`）端
同步 v3.15.4 的 O(1) 索引化（此前只修了 C++ oracle，自举端 P6 仍 O(N²) 37x/2N）。

对照 `bench/compiler/`（N=1000，`myp_self2` 旧 vs 新）：

| 基准 | 旧 selfhost | 新 selfhost | 加速 |
|------|------|------|------|
| P1 类×裸属性 | 0.95s | 0.74s | 1.3x |
| P2 接口×方法调用 | 0.90s | 0.50s | 1.8x |
| P3 接口×变量声明 | 1.24s | 0.76s | 1.6x |
| P4 struct×字段 | 0.84s | 0.51s | 1.6x |
| P5 enum×variant | 0.71s | 0.44s | 1.6x |
| P6 类×方法调用 | 11.95s | **0.76s** | **15.7x** |
| P7 泛型实例 | 6.32s | 1.67s | 3.8x |

主要改动（`codegen.myp` + `sema.myp`，`StrHashMap<int>` 索引）：

1. **codegen**：`generate()` 入口一次建 `classIdx_`/`ifaceIdx_`/`enumIdx_`/
   `structExist_`（裸名+`Parent::name`）/`topFunc_`（函数+FFI）/`typeIdNames_`+
   `typeIdMap_`（type-id 表预计算，原 `classTypeId`/`classTypeNames`/`classTypeCount`
   每调用 O(N²) seen 去重）。`isClassName`/`isInterfaceName`/`isEnumName`/
   `isStructName`/`isTopLevelFunc`/`findAction`/`hasMethodInClass`/`hasEventInClass`/
   `findEvent`/`isStaticAction`/`classImplements`/`methodParamLts`/`methodRetAstType`/
   `methodParamAstType` 等改 O(1) `classIndex`/索引；`emitArcSupport` 的每类线性扫全类
   改 `classIndex`。

2. **sema**：`classIdx_`/`structIdx_`/`enumIdx_`/`ifaceIdx_`/`funcIdx_`/
   `methodSigIdx_`（`"cls.meth"`→索引，注册点增量登记）并行维护；
   `findClass`/`findStruct`/`findEnum`/`inInterface`/`isFuncName`/`isGenericClass`/
   `resolveBase`（精确命中）/`findClassTypeParams` 及 `findMethodRet*`/
   `findMethodParams`/`isMethodCoro`/`isFuncSectionMethod`/`hasMethod` 改 O(1)。

3. **P6 残余说明**：P6 由 O(N²)（2.24→11.88→78.90s@500/1k/2k）降到近线性
   （0.47→0.75→1.70→4.85s@500/1k/2k/4k，~2.3–2.9x/2N）；残余超线性为自举编译器
   **字符串处理 ARC 开销**（perf：`myp_release`+`myp_retain` 32%、`strcmp`+`myp_str_eq`
   14%——自举用字符串拼接生成 IR，每串引用计数），非线性扫描。

**回归**：bootstrap 16/16 不动点（oracle↔selfhost token/ast/sema 三方字节一致、
myp_self2==myp_self3）；selfhost 全量 323/323（`exception_thread` 为既有 @thread
时序 flaky，复跑通过）。

### v3.15.4 — 编译器缩放 P1：sema/codegen 热路径 O(N) 扫描 O(1) 索引化（P1–P6 线性化）

**非破坏性（性能，行为不变）**，oracle（mypc）端。根因：sema/codegen 在**每个
表达式/语句**的热路径里 `for (auto& cls : current_tu_->classes)` / `interfaces` /
`structs` / `functions` 线性扫全表（方法解析、接口判定、enum 构造、struct 查找、
@op/@coro/@async 函数查找、泛型实例复用查找），N 个类 × N 次调用 = O(N²)。
对应 `bench/compiler/`（P1–P7，`docs/testing_benchmark_roadmap.md` §5）实测斜率：

| 基准 | 修复前（16k 或 4k） | 修复后 | 现状 |
|------|------|------|------|
| P1 类×裸属性 | 10.26s@16k | 0.49s@16k | 线性（1.90x/2N） |
| P3 接口×变量声明 | 8.27s@16k | 0.48s@16k | 近线性（2.74x/2N） |
| P6 类×方法调用 fallback | 11.21s@16k | 4.67s@16k | 线性（1.92x/2N） |
| P7 泛型实例 | 15.8s@4k | 10.5s@4k | 剩余超线性见下 |

主要改动（`src/sema/*`、`src/codegen/*`、`include/mylang/*`）：

1. **O(1) 声明索引**：codegen `generate()` 入口一次建全 `class_decls_`/
   `interface_decls_`/`enum_decls_`/`struct_decls_`（key=name 或 `Parent::name`）/
   `first_member_class_`（成员名→首个定义类）；sema `analyze()` 建 `class_indices_`
   /`function_indices_`（含单态化后 `indexFunction` 增量登记）/`struct_by_name_`。
   `findClass`/`findEnum`/`findStruct`/`findClassDecl`/`findFunctionDecl` 全部改
   哈希查找，`isInterfaceName` 等新增辅助。

2. **热路径扫描替换**（逐个 perf 定位）：
   - `visitMemberAccess`：static 类判定、接口判定 → `findClassDecl`/`isInterfaceName`。
   - `visitBinaryOp`：`@op` 函数扫描 → `any_op_functions_` 短路；`@coro` 返回句柄
     扫描 → `any_coro_functions_` 短路。
   - `visitCall`/`isAsyncCallee`：函数扫描 → `findFunctionDecl`。
   - `resolveGenericCall`/`resolveGenericStaticCall`：实例复用线性扫函数 →
     `findFunctionDecl`。
   - `generateCallImpl`：接口 dispatch 的类名判定、enum variant 构造、静态类判定、
     本类属性对象解析 → `findClass`/`findEnum`。
   - `memberObjectClassName`/`callReturnTypeNode`/`generateNewExpr`/`generateIdentifier`
     /`generateMemberAccess`/`generateAssignment` 等 15+ 处 → `findClass`/索引。

3. **P7 剩余超线性说明**：sema 与 -O0 codegen 已降到斜率 <3.0（泛型实例查找/表扩容的
   MYP 侧 O(N²) 已消除）；-O2 默认管道下 P7 仍 ~3.4–3.6x/2N，perf 归因为 **LLVM
   SROA/mem2reg（`PromoteMem2Reg::run` 65%）**——N 个互异 struct 类型 + N 个泛型实例
   全部内联进单个 `run()`（1 基本块、~4N alloca）后，LLVM O2 管道在单函数上呈超线性，
   非 MYP 自身线性扫描。属 LLVM 内部成本，不阻塞其余基准线性化。

**回归**：oracle 323/323、selfhost 323/323、bootstrap 16/16 不动点；bench P1–P6
斜率全部 <3.0。

### v3.15.3 — 借用 ARC 参数重赋值 UAF 修复 + 测试框架可信度（T1/T2）+ selfhost 诊断输出修复

**非破坏性（bug 修复 + 测试基础设施）**，oracle（mypc）与 selfhost 双端同步：

1. **借用 ARC 参数重赋值 UAF（P0）**：`string f(string s){ s = s + "a"; s = s + "b";
   return s; }` —— 字符串/类参数是**借用**（非 ARC 槽），此前赋值路径要么走就地追加
   `myp_str_append`（消费借用的入口值 → 改写/释放调用方字符串）、要么 fresh 临时在语句
   末被 flush 释放 → 参数槽悬垂、链式重赋值读已释放内存。修复：借用参数**首次重赋值**
   惰性提升为拥有槽（fresh 消费 / 别名 retain，不释放借用的入口值，注册到**函数作用域**
   而非块作用域），后续重赋值走普通 owned-slot 路径。C++ `codegen_stmt.cpp` +
   selfhost `codegen.myp`（`funcPtrSlots_` + `funcPtrSlotHas`）双端镜像。回归
   `tests/@test/str_param_append.myp`（链式/循环/别名/类参数 7 断言，双编译器）。

2. **测试框架可信度（T1/T2，`docs/testing_benchmark_roadmap.md` §二）**：
   - **T1 缺失 expected 默认失败**：普通模式找不到 `tests/expected/*.expected` 不再
     静默把输出当基线并计 PASS → 报 `MISSING BASELINE` 且计 FAIL；仅 `--update`
     允许创建 baseline（漏提交测试资产 CI 必然失败）。
   - **T2 负测试校验诊断原因**：解析 `// EXPECT ERROR: <substring>` 并按固定字符串
     （`grep -F`）断言 stderr 含该子串；意外 SIGSEGV/SIGABRT/ASan 归类为 `CRASH`
     而非负测试通过。同步修正 27 个历史漂移的 `EXPECT ERROR` 注释（此前是人工描述、
     从未机器断言，与实际诊断不符）。

3. **selfhost 诊断输出修复（T2 暴露的既有 parity 缺口）**：
   - **UTF-8 双重编码**：`main.myp` `Frontend.escape`/`dotToSlash` 用 `__myp_chr(c)`
     把 `myp_charcode` 返回的**字节**当**码点**再 UTF-8 编码（0xE5 → c3 a5，中文诊断
     变 mojibake）→ 改用 `Str.substring(s, i, i+1)` 字节透传（对齐 `ast.myp Dump.esc`
     与 C++ `escapeDumpString`）。
   - **BUG-046 镜像**：selfhost 补同名 static 方法（签名不同）诊断
     `duplicate static action '...' in class '...' (different signature)`（`sema.myp`
     `staticActionSig` 签名串比较；签名相同保持历史静默合并）。

**回归**：oracle 323/323、selfhost 323/323（parity 0 差距）、bootstrap 16/16
不动点（myp_self2 == myp_self3 md5 一致）。

### v3.15.2 — 自举 link.myp 硬编码重构（P0 工具链探测 / P1 缓存路径 / P2 集中配置 / P3 平台）

`tools/selfhost/src/link.myp`（自举编译器链接器）去硬编码：

- **P0 工具链探测**：`findLlc`/`findOpt`/`findHostTriple` 改为数据驱动——通用
  `probeTool(name, env, versions)`：`MYP_*` 环境变量 → `command -v`（PATH 优先，含
  `<name>-<ver>` 版本化命令）→ 版本绝对路径候选表（`/usr/bin/<name>-N`、
  `/usr/lib/llvm-N/bin/<name>`）→ 回退裸命令名。`$CC` 环境变量优先 + `command -v
  cc`/`gcc` 探测。
- **P1 缓存路径**：固定 `/tmp/myp_self_*.o`（并发 myp_self 互相覆盖）→ 内容哈希缓存
  `FNV-1a 64`（源码+标志 → `/tmp/myp_rt_cache/myp_rt_<hex>.o`，对齐 C++ `cacheObj`）：
  runtime.c / coro_ctx.S / runtime_gpu.c / runtime_lib.c / 各 bridge 全部走哈希缓存，
  跨进程共享复用、无并发覆盖；`myp_self2` 链接 ~0.35s（runtime.c 复用）。
- **P2 集中 Toolchain 配置**：消灭 5 处重复 gcc flags——`findCc()`/`baseCflags(inc)`/
  `ldLibs()` 单点维护。
- **P3 平台基础**：`isWindows()`（`$OS`/`uname -s` → MinGW）+ 平台链接库
  （`-lws2_32 -lwinmm`）+ 平台协程汇编（`coro_ctx_win.S`）选择，对齐 C++ 平台分支。

**回归**：selfhost 322/322、bootstrap 16/16 不动点、selfhost 对拍 95/95；`$CC`/
`MYP_LLC`/`MYP_OPT`/`MYP_LLVM_CONFIG` 环境覆盖验证通过。

### v3.15.1 — `@derive(Json)` 派生序列化（serde 式类级派生，P0 标量/string/bool）

**非破坏性（additive）**，oracle（mypc）与 selfhost 双端同步实现：

- **类级注解 `@derive(Json)`**：编译器在 sema 前为类自动注入 `string toJson()` 与
  `void fromJson(string j)` 两个方法（合成源码 → 复用既有 parser → 注入类 action 段）。
- **属性类型 v1**：int/long/short/byte/uint/ulong/ushort/ubyte、double/float、bool、
  string。toJson 输出合法 JSON（`Json.escape` 转义字符串）；fromJson 用 `json.myp`
  路径查询回填，round-trip 一致。数组/类/struct/元组/函数属性 → 编译期诊断（负测试）。
- 泛型类 `@derive` 暂不支持（v1 诊断）；非 `Json` 派生名 → 诊断。
- `json.myp` 新增 `Json.escape` 静态方法。

**回归**：oracle 322/322、selfhost 322/322、bootstrap 16/16 不动点、dump 对拍 95/95、
fmt 对拍 5/5。新正测试 `tests/@test/manual_serde_derive.myp`（round-trip + 合法 JSON）；
负测试 `tests/negative/derive_unsupported_type.myp`。

**附带发现**：字符串**参数**连续重赋值链有 ARC use-after-free 既有 bug（局部变量无碍，
`Json.escape` 已用局部规避）；待单独修复。

### v3.15.0 — 表达力三小改：多行字符串 / 字符串插值 / 空安全 `?.` `??`

**非破坏性（additive）语言特性**，oracle（mypc）与 selfhost 双端同步实现：

1. **多行字符串 `"""..."""`**：三个连续引号开始，到下一个未转义三连引号结束；
   内容可含换行与单个 `"`，`\` 转义与单行字符串一致。**raw 语义**：三引号串不做
   `$name` 插值展开、保留字面 `$`（`Token.raw` 标记；parser 对 raw 串跳过插值）。

2. **字符串插值**（两种形式）：
   - `${expr}` —— 任意表达式插值（lexer 在字符串内遇 `$` 后跟 `{` 时合成
     `interp_open`/`interp_close` token，parser 折叠为 `+` 拼接）。
   - `$name` —— 简单标识符插值（既有语法，保持不变）。
   - 字面 `{` `}` 不受影响（无 `$` 前缀即字面，JSON 等字符串零冲突）；
     多行 `"""..."""` 内不插值。

3. **空安全 `?.` / `??`**（parser 脱糖，复用既有 `!= null` 与三元语义）：
   - `a ?? b` → `(a != null ? a : b)`（右结合）。
   - `a?.m(args)` → `(a != null ? a.m(args) : null)`；`a?.f` 同理。
   - 结果须可空：类字段/类返回的方法可用；值类型成员（int）无法为 null，
     报 `ternary branches have incompatible types`（需 Option<T> 或 `??` 默认值）。
   - 注意：`a` 在脱糖后求值两次（条件 + 真分支），对变量/字段读取无副作用；
     副作用调用作左操作数时请先提局部变量。

**回归**：oracle 320/320、selfhost 320/320、自举不动点 16/16、tokens/ast/sema
对拍 95/95、fmt/viz 对拍全绿。新正测试：`tests/@test/manual_lexer_triple_string.myp`、
`manual_lexer_interp.myp`、`manual_null_safe.myp`。

### v3.14.2 — 深度学习框架：SD1.5 文生图全管线（D1–D6）

纯 MYP 实现的通用深度学习框架（`examples/deeplearning/`，LLVM 后端）扩展出
**SD1.5 文本→图像**全管线，三大网络全部数值验证通过：

- **D1 DDIM 调度器**（`diffusion/`）：`DDIM(η=0)` 纯 MYP，vs numpy float64 字节
  精确（diff==0）+ diffusers 0.39 交叉 3.5e-7。
- **D2 CLIP 文本编码器**：QuickGELU + 因果/padding 掩码 attention（类内私有规避
  BUG-046），77 位置 vs transformers maxAbsDiff=2.7e-4；D2b BPE 分词器（GPT-2
  `</w>` 词尾式）。
- **D3 UNet**：GroupNorm(32 组)/attention2(cross+self)/GEGLU/nearestUpsample2x，
  vs numpy 0~1.8e-7；D3b 完整前向 maxAbsDiff 1.29e-5。
- **D4 VAE decoder**：完整前向数值验证通过（VAE DECODE OK maxAbsDiff 9.4e-6）。
- **D5 端到端**：CLIP→UNet 50 步→VAE→PPM 出图（IMAGE OK 0.11%）；热点算子
  `@parallel for` ~14x。
- **D6 GPU 加速 + 交互工具**：`@gpu for` 加速扩散管线；`tools/.../gen_image.py`
  prompt → 分词/编码 → GPU DDIM → GPU VAE → PNG（交互循环/单次/--steps/--skip-ref）。

附 `examples/deeplearning/README.md` 各子目录文档 + 里程碑计划 `diffusion_plan.md`。

### v3.14.1 — 优化点推进：多文件并行编译 + GPU R0 止血 + parity 零差距

优化清单推进（多文件并行编译 ① / GPU R0 止血 ②）+ 前置两处修复：

1. **`==`/`!=` 字符串比较类型检查（58def81）**：`bool == string` 应报错，此前静默
   放行（oracle 与 selfhost 双端）。
2. **selfhost parity 零差距（c3d88ac）**：补齐 4 项 parity 差距（`arc` / `arc_m2` /
   `weak_cycle` / `closed-lib`），oracle 与 selfhost 同套 `tests/run_tests.sh`
   315/315 零差距。
3. **多文件并行编译（682ab9b）**：
   - `@parallel for` CPU 并行体从纯数值内核切到完整 `generateStmt`（支持字符串 /
     `new` / 方法调用 / 字段访问）；`@gpu for` 保持纯数值内核（NVPTX 无字符串/堆，
     错误消息只提 `'@gpu for'`）。
   - selfhost 前端多文件 lexer+parser 用 `@parallel for` 并行化 + 顺序合并。
   - 打通自举约束：selfhost 源码可被 mypc 编译（bootstrap 种子不受限）。
4. **GPU R0 止血（f803439）**：`gpu_check_err` 统一 CUDA 错误检查（失败记录 kernel
   名 + 错误码、置 `g_force_cpu`、首错打印一次明确诊断）；kernel 名跟踪（launch
   失败信息带 grid/block 定位 OOB）；全管线回退（`myp_gpu_init` 开头
   `if (g_force_cpu) return 0`）；新 FFI `myp_gpu_force_cpu()` + `Cuda.forceCpu()`
   （oracle/selfhost 双端注册）。

**回归**：oracle 317/317、selfhost 317/317、GPU 回退 61/61、bootstrap 16/16
不动点、parity 0 差距。

### v3.14.0 — Windows 移植里程碑 1/2：运行时层交叉编译通过（MinGW-w64）

**背景**：评估 MYP 全生态 Windows 适配可行性后，首个里程碑 = 让运行时层
（`runtime.c` + stdlib bridges）能在 Windows 交叉编译通过，收敛 POSIX 依赖。

**交叉编译验证工程**（新）：
- `cmake/win64-mingw.toolchain.cmake`：MinGW-w64 toolchain（Linux host →
  Windows x86_64）。注意 `CMAKE_TOOLCHAIN_FILE` 须用**绝对路径**（相对路径会被
  CMake 相对源目录解析）。
- `cmake/cross-runtime/CMakeLists.txt`：只编译 `runtime.c` + 无外部依赖 bridges
  （不依赖 LLVM——编译器本体 mypc/myp_lsp 需 Windows 版 LLVM 库，属下一步，
  可走 llvm-mingw 或 Windows/WSL2 原生构建）。

**Windows 平台适配层 `src/runtime/platform_win.h`（新）**：
- termios（raw 模式 → `GetConsoleMode`/`SetConsoleMode`；ICANON/ECHO/VMIN/VTIME）
- ioctl + TIOCGWINSZ（终端尺寸 → `GetConsoleScreenBufferInfo`）
- dirent（opendir/readdir/closedir → `FindFirstFile`/`FindNextFile`）
- stat 宏 S_ISDIR/S_ISREG、`mkdir→_mkdir`、`lstat→stat`
- `setenv/unsetenv → _putenv`、`sysconf(_SC_NPROCESSORS_ONLN) → GetSystemInfo`
- `poll → WSAPoll`（复用 winsock2.h 的 `pollfd`；runtime 协程 fd 就绪检测用）
- pthread/semaphore 由 MinGW 自带 winpthreads 提供（链接 -lpthread）

**bridges Windows 移植**（`#if defined(_WIN32)` 分支）：
- `net_bridge.c`：Winsock（winsock2.h + WSAStartup 一次性初始化 + closesocket
  + ioctlsocket 非阻塞）。TCP 语义与 POSIX 1:1 兼容，是网络移植最顺的一块。
- `process_bridge.c`：`system`/`_popen`/`_getpid` + `OpenProcess`（运行检测）+
  `CreateProcess`（spawn 后台进程）。`getppid` 无对应 → 返回 -1。
- `uds_bridge.c`：`_WIN32` 下 **stub**（UDS 在 Windows 用命名管道，属后续里程碑）。
- `regex_bridge.c`：`_WIN32` 下 **stub**（POSIX regex 后续换 PCRE 或移植 mini 引擎）。

**验证**：
- 交叉编译：`libmyp_win_runtime.a`（PE/COFF 目标）构建 **100% 通过**——
  runtime.c + net/uds/process/regex/json/base64/date/hash 全部 .obj 产出。
- Linux 零回归：`mypc` 重建 OK；`hello`、`tests/async_socket`（协程+网络+超时）
  实跑正常；coro_stack/async_socket/regex/process 编译 0 errors。

**协程 Win64 汇编（`src/runtime/coro_ctx_win.S`，新）——里程碑 3**：
- Win64 ABI 上下文切换：非易失寄存器 rbx/rbp/rsi/rdi/r12-r15 + **xmm6-xmm15**
  （10 个。SysV 的 xmm 全是 caller-saved 不用存，但 **Win64 的 xmm6-15 是
  callee-saved**——协程内大量 double，不保存必错乱）。
- 保存块 256 字节基址/有效 248：`myp_ctx_init` 设 base=top-256（base%16==0 →
  xmm `movaps` 对齐；恢复后 rsp=base+248，%16==8 满足 Win64 函数入口对齐）。
- `runtime.c` 的 `myp_ctx_init` 改三态（Linux 7 槽 / Win64 256 块 / ucontext 回退）。
- 交叉编译 + 链接验证：`coro_ctx_win.S.obj`（pe-x86-64）反汇编确认切换逻辑，
  `myp_ctx_switch` 符号链接解析；Linux `coro_nest` 测试零回归。
- 注：MinGW as（PE 目标）不支持 `.type`/`.size` 伪指令（ELF 专属）→ 已移除。
- **真机验证（协程内 double 跨切换的 xmm 保护）需 Windows 实机**（里程碑 4）。

**GPU 运行时交叉编译（`runtime_gpu.c` + `runtime_rocm.c`）**：
- dlfcn → `LoadLibrary`/`GetProcAddress`（`_WIN32` 宏兼容层：RTLD_* 定 0、
  dlsym→GetProcAddress、alloca.h→malloc.h）；`libcuda.so.1` → `nvcuda.dll`、
  `libamdhip64.so` → `amdhip64.dll`。
- CUDA driver API 类型由 runtime_gpu.c 自 typedef（无需 cuda.h）→ 交叉编译通过。
- 顺带修：`net_bridge.c` Winsock `setsockopt` optval 是 `const char*`（cast）；
  `runtime.c` `%zu` → `%llu`+cast（MSVCRT printf 不支持 %zu）。

**后续修复（M4）：SOCKET fd 表 + regex 迷你引擎 + warning 清零**：
- **SOCKET fd 表**（`net_bridge.c` + `platform_win.h`）：64 位 SOCKET → 小整数
  int fd（`myp_win_fd_alloc/lookup/free`，跨 TU），消除把 UINT_PTR SOCKET 塞进
  int 的截断隐患；runtime 协程 `poll` 查表还原 SOCKET 再 `WSAPoll`。
- **regex 迷你引擎**（`stdlib/bridges/regex_win.c`，新）：AST + 贪婪回溯 ERE 子集
  （字面量/`.`/`[...]`范围与取反/`*+?`/`^$`/`()`/`|`/`\`转义），30 用例全对；
  `regex_bridge.c` 的 `_WIN32` 分支改 include 此引擎（替代系统 `<regex.h>`）。
- 交叉编译 warning 清零：删冗余变量、`(void)dlflags`、修 `json_bridge` `strrchr`
  未初始化读（潜在 UB，顺带修 json 路径修改 bug）。

**遗留障碍（后续专项，需设计或 Windows 工具链）**：
- UDS→命名管道：MOS 用**多连接 + 多路复用**（`poll` 监听 fd + 多个客户端 fd），
  命名管道单实例模型不匹配，需 MYP 侧接口适配设计（涉及 MOS IPC）。
- 编译器本体（LLVM）：需 Windows 版 LLVM 库（llvm-mingw 或 Windows 原生）。

**编译器本体 Windows 化（`src/main.cpp` + `CMakeLists.txt`，LLVM 库之外的部分）**：
- `src/main.cpp`：新增平台配置，`_WIN32` 下生成程序用 MinGW gcc + `-lws2_32
  -lwinmm`（Linux 为 `-lm -ldl`）、协程汇编选 `coro_ctx_win.S`；8 处 gcc 命令
  改用 `kCC`/`kPlatformLibs`。Linux 生成程序行为不变（全量回归确认）。
- `CMakeLists.txt`：`MYP_THREAD_LIBS`（Linux pthread / Windows winpthreads）、
  `coro_ctx.S` → 按平台选 `coro_ctx_win.S`。Windows 配置验证：FindThreads 解析
  成功，仅卡在 `find_package(LLVM)`（装好 Windows LLVM 后 `-DLLVM_DIR` 指向即可）。
- Windows 分支编译验证需 Windows LLVM（用户侧安装）。

**测试套件 Windows 化（Git Bash 兼容层）**：
- 新增 `tests/lib/portable.sh`（共享移植层，各测试脚本 source）：平台检测
  （MINGW/MSYS/CYGWIN）+ `myp_timeout`（Linux 透传 GNU timeout；Windows 纯 bash
  后台轮询超时强杀，退出码 124 语义一致）+ `myp_resolve_bin`（.exe 前向兼容，
  仅 Windows 启用）+ `myp_guard_ulimit`（Linux 防 OOM；Windows 静默跳过）。
- `run_tests.sh`（4 处 timeout → myp_timeout）、`run_tests_tsan.sh`、
  `bugs/run_bugs.sh`、`stress/run_stress.sh`、`test_myp_self.sh` +
  `test_myp_bootstrap.sh`（ulimit → myp_guard_ulimit）全部接入。
- 新增 Windows 启动器 `tests/run_tests_win.bat`：自动定位 Git Bash/MSYS2
  bash.exe（常见安装路径 + PATH 兜底），切到仓库根调用 run_tests.sh。
- **Linux 全量回归确认**：run_tests 314/314、self 95/95、bootstrap 16/16、
  bugs 12/12，行为零变化（Linux 走 GNU timeout/ulimit 原路径）。
- 前提（Windows 侧）：Git Bash + Windows LLVM 构建 mypc.exe + MinGW gcc 在 PATH；
  mypc 在 Windows 仍按 `-o` 精确命名 `.out`（PE 文件，msys 运行时按魔数可直接执行）。

**`mypc run` 子命令 POSIX 依赖清零（编译器本体可完整编译）**：
- 头文件分平台：Windows 用 `runtime/platform_win_dirent.h`（新，dirent 模拟头，
  从 platform_win.h 提取供编译器共用，避免把 termios/poll/mkdir 宏拖进编译器 TU）
  + `windows.h`/`process.h`/`direct.h`；Linux 保持 `dirent.h`/`sys/wait.h`/`unistd.h`。
- 新增 `mkdirPortable`/`pidPortable`/`tempDir` 跨平台工具：Windows 用 `_mkdir`/
  `_getpid`/`GetTempPathA`（%TEMP%，替换硬编码 `/tmp`；runtime 缓存目录同改）。
- `selfExeDir`：Linux `readlink(/proc/self/exe)` → Windows `GetModuleFileNameA`。
- `nmSymbols`/`nmDynSymbols`：Windows 下 popen 走 cmd.exe，重定向 `2>/dev/null` →
  `2>NUL`（nm 来自 MinGW binutils，须在 PATH）。
- `mypc run` 执行：Windows 用 `CreateProcessA`（同步 + `GetExitCodeProcess` 透传
  退出码，临时二进制显式 `.exe`）替代 `fork/execv/waitpid`；Linux 分支不变。
- 验证：Linux 全量 314/314 + `mypc run` 8/8（含 args 透传、无残留）；
  移植片段用 MinGW 交叉编译链接成 PE（0 error，仅 NOMINMAX 重定义已修）；
  cross-runtime 交叉编译重构后仍通过。
- ⚠️ main.cpp 的 `_WIN32` 分支整体编译验证仍需 Windows LLVM（M5 已知限制）。

**全仓 POSIX 依赖审计（编译器本体之外残留清零）**：
- `src/runtime/runtime_lib.c`（cuBLAS hook）：补 `_WIN32` dlopen→LoadLibrary shim
  （同 runtime_gpu/rocm，M3.5 模式）；库名 `libcublas.so.*`→`cublas64_*.dll`。
  **此文件被 mypc 无条件编译，原裸 `dlfcn.h` 会让 Windows 每个生成程序编译失败**。
- `src/runtime/runtime.c myp_capture_args`（程序 argv 捕获）：Windows 用 CRT
  `__argc`/`__argv`（stdlib.h 已声明，构造期可用）替代 `read(/proc/self/cmdline)`，
  否则 Windows 上 MYP `main(argc, argv)`/`args` 模块的 Argc 恒为 0。
- `src/dap/dap_server.cpp`（myp_debug，DAP↔gdb MI）：fork/pipe/dup2/execlp/poll/
  read/write/usleep 全套 POSIX 编排 → **Windows 暂不构建**（CMake `if(NOT WIN32)`，
  Linux 保留）。Windows 版 DAP 调试（CreateProcess+管道+等待）为后续里程碑。
- `cmake/cross-runtime/`：纳入 runtime_lib.c + runtime_rocm.c（与 gpu 同享 shim
  cross 验证）。cross-runtime 构建 0 warning。
- 审计结论：编译器本体可执行文件（mypc/myp_lsp/myp_fmt/myp_viz）POSIX 依赖已全
  清零；tools/、mypview/ 全为 MYP 源码无原生代码。
- 回归：Linux 314/314 + cross-runtime 交叉编译 0 warning（含新 args/runtime_lib）。

### v3.13.8 — P6 ② 声明式 reduce/scan 块内并行（§8.2/8.3，用户选）
- **reduce 块内并行 halving 树**（§8.6 规范树，`emitBlockSumTreePtx`）：2 的幂块
  大小时 K1 改 ping-pong 共享内存树（每线程 1 元素，末块尾以 init 单位元填充），
  CPU 镜像 `emitSeqBlockTreeReduce` 同树 → **位级一致**（`test_gpu_reduce_bit`
  GPU==CPU==1177075682）；非 2 幂回退串行 K1（纯块和，修正 init 双计 bug）。
- **reduce 表达式形式**（`GpuReduceExpr`）：`float s = @gpu reduce ... over
  a[0..n);` 无 `-> out`，parser 3-token lookahead 区分 lambda，sema 合成
  `__gpu_rdtmp_N` 临时，可嵌套/参与运算（`bench/rdexpr.myp` 双模式 PASS）。
- **scan Hillis-Steele 块内并行**（`emitScanK2HsPtx`）：inclusive + 2 的幂块 →
  K2 改 ping-pong 双缓冲 HS（d∈{1,2,4,…}，kernel 名 `myp_scan_k2_hs`）；launch
  按 `use_hs` 选 kernel 名（否则静默回退 CPU）。
- **scan exclusive 变体**：`@gpu scan(exclusive) ...`（K2 写前落盘 / CPU 写前
  先存）；`test_gpu_scan.myp` 增 exclusive 全量/子区间/非零 init 三 case 双模式 PASS。
- **CPU 回退权衡**：scan 回退统一串行 `emitSeqScan`（HS 位一致镜像
  `emitSeqScanBlocked` 在串行 CPU 上慢 ~10× 不采用）→ GPU/CPU 浮点差几个 ulp
  （容差内）；reduce 位级一致不受影响。
- **性能验证**（`bench/gpu_reduce_scan.myp`，基线 `BASELINE_gpu_reduce_scan.md`）：
  GPU reduce 1M 0.87–0.97→0.67–0.9、scan 1M 1.6–2.0→1.2、reduce 4M 2.9–3.2→2.07、
  scan 4M 5.6–6.4→4.07 ms/op——**全面改善，无性能回退**；CPU 回退持平。
- **坑**：树 kernel `src[tid+half]` 越界（tid≥half 线程）→ 非法内存访问，须
  `select` 钳索引到 tid；scan j-loop 缺 j++ 回边 → partials 只算 block 0；offsets
  循环须重绑 acc/x（步骤 4 恢复后步骤 5 复用未绑定 → "undefined variable acc/x"）。
- 回归 266/266 + AMD 交叉编译 + GPU 双模式（reduce/scan/algo/reduce_bit）PASS。

### v3.13.7 — P6 ② 图内存（CUDA Graph）+ P6 ③ BYOC（§9.7）
  - **图内存**：`stdlib/gpu/graph.myp` 的 `GpuGraph`（captureBegin/captureEnd/
    instantiate）与 `GpuGraphExec`（launch 重放/destroy）。宿主 FFI
    `myp_gpu_graph_capture_begin/end/instantiate/launch/destroy/exec_destroy`
    （`runtime_gpu.c`，dlopen libcuda）。机制：**流捕获**（`cuStreamBeginCapture`
    THREAD_LOCAL → `cuStreamEndCapture` → `cuGraphInstantiate` → `cuGraphLaunch`
    重放）。约束：内核须 `resident()` + `GpuBuffer`（持久 `devicePtr`），捕获段
    只排内核。
  - **关键坑**：`cuGraphInstantiate` 在 MYP 协程上下文段错误（同 cuModuleGetGlobal
    的 TLS 问题）→ 所有图入口先 `cuCtxSetCurrent(ctx)` 强制上下文当前即修复。
  - **BYOC 自定义 PTX**：`stdlib/gpu/byoc.myp` 的 `GpuByoc`（load/launch），宿主
    FFI `myp_gpu_byoc_load/launch`（参数 `long[]`：指针放指针值、标量放数值、
    double 放位型）；启动手写自包含 PTX（`tests/test_gpu_byoc.myp` 的 `dbl`）。
  - **BYOC 厂商库 hook**：`runtime_lib.c`（独立编译，dlopen libcublas 惰性加载，
    缺库回退）暴露 `myp_cublas_available/sgemm` → `GpuLib` 列主序 SGEMM；
    测试与 host 参考误差 <1e-4。
  - 注册：sema `add_intrinsic`/`add_gpu_arr` + codegen `declareRuntimeFunctions`
    intrinsic_map（`__myp_gpu_graph_*`/`__myp_gpu_byoc_*`/`__myp_cublas_*`）。
  - 测试 `test_gpu_graph.myp`（3 resident 内核捕获→重放逐位一致 + 二次重放幂等，
    CPU 模式 no-op）、`test_gpu_byoc.myp`（PTX dbl 全 n 逐位 + cuBLAS SGEMM）。
    回归 266/266 + AMD 交叉编译 + 既有 GPU 测试（algo/printk/query）GPU 模式无回归。
  - 踩坑：range-for（`for k in 0..n`）解析提前 return，**不解析 resident/stream
    子句**——`resident`+`stream` 必须配标准 `for(;;)`；`@gpu for` 内嵌 PTX 手写
    时寄存器声明 `<N>` 含 %r0（用 %r4 须 `<5>`）；ptxas 拒绝 `[reg+reg]` 寻址
    （改用 `add.u64` 合成地址）。

### v3.13.6 — P5 ② kernel 内 printk/assert 调试（§9.6）
  - **`kernel.printk(fmt, v...)` / `kernel.assert(cond, fmt, v...)`**（`@gpu for`
    内核内；fmt 字符串字面量，值参 int/long/double/float ≤3）。
  - **GPU staging**：runtime 分配设备缓冲/计数器（`myp_gpu_printf_buf/cnt/fail`）
    + kernel 末尾 3 个附加 i64 参数传指针（**避开 cuModuleGetGlobal**——协程/
    @thread 上下文下返回 CUDA_ERROR_INVALID_CONTEXT）；kernel 内 atomicrmw 领槽
    （仅 printk/断言失败才领）+ 写 7×i64 记录；launch 后 `myp_gpu_flush_printf`
    回读 mini-printf 打印、清零；assert 失败 exit(1)。
  - **CPU 回退**：宿主 `myp_printf`/`myp_assert_abort`，前缀 `kernel[gid=<循环变量>] `
    与 GPU 一致 → 单格式双模式输出逐字节相同。
  - **格式**：值参类型匹配（int→%d、long→%ld、double→%g）；GPU 按 mask 宽容打印。
    多格式记录顺序受 GPU 线程调度影响（非确定性，同 CUDA printf）。
  - 测试 `test_gpu_printk.myp`（printk+assert 通过，双模式输出 IDENTICAL）、
    `test_gpu_assert_fail.myp`（断言失败双模式 exit 1 + 消息一致）；负测试 3
    （格式非字面量/参数过多/类型不符）。回归 266/266 + AMD 交叉编译无回归。
  - 踩坑：O2 GlobalDCE 删只写不读的模块全局 → 改 runtime 缓冲 + 附加参数；
    cuModuleGetGlobal 协程 201；领槽 atomicrmw 须 gate 在 do_write 内（否则通过
    assert 也消耗槽位）；`@gpu for` 边界按 `<` 处理（`i<=n` GPU 只到 n-1）。

### v3.13.5 — P5 ④ 并行算法库（stdlib/gpu/algo.myp，GpuAlgo）
  - **compact**（流压缩）：keep 的 inclusive 前缀和（§8 scan）→ 目标位置
    pos[i]=off[i]-keep[i]（exclusive）→ 条件写（@gpu for）；返回保留数。
  - **unique**（相邻去重）：change[i]=(i==0 或 a[i]!=a[i-1]) → 对 change 做 compact。
  - **histogram**：ones 数组 + `@gpu scatter(atomic_add)` → hist[idx[i]] += 1
    （整数原子计数位一致；host 预扫越界自保，越界返回 0）。
  - **sort**（原地升序）：确定性 odd-even 转置比较交换网络（每轮偶相+奇相两个
    独立 kernel launch 提供隐式全局同步；n 轮有序；O(n²)，radix/bitonic 留后续）。
  - **双实现位一致**：GPU 与 CPU 回退跑同一算法序列 → 输出逐字节相同。
    `tests/test_gpu_algo.myp`（n=2048：质数 compact / i/8 unique / LCG 16 桶
    histogram / 0..999 重复值 sort）MYP_GPU=0/1 双模式输出 IDENTICAL；AMD
    交叉编译（MYP_GPU_TARGET=amdgcn）二进制 CPU 回退语义 PASS。回归 263/263。
  - 踩坑记录：`@gpu scan` int init 须 `init int(0)`（字面量 0 为 byte）；`@gpu for`
    假设循环从 0 起（非 0 起始在 GPU 下执行 p=0 分支 → 越界读，须体内处理边界）；
    静态方法须类名限定调用。

### v3.13.4 — P4 §9.5 ④ 厂商探测 + 能力查询（§7.4）落地
  - **runtime_gpu.c 厂商探测**：`myp_gpu_vendor()` → "nvidia"/"cpu"（无 GPU），
    `myp_gpu_gfx_arch()` → ""（NV 无此概念）；ROCm 版返回 "amd"。
  - **runtime_gpu.c 能力查询**：`myp_gpu_shared_per_block/regs_per_block/
    max_grid_dim/max_block_dim/clock_mhz/concurrent_kernels/mem_alignment/
    double_precision/atomics64`，统一 `cuDeviceGetAttribute`（属性 ID 对齐
    /usr/include/cuda.h：MAX_BLOCK_DIM_X=2、MAX_GRID_DIM_X=5、MAX_SHARED_MEMORY_
    PER_BLOCK=8、MAX_REGISTERS_PER_BLOCK=12、CLOCK_RATE=13、CONCURRENT_KERNELS=31）。
  - **runtime_rocm.c HIP 镜像**：`hipDeviceGetAttribute` 同 ABI 补全（vendor="amd"）。
  - **codegen/sema**：注册 vendor-neutral intrinsic `__myp_gpu_vendor/gfx_arch/
    shared_per_block/regs_per_block/max_grid_dim/max_block_dim/clock_mhz/
    concurrent_kernels/mem_alignment/double_precision/atomics64`。
  - **stdlib/gpu/device.myp**：`GpuDevice` 补齐 §7.4 全部字段（vendor/gfxArch/
    sharedPerBlock/regsPerBlock/maxGridDim/maxBlockDim/clock/concurrentKernels/
    memAlignment/doublePrecision/atomics64）；`GpuHAL.vendor()` 改真实设备探测。
  - **实测 RTX 2070 SUPER**（`test_gpu_query.myp` 双模式 PASS）：vendor=nvidia、
    capability=705、sharedPerBlock=49152、regsPerBlock=65536、maxGridDim=2147483647、
    maxBlockDim=1024、clock=1815MHz、concurrent=1、atomics64=1（sm_60+）、
    doublePrecision=0（sm_75 消费卡 FP64 1/32）。CPU 回退 vendor=cpu、能力全 0。
  - 回归 263/263 + AMD 交叉编译（tests/cross_compile_amd.sh）无回归。

### v3.13.3 — P4 跨厂商（AMD）编译期落地（无 AMD 硬件，交叉编译验证）
  - **§9.5/§6.4 AMDGPU 后端 + GCN 交叉编译**：
    - `MYP_GPU_TARGET=amdgcn` 让 `@gpu for`/reduce/scan 内核编译期发射
      `amdgcn-amd-amdhsa` GCN ELF code object（`ObjectFile` 直接出 ELF，EM_AMDGPU，
      含 `myp_kernel` 符号），写 `MYP_GPU_EMIT_FILE`（默认 /tmp/myp_kernel.gcn）；
    - codegen 参数化：双后端 init（`ensureGpuTargetsInited`）、kernel CC
      （NV PTX_Kernel / AMD AMDGPU_KERNEL）、线程索引（NVVM sreg ↔ AMDGCN
      workitem/workgroup.id.x）、kernel alloca addrspace(5)（AMD private）、
      blockDim = launch 常量、`kernel.sync()`（bar.sync 0 ↔ s_barrier）、
      O2 管线消解 AMDGCN 无法选中的构造（声明式 kernel）。
  - **§9.5 ③ runtime_rocm.c 骨架**（`-DMYP_ENABLE_ROCM=ON`）：dlopen
    libamdhip64 + HIP 函数指针镜像 myp_gpu_* ABI（hipModuleLoadData/hipLaunchKernel/
    hipMemcpy/stream/event），无 ROCm 不构建。
  - **§9.5 ⑤ 交叉编译验证** `tests/cross_compile_amd.sh`（无硬件）：GCN ELF
    magic + kernel 符号（llvm-readobj）+ AMD 二进制 CPU 回退语义 + NV PTX 无回归。
  - **受限（无硬件留待）**：`@gpu stride`/scatter（gridDim 无 AMDGCN intrinsic）
    AMD 回退 CPU；`@gpu tile` __shared__ 对象发射；数学 NV libdevice（AMD 走
    LLVM intrinsic/ocml）。
  - CMake：AMDGPU LLVM 组件；`MYP_ENABLE_ROCM` 选项。回归 263/263。

### v3.13.2 — LSP 语义高亮（semantic tokens）：MYP 文件通过 LSP 有语法颜色

**背景**：vscode-myp 扩展的 LSP（`myp_lsp`）只提供诊断/补全/hover/文档符号，
未实现 `semanticTokensProvider` —— MYP 文件在 VS Code 中只有 TextMate 基础
高亮（关键字/类型/注释），函数名、方法、变量、属性等大量标识符是默认色，
用户反馈「LSP 这块语法没有颜色和高亮」。

**实现**（`src/lsp/lsp_server.cpp`）：
- capabilities 声明 `semanticTokensProvider`（legend 19 类型：comment/keyword/
  string/number/type/class/struct/interface/enum/function/method/property/
  variable/parameter/operator/namespace/annotation/boolean/macro，`full:true`）。
- 新增 `textDocument/semanticTokens/full` handler：注释扫描（正确跳过字符串/
  字符字面量内的 `//` 与 `/* */`）+ lexer tokenize + 标识符语义分类：
  - 关键字/类型/数字/字符串/操作符/注解 → 按 TokenKind 直接映射；
  - 标识符：`.xxx(` → method、`.xxx` → property、`@xxx` → annotation、
    大写开头 → type（类/struct/enum/interface 名）、`xxx(` → function、
    `int x`/`Foo bar`（前一个是类型）→ variable。
- UTF-16 偏移 delta 编码（`utf16CodeUnits` 处理多字节源文件）。
- **lexer 坑**：单字符 operator 的 range 是 `[offset,offset)`（`currentRange()`
  在 `advance()` 之后返回，len=0 且位置指向下一字符）→ 语义编码跳过空 token，
  操作符颜色交给 TextMate（tmLanguage 已有 operators 规则）。
- 扩展端无需改动（vscode-languageclient 自动请求 semantic tokens）。

**验证**：`textDocument/semanticTokens/full` 对 `demo_model.myp` 返回 296 个
语义 token（comment 10 / keyword 8 / string 4 / number 92 / type 23 /
function 3 / method 46 / variable 110）；`tests/test_lsp.js` **14/14 PASS**。
使用：VS Code 里 `MYP: Restart Language Server`（或重载窗口）让扩展加载新
`myp_lsp`。

### v3.13.1 — 编译器 DX：漏参/窄化诊断 + GPU fast-math + 自举 UTF-8 修复（95/95）

**1. 参数缺失报错改进**（`src/sema/sema_expr.cpp` + `tools/selfhost/src/sema.myp`）
- 漏参数时一次性列出**全部**缺失形参（而非只报尾部一个），并附期望/实得数量：
  `missing required argument(s) 'batch' 'dev' — expected 10 arguments, got 8`。
- 修复场景：GPU 算子调用漏 batch 时之前只报 `'dev'`（误导性极强），现能看出是
  中间漏参导致后续错位。C++ 报错文本已同步 selfhost（sema.myp）。

**2. 参数窄化诊断（narrowing warning）**（`include/mylang/Sema.h` +
`src/sema/sema_expr.cpp` + `tools/selfhost/src/sema.myp`）
- 新增 `Sema::isNarrowing`：实参→形参「兼容但有损窄化」时发 warning——
  `argument 1: passing 'long' to parameter of type 'int' is a narrowing
  conversion; ... use convert<int>(...) to make it explicit`。
- **为什么只有 Long→Int 命中**：MYP 数字隐式只允许 Int↔Long 双向 + 同符号拓宽 +
  f32→f64；`double→float`、`double→int`、`long→short` 等本就在 `typesCompatible`
  层报 error（`expected 'float', got 'double'`）。故「兼容但窄化」实际只有
  Long→Int 一种——正是漏参/换位时最常见的错位源（如 GPU dense 的 `dev`/`yOff`）。
- **warning 而非 error**：现有代码大量合法 long→int（ARC 计数断言
  `Test.assertEq(long,0,..)`、`Fmt.i(long ms)`），error 会破坏回归。
- **对拍约束**：`--frontend-dump sema` 输出含 Diagnostics 段，任何新诊断必须
  C++/selfhost **逐字节一致**（selfhost 无 param_names 元数据 → 诊断文本不带
  形参名）。新增对拍语料 `tests/narrow_sema.myp`（3 warning + 1 error，两编译器
  dump 字节一致）。

**3. GPU fast-math（opt-in）**（`src/codegen/codegen_gpu.cpp`）
- `MYP_FAST_MATH=1` 时给 GPU 内核 IRBuilder 打 fast flags（reassoc/contract/nnan/
  ninf/nsz/arcp），默认 OFF（不改变现有数值行为）。
- 实测 Qwen2 batch=4 LLM 反而变慢（21 vs 16 ms/step）——LLM decode 带宽受限，
  计算优化无益；保留 opt-in 供纯计算型内核（卷积/归约）使用。

**4. 自举 UTF-8 修复（Dump.esc，区别于 v3.12.48 词法器 BUG-039）**
  （`tools/selfhost/src/ast.myp`）
- `Dump.esc` 用 `__myp_charcode`+`__myp_chr` 逐字符 → 字节被当码点再 UTF-8 编码
  （双重编码，`"多参"`→`"å¤å"`）。改用 `Str.substring(s,i,i+1)` **原样字节透传**
  （与 C++ `escapeDumpString` 逐字节一致）。lexer 层本就正确（tokens 对拍已 PASS），
  无需改动。
- 自举 sema 对拍 **92/94 → 94/94**（+本次窄化语料 = **95/95**）。

**回归**：全量 `tests/run_tests.sh` **314/314**；`tests/test_myp_self.sh` **95/95**。

### v3.13.0 — 自举编译器 GPU 真机加速：resident 直传 + Device./Atomic 映射（BUG-045）

> 编译器版本序列自本条目起与 mypview 分离：**编译器/自举/运行时版本独立计数**
> （主 changelog 最后编译器条目 v3.12.57；v3.12.58–69 为 mypview 框架独占里程碑，
> 记于 `mypview/CHANGELOG.md`）。本文件后续编译器条目从 v3.13.0 起单独递增。

**背景**：自举编译器（`tools/selfhost/src/codegen.myp`）编译 deeplearning 框架时，
GPU kernel 对 `resident` 一律回退 CPU、`Device.*`/`Atomic.*` 设备调用发 undefined
符号（llc 拒 → 全量 GPU CPU 回退）。本次补齐，使自举产物在真机（RTX 2070 SUPER，
CUDA 13.2，`MYP_GPU=1`）真正发射 kernel。

**修复内容**（全部在 `tools/selfhost/src/codegen.myp`）：
- **resident 设备驻留直传**：`genGpuKernel`/`genGpuTileKernel` 不再拒绝 resident
  数组；新增 `gpuResidentDev(st, arr)` 从 `st.resident()` 解析 devVar，设备指针用
  `inttoptr(load devVar)`，跳过 H2D/D2H/free。
- **`gpuKernelMode_` 标志**：kernel 模块 body 生成期间置 1，区分 NVPTX kernel 与
  CPU 回退串行 codegen。
- **`genKernelDeviceCall(e, rfn)`**（对应 C++ oracle 的 `emitKernelExpr` 特判）：
  - `Device.sqrt/exp/log/sin/cos/pow/abs/floor/ceil/trunc` → `llvm.*` intrinsic
    （f32/f64 仅 intrinsic 名后缀，LLVM 类型用 float/double）；
  - `Atomic.addDouble/addFloat` → `atomicrmw fadd`、`addInt/addLong` →
    `atomicrmw add`（`getelementptr <elemT>, ptr, i64 idx`）。
- 两处拦截：resolved 分支 + member 调用路径（后者处理不走 `e.resolved()` 的
  `Device_*`/`Atomic_*` 静态调用）。
- 两个 kernel 模块前言（`emitGpuKernelModule`/`emitGpuTileModule`）补
  `llvm.exp/log/sin/cos/pow.f32/f64` intrinsic 声明。

**效果**：
- kernel 未定义符号 **21 → 1**（仅剩未用的 `Device_tan`——LLVM 无 tan intrinsic，
  回退 CPU 可接受）。
- GPU 重模型真发射且结果正确：`conv3d_main`/`coarselike`/`coarselike32`/
  `pad3d_avgpool3d` 全「GPU OK」（nonzero-grid launch 7/21/21/3 次）。
- r18 的 40 个 kernel 全 `grid=0 → cuLaunchKernel failed` 为**框架层预先存在 bug**
  （oracle 的 r18 GPU 同样 40 个 grid=0，行为一致），非编译器问题。

**BUG-045（🟩）——自举 `@parallel for` 数组参数元素类型缺失**：`genParallelFor`
捕获参数传 `elem=""` → 并行体内 `float[] arena` 被当 `i32[]`（GEP/store 用 i32）→
3D Conv3D/MaxPool3D/AveragePool3D 输出全 0。修复：参数捕获按 `paramAstTypes_`
取元素类型（slice→`sliceElemType`+slice 标记，数组→`llvmType(element())`），与
`varElemType`/`isSliceVar` 对齐。回归：`tests/bugs/b045_parallel_float_array.myp`
（4 断言双编译器通过）；infer_tests 18 入口自举与 oracle 一致。

**回归**：CPU 11 入口 0 mismatch；`tests/test_myp_self.sh` PASS=92/2（2 个 sema
对拍失败为预先存在：`mega/test.myp`、`@test/function.myp`）；`run_bugs.sh`
12 green 0 red。

### v3.12.57 — 再剥离 6 组 FFI 出 runtime（net/process/regex/base64/date/hash）

**背景**：继 JSON 之后，继续审计 `src/runtime/runtime.c`——凡是「只被单个 stdlib
模块引用、不被核心运行时依赖」的函数组都移出为独立 bridge（按需链接）。

**剥离 6 组**（runtime.c 5949→5436，-513 行）：
| 组 | bridge | 对应 stdlib | 行 |
|---|---|---|---|
| `myp_net_*` | `net_bridge.c` | net | 106 |
| `myp_process_*` | `process_bridge.c` | process | 63 |
| `myp_regex_*` | `regex_bridge.c` | regex | 31 |
| `myp_base64_*` | `base64_bridge.c` | base64 | 63 |
| `myp_date_*` | `date_bridge.c` | date | 45 |
| `myp_hash_*` (md5/sha) | `hash_bridge.c` | crypto | 205 |
- 每个 bridge 头统一 `#include "mylang/runtime.h"`（`myp_alloc`）+ 标准头；
  process 补 `<unistd.h>`（getpid/kill）。
- 修复一处剥离边界：base64 段后无分隔线直接接 alloc 核心段，初版误吞
  `myp_alloc` 核心 → 硬编码段边界重剥（base64 只到 decode 结束）。
- 仅用到这些模块的程序才链接对应 bridge（同 sdl/ttf/json）。

**保留核心基座**：ARC（alloc/retain/release）、GC pool、字符串、协程/事件、
线程/并行原语、类型转换、异常、断言等——被编译器生成代码/语言特性直接依赖。

**验证**：综合测试 `import regex/base64/date/crypto/process` 编译运行全对
（`re=1 b64=aGk= md5=900150... pid=...`，bridge 按需链接）；parent 313/313、
bootstrap 16/16、bugs 11/11、mypview UIX/BNCT/PIPE 全 PASS。
**后续候选**：`myp_fs_*`（fs，独立但高频用）、`myp_math_*`（编译器 intrinsic，
需专项验证）暂留 runtime。

### v3.12.56 — JSON 从 runtime.c 分离为独立 bridge（按需链接）

**背景**：JSON 解析/查询 FFI 常驻 `src/runtime/runtime.c`（约 300 行），每个
程序（无论用不用 JSON）都编译进运行时。JSON 仅在 `import json;` 时才有意义，
不应是运行时基座。

**分离**（对齐 sdl/ttf bridge 机制）：
- JSON 段从 `src/runtime/runtime.c` 移出（-299 行，6247→5948），新建
  `stdlib/bridges/json_bridge.c`（含 `myp_json_parse/get_type/get_string/
  get_number/get_bool/array_length/free` + 递归下降解析器）。
- 依赖 `myp_strdup`（runtime.h 声明，runtime 链接提供）。
- `stdlib/json.myp` 的 ffi 声明不变（符号名 `myp_json_*` 由 bridge 按需提供）。
- 效果：只有 `import json;`（调用 `myp_json_*`）的程序才链接 json_bridge.o，
  不用 JSON 的程序运行时更小。

**验证**：`import json` 程序编译链接运行正常
（`json a=1 name=hi ok=1 arrlen=3`，`nm` 确认 json_bridge 按需链接）；
parent 313/313、bootstrap 16/16、bugs 11/11、mypview UIX/BNCT/PIPE 全 PASS。

### v3.12.55 — mypview 新增 Card 控件 + BNCT 病例管理页示例

**Card 控件**（`mypview/src/controls/card.myp`，第 53 个控件）：圆角信息卡片——
标题/副标题/元信息 + 底部「详情/进入」双操作按钮，hover 提亮、选中 accent 描边。
事件：`Clicked`（整卡）/ `PrimaryAction` / `SecondaryAction`；声明式属性
title/subtitle/meta/primary/secondary/color/accent。适配 Web dashboard 卡片
（对齐 Theme 深色表面 + accent）。

**BNCT 病例管理页示例**（`mypview/examples/bnct_cases.myp`）：复刻 BNCT 治疗
计划系统病例管理页结构——Header（系统标题/页面标题/用户区）+ SearchBar +
排序/来源 Dropdown + 操作按钮行 + 病例/序列 Dropdown +「进入勾画」+ Card 卡片
网格（搜索过滤联动 + 卡片详情/进入事件）。**双模式**：
- **默认 SDL 窗口界面**（`bash build.sh bnct_cases`）：1100×700 dark blue 窗口，
  卡片渲染 + hover 提亮 + 点击事件（进入/详情）+ 键盘搜索实时过滤，ESC/关窗退出；
  帧循环已验证（`bnct ui running cards=2` → `done frame=5`）。
- **`BNCT_HEADLESS=1` headless 逻辑验证**：
```
bnct header=BNCT 治疗计划系统 / 病例管理 / Doctor demo
bnct cards=2 first=Patient-7736D7 (ID-C4A0)
bnct search '305891' shown=1        ← 搜索过滤
bnct enter=Patient-7736D7 (ID-C4A0) ← 点卡片「进入」
bnct detail=Patient-305891          ← 点卡片「详情」
```
`build.sh` 对 `bnct_cases` 自动加入 `backend/sdl_renderer.myp`（同 player）；both
对比模式走 headless。实现中修复：数组元素传接口参数（`RootView.add(cards_[i])`）
不自动接口上转 → 局部变量中转（同 BUG-041 类别）。

**测试**：`mypview/tests/run.sh` 两个 SRCS 列表加 `card.myp`，BNCT 节编译含
`backend/sdl_renderer.myp`，新增 `MYPVIEW-BNCT PASS`（7 断言，`BNCT_HEADLESS=1`）。
mypc 与 myp_self both 输出一致；parent 313/313、bootstrap 16/16、bugs 11/11、
mypview UIX/BNCT/PIPE 全 PASS。

### v3.12.54 — MYP 源码闭源：签名声明（无 body 方法→外部声明）+ 预编译库链接

**背景**：v3.12.53 打通了「C 下沉 + bridge 预编译 .so」闭源。但核心算法若本身
用 MYP 写（而非 C），`import` 是源码合并、分发必含 `.myp` 源码。本版本让
**MYP 源码也能闭源**：实现编译成 `.so`（`mypc --shared` 已有），分发「签名
`.myp`（只声明无 body）+ `.so`」。

**修复**（`src/codegen/codegen_class.cpp`）：类方法生成器（`generateClassAction`/
`generateStaticAction`/`generateClassFunction`）对**无 body 方法**不再生成
`ret 默认值` 的 internal stub，而是保持**外部声明**（ExternalLinkage、无 body）
——调用方生成对 `Class_method` 的 undefined 引用，链接器经 `MYP_BRIDGES`
从预编译 `.so`/`.a` 解析（配合 v3.12.53 的 bridge 预编译库支持）。
`markNonMainFunctionsInternal` 已跳过声明（`isDeclaration()`），无冲突。

**闭源分发闭环（MYP 源码）**：
- 实现 `secret.myp`（class Secret { static: mul/verify/greet }）→
  `mypc secret.myp --shared -o libsecret.so`（导出 `Secret_mul` 等符号）
- 分发：签名 `sig.myp`（`static: int mul(int a,int b);` 无 body）+ `libsecret.so`
- 用户：`import "./sig.myp";` 调 `Secret.mul(...)`，编译时
  `MYP_BRIDGES=<含 libsecret.so 目录>` → 自动链接
- 运行：`mul=24` / `verify=1` / `greet=hi Bob`（含字符串返回与 ARC，均正常）

**测试**：`tests/test_closed_lib.sh` 扩展第 7 节（MYP 源码闭源，6 断言），现
12 断言全过；parent 313/313、bugs 11/11、bootstrap 16/16、mypview UIX/PIPE PASS。

### v3.12.53 — bridge 机制支持预编译库（.so/.a）→ MYP 闭源分发

**背景**：MYP 的 import 是「源码合并」模型，包分发必然含 `.myp` 源码，无法闭源。
借鉴 Java（字节码 jar + 混淆 + JNI 下沉）/C（头文件 + 预编译库）模式，打通
「预编译库 + FFI 封装」闭源路径。

**实现**（`src/main.cpp` `linkObjects` bridge 逻辑）：
- bridge 发现扩展：`MYP_BRIDGES` 目录除 `.c` 外，也收集预编译库 `.so`/`.a`
  （新增 `listLibFiles`）。
- 预编译库按「用户程序未定义符号 ∩ 库已定义符号」固定点匹配直接链接，无需
  `.c` 源码（`selected_libs` + `bridge_obj_list` 直接带库路径）。
- `.so` 用动态符号表匹配（新增 `nmDynSymbols`，`nm -D`——strip 过的共享库也能
  读到导出符号）；`.a` 用普通 `nm`。
- 现有 `.c` bridge（sdl/ttf 等）逻辑不动，回归全绿。

**闭源分发闭环（端到端验证）**：
- 核心算法 `secret.c` → `gcc -shared -fPIC` → `secret.so`（分发物，不含 `.c`）
- 封装 `api.myp`：只 `ffi` 声明 + `static:` 薄封装类（核心实现不可见）
- 包化：`package.myp` + `src/secretpkg.myp` + `lib/secret.so`，`myp install` 后
  用户 `import secretpkg;`，编译时 `MYP_BRIDGES=<含 secret.so 的目录>` 自动链接
- 运行：`pkg mul(5,6)=60` / `verify=1`（逻辑来自 .so）；`.so` 与 `.a` 两种形态均可

**测试**：新增 `tests/test_closed_lib.sh`（6 断言：.so 链接/核心输出/闭源无 .c/
.a 链接）接入 `run_tests.sh`；parent 313/313、bugs 11/11、bootstrap 16/16、
mypview UIX/PIPE PASS。

### v3.12.52 — 点分模块名导入增强（a.b.c → 包内 src/ 子路径）+ mypview 子目录聚合

**点分导入（对齐 `gpu.hal` 惯例）**：编译器 import 解析对点分模块名
`a.b.c` 追加「首段=包名」解析——在现有 `<pkg>/a/b/c/src/a.b.c.myp` 形态未命中
后，尝试 `<pkg>/a/src/b/c.myp` 与 `<pkg>/a/b/c.myp`（向后兼容）。
- mypc：`src/main.cpp` `loadModule`；myp_self：`tools/selfhost/src/main.myp`
  `loadModule`（两编译器同步，bootstrap 16/16 不动点）。
- 效果：`import mypview.controls.app_icon;` → `mypview/src/controls/app_icon.myp`。

**mypview 子目录聚合**：新增 `src/core.myp` / `src/controls.myp` /
`src/layout.myp` / `src/uix.myp` / `src/animation.myp`（相对路径 import 聚合各
目录；controls/layout/uix/animation 内部 `import "./core.myp"` 自包含，
uix 额外依赖 controls+layout）。点分粒度导入：
- `import mypview.core;`（核心 6 文件）/ `import mypview.controls;`（49 控件）
  / `import mypview.layout;` / `import mypview.uix;` / `import mypview.animation;`
- 单文件：`import mypview.controls.app_icon;`（需补 `import mypview.core;`）

**验证**：mypc + myp_self 对 `import mypview.core + mypview.controls.app_icon`、
`import mypview.controls`（AppIcon/Button/NumberInput）、`import mypview.uix`
（UixLoader）编译运行输出一致；parent 312/312、bugs 11/11、bootstrap 16/16、
mypview UIX/PIPE PASS。

### v3.12.51 — mypview 打包为 MYP 标准包（`import mypview;`）+ BUG-044 修复

**mypview 包化**：mypview 从「源码集合」升级为 MYP 标准包——
- 新增 `mypview/package.myp`（`name: mypview, version: 1.0.0`）。
- 新增聚合主模块 `mypview/src/mypview.myp`：用相对路径 import 递归聚合
  src/ 下全部 51 个非 SDL 源文件（core → controls → layout → uix → animation）。
  借助 `loadModule` 的递归子 import 机制，`import mypview;` 一次合并整个框架，
  与「多文件编译」语义完全等价；`src/backend/sdl_renderer.myp`（需 SDL/ttf）
  默认不引入。
- 全链路验证：`myp install <mypview>` → `myp_packages/mypview/` → 消费者
  `import mypview;` + `mypc/myp_self --package-path` 编译运行输出一致
  （`pkg button=Login label=hello num=50 slider=60`）。README 增「方式二：
  作为 MYP 包」，路线图「包化分发」勾选。

**BUG-044（mypc 接口默认实现 stub 用残留返回类型生成 myp_retain(i32)）**
- 打包验证暴露：`import mypview;` 编译时 LLVM verify 失败
  `call void @myp_retain(i32 1)`；同一批文件直接多文件编译正常——类 codegen
  顺序不同暴露。
- 根因：`CodeGen::generateClassDefaultAction`（生成 `__ifdef_View_<m>_<C>` 接口
  默认实现 stub）**未设置 `current_ret_ti_`**，return 语句用上一函数残留类型判断
  是否 ARC retain。
- 修复：补设 `current_ret_ti_`，并与 generateClassAction 对齐
  `arc_skip_retain_return_`/`arc_pending_temps_`。
- 验证：parent 312/312、bugs 11/11、mypview UIX/PIPE PASS；双编译器 import 包
  消费者输出一致。

### v3.12.50 — 文件顺序敏感修复（BUG-041）+ 自举内部符号改 internal（BUG-042）

**背景**：对比 mypc 与自举编译器编译的 mypview 二进制时发现：
- 同样的 55 个文件，仅**顺序不同**（run.sh 手工顺序 vs 字母序），mypc 编译结果
  一个 69 行全绿、一个运行段错误（ConstraintLayout 对象悬垂被字符串覆盖）。
- 自举 myp_self 二进制比 mypc 多导出 ~90 个全局函数（`__myp_destroy_*`/
  `__myp_coro_*` 应为 internal）。

**BUG-041 — 方法调用 callee 选择 fallback 按类序 → 同名方法错调（已根治）**
- 根本原因（src/codegen/codegen_expr.cpp）：`cols_[i].layout()`（UixLoader
  `layoutAll()` 遍历 `LinearLayout[]` **类属性数组**元素）的方法解析用
  `memberObjectClassName`（查 `array_elem_class_map_`，该 map **只记录局部变量
  数组**）→ 类属性数组缺失 → **fallback 按类注册顺序找第一个同名 `layout`
  方法** → 字母序下 ConstraintLayout 先注册 → 错调 `ConstraintLayout_layout`
  → 对象 ARC/字段错乱崩溃。run.sh 顺序（linear 在 constraint 前）碰巧正确。
- 修复：callee 选择两处（主选 + fallback）优先用 sema 的
  `ma.resolved_object_class`（静态元素类型，**与文件顺序无关**），为空才回退
  `memberObjectClassName`。字母序 mypc 编译运行 uix_logic 69 行（原崩溃）。
- 验证：字母序全量 69 行；父级 312/312、bugs 11/11、bootstrap 16/16、
  mypview UIX/PIPE PASS。

**BUG-041b — myp_self 对「绝对路径源码 + 相对 target」混合路径丢 main**
- 根因：myp_self 对绝对/相对混合路径的 target 处理错，未生成/链接 main。
- 修复（mypview/examples/build.sh）：源码/标准库一律用相对路径。
- 验证：myp_self 编译 player/counter 全绿。

**BUG-042 — selfhost 内部析构/协程入口生成为 global 符号**
- 修复（tools/selfhost/src/codegen.myp）：`__myp_destroy_*`（10462）、
  `__myp_coro_entry_*` 方法/函数入口（2894/2922）三处改为 `define internal`。
- 效果：全局函数 T 符号 130 → 40（与 mypc 的 42 几乎对齐）；`.dynsym` 不变
  （58 个，不影响动态链接）；strip 无影响。
- 验证：mypview 69 行输出与修复前一致；bootstrap 16/16、父级全过、bugs 11/11、
  mypview UIX/PIPE PASS。

### v3.12.49 — 真实 SDL 绘制示例 player 自举运行全绿：接口局部变量借用 retain（BUG-040）

用 `myp_self` 编译 mypview 真实窗口示例 `examples/player.myp`（SDL 大窗口 +
全控件 + 帧循环）——**编译 + 运行成功**（120 帧全绿：`frame=120 list=8 q=低
vol=5`、ttf-cache hits=6310）。此前 uix_logic headless 测试 `.draw()` 调用为 0，
真实绘制路径的接口数组遍历 ARC 缺口在此暴露。

**BUG-040 — 接口局部变量初始化借用 fat 不 retain → draw 崩溃**
- 症状：`LinearLayout_draw` 里 `View kid = kids_[i]` 后 `kid.draw(r)` 段错误
  （`call *0x30(%rbx)` 中 rbx=0，或 this 对象 kids_ 字段 = 垃圾）。
- 根因（codegen.myp 局部接口变量初始化）：`View kid = kids_[i]`（接口数组元素
  **借用 fat**）走 `it=="{ptr,ptr}"` 分支直接 store **不 retain**；局部接口变量
  是 arcSlot，作用域末 `releaseArcSlots` 释放 data → **释放借用** → kids_ 悬垂。
  `buildIfaceFat` 分支（借用具体类实例）同样缺 retain。
- 修复：局部接口变量初始化两分支对**借用**（`isFreshTemp==0`）store 前
  `extractvalue 0 + myp_retain(data)`（局部持有，作用域末释放配对）；fresh（new）
  保持 `consumeTemp` 转移。
- **附带修复**：`mypview/examples/build.sh` 固定文件列表过时（缺
  sortable_list/long_press_button/gesture/theme/dialog 等）→ 改用目录通配符，
  player 编译通过（mypc 与 myp_self 均验证）。
- **验证**：player 120 帧全绿；bootstrap 16/16、父级 312/312、bugs 11/11、
  mypview UIX/PIPE PASS。
- **教训**：接口 fat「借用 vs fresh」判定须贯穿所有路径（局部接口变量/接口数组
  store/方法参数返回/赋值 RHS）。凡借用 fat 被 ARC 槽持有，作用域末 release 前
  必须先 retain；只有 new/fresh 才转移。headless 测试不 draw 会漏真实绘制 ARC 缺口。

### v3.12.48 — mypview 全集自举编译运行全绿：接口数组元素 ARC（BUG-037/038）+ UTF-8 双重编码（BUG-039）

用 `myp_self`（自举编译器）编译 mypview 全集（src/core+controls+layout+uix+animation +
uix_logic 测试，63 文件）——**编译通过 + 运行全绿**（69 行输出与 mypc 参考一致，含中文）。
连同上文 v3.12.47 之前的修复，自举接口 fat pointer 缺口全部补齐。

**BUG-037 — 接口数组元素 = 具体类 `new` 缺 fat 上转 + fresh 转移**
- 症状：`nodes_[i] = new Label(...)`（`View[]` 接口数组元素 = 具体类 new）时 LLVM
  报 `'%t55' defined with type 'ptr' but expected '{ ptr, ptr }'`（编译期）；修类型
  错误后运行期对象提前释放（缺 fresh 转移）。
- 根因（`tools/selfhost/src/codegen.myp` Subscript 左值分支）：`elemLt ==
  "{ ptr, ptr }"`（接口数组元素）走 else 直接 `store {ptr,ptr} rv`，未像 Member
  分支那样 `upcastIface/buildIfaceFat` 把具体类裸 ptr 上转为 `{data, vtable}`；
  上转后也未消费 `new` 的 fresh temp → 语句末 `flushTemps` 双重释放。
- 修复：新增 `subscriptElemIfaceName(arr)`（数组元素接口名解析，镜像
  `subscriptElemLt` 形态）；Subscript 分支 `elemLt=="{ptr,ptr}" && rt=="ptr"`
  → `buildIfaceFat` + `isFreshTemp(rawV)!=0 → consumeTemp(rawV)`（对齐局部接口
  变量初始化的 `arcConsumeTemp`）。

**BUG-038 — 接口数组元素 store 借用 fat 不 retain → 对象悬垂、内存被字符串复用**
- 症状：myp_self 编译的 mypview 运行段错误 139。gdb 逐层定位：sync 的
  `setAttr` 接口调用参数全部正确（this/name/val），但 this 对象 `text_` 槽 =
  `0x65646f6e20786975`（"uix node" 字符串内容）；watchpoint 捕获对象被
  `myp_strcat`（walkParents 拼接 "children"）写入 → **对象内存被字符串拼接复用**。
- 根因：build 里 `Label l = new Label(...)`（rc=1 局部），`registerNode` 的
  `nodes_[nodeCount_] = v`（v 为接口 fat **借用**）→ Subscript 分支对
  `rt=="{ptr,ptr}"` 直接 store **不 retain**；build 返回时 `l` 作用域末释放 →
  rc 0 → 对象释放 → nodes_ 数组悬垂 → 内存被 `myp_alloc/myp_strcat` 重新分配。
- 修复（codegen.myp Subscript 分支新增 else-if）：`elemLt=="{ptr,ptr}" &&
  rt=="{ptr,ptr}"`（接口 fat 借用）→ `extractvalue 0` + `myp_retain(data)`
  （数组槽持有借用 fat）。
- 教训：**接口数组元素 store 的引用语义**——具体类 `new` → fat 上转 + fresh 转移
  （数组接管 rc=1）；借用 fat（局部/参数）→ 必须 `retain`（数组持有），否则调用方
  作用域末释放 → 悬垂。与 C++ oracle 的接口数组 store 语义必须一致。

**BUG-039 — 词法器 UTF-8 双重编码（中文乱码）※重要，后续编码处理必读**
- 症状：`myp_self` 编译含中文字符串字面量的程序，输出乱码（`你好` → `ä½ å¥½`），
  `Str.len` 返回字节数翻倍（6 → 12）；IR 里常量变 `c"\C3\A4\C2\BD\C2\A0..."`
  （9 字节，源码是 6 字节 `E4 BD A0 E5 A5 BD`）。`mypc`（C++ oracle）正常。
- 根因（`tools/selfhost/src/lexer.myp` `scanString`）：非转义字符
  `val.append(__myp_chr(advance()))` —— `advance()` 返回源码**原始字节**
  （UTF-8 多字节的一部分，如 `0xE4`），而 `__myp_chr` 按 **Unicode 码点**生成
  UTF-8（`0xE4` 是 `ä` 的码点 → 编码成 2 字节 `C3 A4`）→ **双重编码**。
- 修复：`scanString` 对 `>=128` 的源码字节改用 `Str.substring(source_,
  pos_-1, pos_)` **原样保留字节**；`<128`（ASCII）仍走 `__myp_chr`。
- **UTF-8 处理铁律（避免以后再犯）**：
  1. 源码文件是 UTF-8，词法器按**字节**读（`__myp_charcode` O(1)）。多字节字符的
     每个字节单独出现，`>=128` 的字节**不是**字符码，只是 UTF-8 序列的一部分。
  2. `__myp_chr(code)` 按**码点**生成 1–4 字节 UTF-8——两者只对 ASCII（`<128`）
     等价；把源码字节直接喂 `__myp_chr` 会双重编码。
  3. **字节级 round-trip 一律用 substring/memcpy 原样保留**（同
     `stdlib/io.myp readAll` 注释：不能用 `__myp_chr(b)` 逐字节拼）。
  4. 字符串长度：`Str.len` 是字节数（strlen）。源码字面量需逐字节复制；含中文时
     按字节 len 与 `mypc` 参考一致才正确（如 `你好` = 6 字节 = mypc `n=6`）。
- 影响面：所有 `myp_self` 编译的含中文/非 ASCII 字符串字面量程序。ASCII 不受影响
  （bootstrap 固定点不破）。

**验证**：mypview 全集 `myp_self` 编译运行 69 行全绿（`ok=7aff`、`sync
status=ok-login`、中文 `t=你好 n=6` 与 mypc 一致）；bootstrap **16/16**
（固定点 `myp_self2==myp_self3`）；父级 **312/312**；bugs **11/11**；mypview
官方 UIX/PIPE PASS。BUGLIST 已登记 BUG-036（🟩 完结）+ 新增 BUG-037/038/039（🟩）。

### v3.12.47 — 修复 BUG-031：跨线程多 @thread 目标事件无限重投（广播可用）

**BUG-031 根因**：mapping handler 注册 `instance=NULL` → `myp_event_dispatch` 第一遍
按 handler 归属 route 副本（`routed=1`）后，第二遍跑**所有**同 event 的 NULL-instance
handler（无归属 → 当前线程跑）——副本在目标线程把**其他目标**的 handler 也跑了，每个
handler 内 BUG-005 的 `myp_thread_is_current(inst)==0 → myp_event_route_to_instance`
检查又把事件 route 回其他目标线程 → **无限乒乓**（route 8.7 万+ 次，各 @thread 目标
收到 5 万+ 次，正常应各 1 次）。

**修复（C++ mypc + runtime + 自举 myp_self 三端一致）**：

1. `src/codegen/codegen_class.cpp` `generateMappingDecl`：handler 注册 `instance`
   从 NULL 改为**目标实例全局地址 `&__myp_inst_X`**（传地址非值——注册发生在
   `__myp_init`，早于实例 new；仅单目标普通目标链生效，lambda/transformer/函数目标
   或无类级实例全局回落 NULL，保留原行为）。
2. `src/runtime/runtime.c`：`myp_event_dispatch` 用 `myp_handler_target()`——注册
   instance 为全局地址时**运行时解引用**得真实实例，按线程归属路由——routed 副本
   只跑归属本线程的 handler，跨线程多目标各收 1 次。
3. `tools/selfhost/src/codegen.myp`：镜像注册 instance 逻辑（`regInst`）。

**验证**：新回归 `tests/bugs/cross_thread_multi_target.myp`（@test 断言 A/B 各收 1 次，
2 断言，已接入 `tests/bugs/run_bugs.sh` 门禁）；`run_bugs.sh` 7/7 全绿；全量回归
311 通过 / 0 失败；selfhost `test_myp_self.sh` 94/94 + bootstrap 16/16（不动点
myp_self2 == myp_self3 字节相同）。MOS `app_lifecycle_demo` 应用从非 @thread 改为
@thread（解除原规避），验证真实跨线程广播：AppManager（@thread）广播 Lifecycle 到
两个 @thread 应用各收 1 次（Notes lifecycle=1/2/3 各 1 次，无乒乓）；MOS ctest 11/11。
跨线程事件广播（1 事件 → 多 @thread 目标）自本版起可用。

### v3.12.46 — 通用桥接发现（新增桥无需改编译器）+ SDL_ttf 中文渲染 + BUG-029/030

**通用桥接发现（核心重构，`src/main.cpp` linkObjects）**：删除 per-bridge 硬编码
（SDL / SDL_ttf 各一段），改为 **symbol 驱动的通用发现——新增 bridge 无需再改编译器**：

- 候选桥接 = `bridges` 目录下的 `*.c`：默认 `<stdlib>/bridges`；可用 `MYP_BRIDGES`
  环境变量追加目录（冒号分隔多个，如 `MYP_BRIDGES=/path/to/mos/bridges`）。
- 程序 `.o` 的 `nm -u` 未定义符号与桥接 `nm --defined-only` 取交集，命中即自动
  编译（`/tmp/myp_rt_cache` 缓存，按源码+标志哈希）+ 链接。
- **固定点迭代处理依赖链**：桥接自身的未定义符号并入待解集合（如
  `sdl_ttf_bridge.c` 依赖 `myp_sdl_get_renderer` → 自动拉入 `sdl_bridge.c`）。
- 侧车文件（可选）：`<名>.c.cflags` 附加编译标志、`<名>.c.libs` 链接库
  （如 `sdl_bridge.c.libs` = `-lSDL2`）。
- 桥接文件自 `src/runtime/` 迁至 `stdlib/bridges/`（`sdl_bridge.c` /
  `sdl_ttf_bridge.c` + 两个侧车）；父 `CMakeLists.txt` `myp_runtime` 路径同步。
- 验证：纯控制台程序不链 SDL；`import sdl` 链 `-lSDL2`；`import ttf` 自动拉
  `-lSDL2_ttf`+`-lSDL2`；`MYP_BRIDGES` 下新增测试桥（含 `-lm` 侧车）不改编译器
  即自动编译链接运行。
- **自举编译器镜像（`tools/selfhost/src/link.myp`）**：同样实现通用桥接发现——
  `bridgeDirs`（MYP_BRIDGES + `<stdlib>/bridges`）/ `listBridges` / `nmSymbols` /
  `readSidecar` / `compileBridge`（mtime 缓存）/ `symIntersect`，link() 内固定点
  迭代依赖链 + 链接命令追加桥 obj/libs。用 `myp_self` 编译 `import sdl` 链
  `-lSDL2`、`import ttf` 拉 `-lSDL2_ttf`+`-lSDL2`、`MYP_BRIDGES` 新桥自动链接
  运行（5/120/5）均验证通过。

**SDL_ttf 中文渲染（M1 里程碑）**：新增 `stdlib/ttf.myp`（`Ttf` 静态类：
`init(px)` / `drawText(x,y,text,scale,r,g,b,a)` / `close()`），经通用桥接自动链接
`stdlib/bridges/sdl_ttf_bridge.c`（TTF_RenderUTF8_Blended + Noto CJK 等系统字体，
抗锯齿中文，替代 5×7 位图仅 ASCII）。`sdl_bridge.c` 增加 `myp_sdl_get_renderer` /
`myp_sdl_get_window` 访问器供复用。MOS 新增 `TtfLabel` 控件 + `apps/ttf_demo.myp`
（headless 冒烟：init / drawText 返回 0）。

**修复 BUG-029（类字段 → interface vtable 空指针崩溃）**：sema/codegen 按当前类
属性表 + 表达式类型解析具体类名（含 `this` / 字段访问），自举 myp_self 镜像
`upcastClsName`。**修复 BUG-030（constructor 内发 mapping 事件崩溃）**：构造函数
入口先写 `class_instance_globals_` 实例指针，自举 `curFnIsCtor_` 标记。`SDL2` 在
`CMakeLists.txt` 改为 `pkg_check_modules(... QUIET)` 可选（无 SDL2 环境仍可构建
mypc 全工具链）。

### v3.12.45 — 接口分派去虚拟化（devirt）+ 自举 slice 边界检查补齐 + 新基准

**去虚拟化（C++ mypc 与自举 myp_self 双端镜像）**：接口方法调用
`Shape s = new Circle(...); s.area()` 此前一律 vtable 间接调用
（extract {data,vtable} → GEP vtable[midx] → load fn ptr → 间接 call）。当对象
具体类在调用点静态已知且从未被重赋值时，改为**直接调用具体类方法**，让 LLVM
内联 → 常量折叠/向量化复利。

- **安全边界（关键设计）**：devirt 仅在**接口变量从未被重赋值**时触发。sema 在
  变量声明时记录具体类快照（concreteClass），任何赋值（含复合赋值/条件分支内）
  标记 reassigned → 后续调用回退 vtable。流不敏感保守：条件分支内重赋值也放弃
  devirt。`new` 表达式接收者本就直接调构造，不涉 vtable。
- **selfhost**：
  - `sema.myp`：`SymbolEntry` 新增 `reassigned_` 字段；`Assign` 分支对接口变量
    重赋值标记；B3 块扩展——从仅 assoc 关联类型方法扩展为**所有接口方法调用**
    （对象是接口变量 + concreteClass 已知 + 未重赋值 → `CallExpr.resolvedClass`
    记具体类）。
  - `codegen.myp` `genIfaceCall`：`resolvedClass` 非空 → 直接调
    `<cls>_<method>`（类覆盖）或 `__ifdef_<iface>_<method>_<cls>`（trait 默认
    实现，复用 findIfaceDefault），跳过 vtable。发射的 extract/GEP/load 在 -O2
    被 DCE（无副作用）。
- **C++ 镜像**：
  - codegen 接口分派点（泛化分派）：接口变量（Identifier 对象）`var_class_map_`
    命中且未重赋值 → 直接调具体类方法（返回类型从具体函数取，含 assoc 真实
    类型）。
  - `generateAssignment` 对 `var_class_map_` 中的变量赋值 → 标记 `iface_reassigned_`。
  - **修复 catch(Error) 污染**：`catch (FileError e)` 曾设 `var_class_map_["e"]`
    残留具体类 → 后续 `catch (Error e)` 的 `e.message()` 被 devirt 误用 FileError
    （exception/exception_lib 回归暴露，`iface_parse` 输出 file error 而非 parse
    error）。修复：iface_catch 绑定处 `var_class_map_.erase(cc.var_name)`——
    catch 接口变量的具体类运行时决定，永不可 devirt。
- **边界检查补齐（自举对齐 C++，正确性）**：selfhost slice 下标此前是裸 GEP
  **无边界检查**（越界静默读穿），C++ `generateSliceElementAddress` 有完整
  `0<=idx<len` + `myp_bounds_error`。新增 `sliceElemAddrChecked` helper（读/写/
  `subscriptElemAddr` 三处统一），文本 IR 发射 `icmp sge/ult` + `and` + 分支到
  error block（`myp_bounds_error`）+ ok block GEP；`ir_emit.myp` 补
  `declare void @myp_bounds_error(i64,i64)`。越界双端一致报
  `slice index 5 out of bounds (length 4)` + abort。实测 LLVM -O2 对
  `for(i=0;i<n)` + `len==n` 形态完全消除检查 → **无性能损失**。
- **新基准**：
  - `bench/myp/iface_dispatch.myp`：接口热循环分派（Shape 接口 3 实现，**有状态
    方法 grow 改内部字段**防常量折叠，devirt 后内联仍须计算，测真实分派开销）。
  - `bench/myp/slicedot.myp`：`slice<double>` 点积（边界检查开销度量）。
  - 已加入 `run_compare.sh`（44 → 46 项）。
- **基准结果（devirt 后，3 轮取最小）**：iface_dispatch **MYP 30ms vs C++ 46ms
  （C++/MYP 1.53，devirt 后内联反超 g++ 保守 devirt）**；slicedot MYP 4ms vs
  C++ 6ms（1.50，边界检查被 LLVM 消除）；其余 44 项与基线一致无回归。
- **验证**：新增回归 `tests/@test/devirt_reassign.myp`（未重赋值 devirt / 重赋值
  回退 vtable / 条件分支重赋值保守 / 独立变量 devirt，双端 5/5）。全量 **311/0**
  （-O0 与 -O2）；selfhost `@test` 94/94；bootstrap 16/16 不动点（self2==self3
  字节相同，md5 6b67c55e…）。

### v3.12.44 — 修复 `-O2 × setjmp/longjmp` 根因：try 入口逃逸全部在作用域局部 + finally flag

根治 B2 遗留的「opt -O2 破坏 setjmp/longjmp 的 finally 语义」根因（C++ 与自举同修），
`-O2` 现在可以安全使用。

- **根因**：LLVM 的 CFG **不建模 longjmp 这条边**——try 块以 noreturn
  `__myp_longjmp` + `unreachable` 结尾，于是：
  1. try 块内对跨 setjmp 存活局部的 store 被死代码消除（DCE 认为 try 块后面不可达）；
  2. longjmp 路径（finally/catch）对这些局部的 load 被折叠成入口值——mem2reg/SROA
     把跨 setjmp 的局部提升为 SSA 寄存器，longjmp 恢复后读到的不是 try 内写的值。
  实测 `arc_throw` 的 `fin_run`：try 内 `fin_run=1` 被 DCE、finally 里 `fin_run+1`
  被整块消除，断言 `myp_assert_eq(i32 0, i32 2)`（应 2）。C++ `mypc -O2` 同样失败
  （v3.9.0 只对 ARC 槽逃逸，标量局部未覆盖）。
- **修复**：try 入口（setjmp 前）把**全部在作用域局部 + finally flag 的地址**传给
  `myp_try_escape` 无操作（运行时空函数），使它们成为 LLVM 眼中的逃逸内存——
  mem2reg/SROA 不再提升、DSE 不删 store，finally/catch 从物理内存读到真值。
  jmp_buf 经 `myp_exception_push`/`setjmp` 传地址本就逃逸，无需额外处理。
  - 自举 `tools/selfhost/src/codegen.myp` `genTryStmt`：遍历 `localAllocas_` 发射
    `myp_try_escape`；`ir_emit.myp` 补 `declare void @myp_try_escape(ptr)`。
  - C++ 镜像：`CodeGen::escapeSlot` 助手（抽出原 registerArcSlot 内联逻辑），
    `generateTryStmt` 遍历 `named_values_` 全作用域指针值 + finally flag。
  - 自举源码自身不用 try → 逃逸不会进入自举编译器自身 IR，bootstrap 不动点不受影响。
- **附带修复测试脚本引号 bug（MYPCC 带参数时失效，v3.9.0 同类遗留）**：`run_tests_O2.sh`
  设 `MYPCC="./build/mypc -O2"`（含空格）时，`test_coro_stack_warn.sh` /
  `test_package_path.sh` 的 `"$MYPCC"` 把整串当单个文件名、`tools/codegen/run_tests.sh`
  的绝对化 `basename` 带进 `-O2` → 均退出 127/找不到编译器。修复：调用处去引号
  （`$MYPCC`），`tools/codegen/run_tests.sh` 二进制取首词绝对化（`MYP_ABS`）+ 保留
  flags，`MYP_CC` 导出二进制绝对路径。
- **附带修复 compile 模式 stdlib 回退**：自举 CLI 直接 compile（非 `run` 子命令）未显式
  给 `--stdlib` 时 stdlib 为空 → 从子目录（如 `bench/`）编译时 `Link.link` 的 runtime
  路径回落 CWD 相对（`src/runtime/runtime.c` 找不到 → 链接失败）。修复：compile 模式
  stdlib 为空时回退 `Cli.selfStdlib()`（与 `run` 子命令一致）。`bench/run_compare.sh`
  用 `myp_self2` 跑全部 44 项基准不再失败。
- **验证**：
  - `arc_throw`：**C++ 与自举 -O0/-O2 均 15/15**；IR 复核：-O2 后 finally 里
    `add i32`（fin_run 自增）存活、断言不再折叠为 0。
  - 全量回归 **-O0 310/0**、**-O2 310/0**（此前 -O2 arc_throw 必挂）。
  - 自举 `@test` 全套 **-O0 104/0、-O2 104/0**；`test_myp_self.sh` 94/94；
    `test_myp_bootstrap.sh` 16/16 不动点（self2==self3 字节相同，md5 dd7fc3c7…）。
  - 异常专项（exception/exception_lib/exception_thread/exception_throwin +
    @test arc_throw/result）自举 -O2 全过。
- **默认优化级别调整为 -O2**：根因修复后 **C++ `mypc` 与自举统一默认 -O2**——
  mypc `src/main.cpp`（`compileSingle`/主流程/`run` 子命令默认 `opt_level` 0→2，
  `--help` 文本同步）、自举 `main.myp`/`link.myp`（CLI/`run`/`fmt` 统一默认
  `optLevel=2`；v3.12.10 曾默认 -O2、B2 回退 -O0、本版本恢复）。`-O0/-O1/-O3` 显式
  覆盖、`MYP_SELF_OPT=0` 强制关闭。opt 步骤编译开销 +24%，换取生成程序 3-23x 运行
  提升。自举链全程 -O2 编译，bootstrap 不动点保持（self2==self3 字节相同，
  md5 d62f9f45…）。
- **性能权衡**：逃逸只作用于含 try 的函数（try 入口每在作用域局部一次无操作调用，
  该函数局部不再提升寄存器）；自举编译器自身无 try → 编译/运行性能不受影响。
  默认 **-O2**（生产性能），`-O0`/`MYP_SELF_OPT=0` 快速编译/调试。

### v3.12.43 — 自举编译器 B 类缺口：assoc 关联类型 / 嵌套 struct / 多文件合并全绿

自举（selfhost）编译器追赶 C++ 编译器的 B 类功能缺口，**全量回归 310 通过 / 0 失败**。

- **B1 slice_class_chain**：`new Node(7).getVal()` 链式调用——CallExpr Member 分支
  cn 解析漏了 `New` 对象（`mo.kind()=="New"` 时取 `resolvedClass`）。自举
  `tools/selfhost/src/sema.myp` 补 `|| mo.kind() == "New"`（镜像 C++）。
- **B2 arc_throw finally**：自举 link 默认 `opt -O2` 把 setjmp/longjmp 跨 finally 的
  alloca mem2reg 提升 → longjmp 后 finally 局部值被清零。改为**默认 -O0**（仅
  `-O` 标志或 `MYP_SELF_OPT=1` 才跑 opt），对齐 C++ 默认。`arc_throw` 15/15。
- **B3 assoc 关联类型 `T::Item`**：接口变量 `Container sb = new StrBox(); sb.getVal()`
  此前返回接口占位 `assoc` → 与真实 `string/int` 类型不匹配。自举 sema 用
  **作用域感知的符号条目具体类字段**（`SymbolEntry.concreteClass_`，镜像 C++
  `var_class_map_`）解析接口变量具体实现类的同名方法返回类型，并把具体类记到
  `CallExpr.resolvedClass` 供 codegen 分派用；codegen `genIfaceCall` 关联类型返回
  类型改从具体类取。`assoc_string_dispatch` 4/4、`manual_ch6_class` 16/16。
- **B4 manual_ch7_struct**：嵌套/文件级限定 struct（`Sensor::Config`、`Device::Mode`）：
  - sema：`typeToKind` 限定 struct 名**优先于关联类型判定**（否则误判 assoc）；
    `findStruct`/`inStructName` 支持限定名→裸名回退；嵌套 struct 注册提前到顶层
    函数/测试体访问前。
  - codegen：`emitStructTypes` 补发嵌套 struct 类型并给全限定 key；LLVM 标识符
    mangling（`::` → `$`，`llvmSafeName`）用于 struct 类型名/方法函数名；struct
    方法内 `this.field` 分支（镜像 C++ `generateStructMemberAddress` ThisExpr）；
    struct 兄弟方法裸调用 → `struct_<key>_<fn>`。`manual_ch7_struct` 12/12。
- **B5 multifile 多文件合并**：compile 模式此前只编译首个输入文件。改为收集全部
  位置参数为输入文件，`Frontend.compile` 逐文件解析 + **全量合并**
  （imports/structs/bitfields/classes/interfaces/mappings/functions/enums/ffis/
  macros/typeAliases，镜像 C++ multi-file），命令行文件预置 loaded 防重复，再统一
  加载所有 imports。`test_multifile.sh` 4/4（含 BUG-025/026）。
- **run 子命令 stdlib 定位**：`Cli.selfStdlib()` 增 cwd 向上搜索
  （`../../stdlib`…），修复从子目录（tools/codegen）跑 `run` 时
  `cannot find import 'env'` → codegen 工具自测通过。
- **回归**：全量 **310 通过 / 0 失败**；自举 `test_myp_self.sh` 94/94；自举
  `test_myp_bootstrap.sh` 16/16 不动点（self2 == self3 字节一致）。自编译
  `build/myp_self2` 与仓库布局对齐后 pm/gitee/LSP 位置推断回归全绿。


### v3.12.42 — 修复 BUG-005：mapping 事件 action 在事件源线程执行（跨线程路由）
- **BUG-005 已修复**：mapping handler 的目标 action 此前在**事件源线程**执行，
  `@thread` 实例 B（action）的线程归属被忽略。改为按 **handler 实例**线程归属投递。
- **修复**（C++ + 自举同修）：
  - `src/runtime/runtime.c`：`myp_event_fire` 增加 `data_size` 参数（载荷深拷贝按
    字节数）；新增 `myp_thread_is_current(instance)` / `myp_event_route_to_instance(...)`
    运行时；`myp_event_t` 增 `data_size`/`data_owned`/`routed` 字段；dispatch 对归属
    其他线程的 handler 将事件深拷贝投到其线程队列，路由副本 `routed=1` 不再重复
    路由，处理后 free 拷贝；新增 `myp_thread_self()` 诊断 FFI（线程稳定 id）。
  - `src/codegen/codegen_class.cpp` `generateMappingDecl`：handler 内对首个非静态
    目标实例做 `myp_thread_is_current` 检查——目标在其他线程 → 调
    `myp_event_route_to_instance` 后返回；否则直接调用。
  - `src/codegen/codegen.cpp` / `include/mylang/CodeGen.h` / `runtime.h`：FFI 声明
    更新（fire 4 参 + 两个新运行时）。
  - 自举镜像：`tools/selfhost/src/codegen.myp` genMappingChain 同检查；
    `genThreadVar` 补存 `@__myp_inst_<Cls>` 全局（原缺失 → handler 取 null）；
    `ir_emit.myp` 更新 `myp_event_fire` 4 参声明 + 新增运行时 declare。
- **回归**：`tests/bugs/mapping_thread.myp`（`myp_thread_self()` 断言 handler 在
  handler 实例自己的线程执行，3 断言）。全量回归 **308 通过 / 0 失败**；bugs 4 绿；
  自举 `test_myp_self.sh` 94/94。


### v3.12.41 — 修复 BUG-011：函数内 mapping 用实例变量名节点 → 编译期诊断
- **BUG-011 已修复**：函数内 `mapping(){ s.e -> t.a; }`（s/t 为局部实例变量）此前
  在 handler 函数里 load 外层函数的局部 alloca → 跨函数指令引用 → LLVM verify
  `Referring to an instruction in another function!`。改为编译期诊断：mapping 节点
  须用类名（实例级映射暂不支持），消息带真实类名提示（`Source.e`）。
- **修复**：`src/sema/sema.cpp` MappingStmt 访问器 + 自举 `tools/selfhost/src/
  sema.myp` `analyzeMapping` 同镜像。
- **回归**：`tests/negative/instance_mapping.myp`（编译拒绝）。文件级/类名节点
  mapping（`tests/@test/instance_mapping.myp` 等）不受影响。


### v3.12.40 — 修复 BUG-014 + BUG-010：原子 load/store + 裸 struct 属性字段链
- **BUG-014 已修复**：`Atomic.loadInt`/`storeInt` 此前编译成**普通非原子** load/store
  （仅命名带 Atomic）。改用原子 `LoadInst`/`StoreInst` 构造器（seq_cst；LLVM 21
  IRBuilder 无 CreateAtomicLoad/Store），与 add/sub/xchg/addDouble 的 atomicrmw
  一致。自举 `tools/selfhost/src/codegen.myp` 同镜像（`load atomic`/`store atomic`）。
  回归：`tests/@test/atomic_load_store.myp`。
- **BUG-010 已修复**：裸 struct 属性字段读写 `p.x`（`property: Point p;`，即
  this.p.x）——读此前 `genExpr(p)` 加载 struct 值当指针 GEP → LLVM verify 失败；
  写落到 "external obj.prop" 错误/属性非首属性时 break 提前退出。
  - 读：`generateMemberAccess` 加裸 struct 属性分支；属性遍历 `continue`。
  - 写：`generateAssignment` 在 `if(!op)` 内、错误兜底之前加裸属性分支（原 2222
    块位于错误之后**不可达**——死代码，已移除）；`generateStructMemberAddress`
    ThisExpr 分支支持类 struct 属性（`this.s.x`）。
  - 自举镜像：memberAddr/memberFieldType/memberFieldAstType 加 `bareStructPropName`
    分支。
  - 回归：`tests/@test/struct_prop_chain.myp`（裸/显式 this 读写，9 断言；C++ 与
    自举均绿）。


### v3.12.39 — 修复 BUG-015 + BUG-008 + BUG-012 + BUG-009 + BUG-006：sema 校验类
- **BUG-015 已修复**：`mypc --package-path` 按 `:` 切分多路径（`src/main.cpp
  loadModule`），与自举 `myp_self` 一致。回归：`tests/test_package_path.sh`。
- **BUG-008 已修复**：接口 action 签名匹配升级为**精确签名**（名称 + 参数类型 +
  返回类型；`paramsMatch`），事件按名称 + 参数类型；关联类型保留仅名称匹配。
  自举 sema.myp 同镜像。回归：`tests/negative/interface_param_mismatch.myp`。
- **BUG-012 已修复**：对 `@thread` 实例直接调用普通 action → 编译拒绝
  （`cross-thread calls must go through mapping()`）；`@startup` 手动调用规则保留。
  自举 sema.myp 同镜像。回归：`tests/negative/cross_thread_call.myp`。
- **BUG-009 已修复**：一个类多个 `@startup` → 编译诊断 `at most one @startup per
  class`。自举 sema.myp 同镜像。回归：`tests/negative/multiple_startup.myp`。
- **BUG-006 已修复**：`main()` 直调检查被运算符/管道绕过——`visitBinaryOp`（外部
  `@op`）与 `visitPipe`（class transform）在 main() 内拒绝；struct 方法调用保留
  放行。自举 sema.myp 同镜像。回归：`tests/negative/main_not_wiring.myp`。


### v3.12.38 — 修复 BUG-022 + BUG-007：sema 校验类（@thread / bitvector）
- **BUG-022 已修复**：`@thread` 仅可用于 class 实例——struct 加 `@thread` 编译
  拒绝（`'@thread' can only be applied to a class instance variable`）。自举
  sema.myp 同镜像。回归：`tests/negative/struct_thread.myp`。
- **BUG-007 已修复**：`bitvector<N>` 宽度校验 ∈ {8,16,32,64}，否则编译拒绝
  （`bitvector width must be 8/16/32/64`）；codegen default 分支不再静默用 i32。
  自举 sema.myp 同镜像。回归：`tests/negative/bitvector_width.myp`。


### v3.12.37 — 修复 BUG-013：Coro.resume 返回值串值（yield/resume 值改每协程存储）
- **BUG-013 已修复**：`src/runtime/runtime.c` 用 `__thread` 线程本地共享槽
  `myp_coro_yield_val`/`myp_coro_resume_val` 存「上次挂起传出的值」与「上次 resume
  传入的值」。同线程多协程混用时后挂起者覆盖前者：echo 挂起 10 被 topLevel
  `Coro.yield(42)` 覆盖 → `Coro.resume(echo_h, 100)` 返回 42（应 10）；加
  `Async.sleep` 定时器挂起覆盖为 0。协程内部值传递始终正确，只有 resume 返回值串。
- **修复**：yield/resume 值改**每协程存储**——`myp_coro_t` 新增 `yield_val`/`resume_val`
  字段；`__myp_coro_yield`/`__myp_coro_resume` 按目标协程槽读写；`__myp_coro_create`
  槽复用/新建时清零。多协程、嵌套 resume 均按各自槽取回。
- **回归**：`tests/@test/coro_resume_value_mix.myp`（echo await 值挂起 10 + topLevel
  Coro.yield 42 + timerCoro Async.sleep 挂起 0 三协程混用，3 断言，3 次运行稳定）；
  `tests/bugs/coro_resume_value_mix.myp` 移除。
- 全量回归 **300 通过 / 0 失败**（含 tests/coro、tests/coro_top 既有协程用例）。
  design.md §8.6.1 的规避写法（不打印 resume 返回值）现可放开。


### v3.12.36 — 修复 BUG-018：类型参数全局作用域泄漏（collections + where 约束伪错误）
- **BUG-018 已修复**：`src/sema/sema.cpp` `visitClassDecl` 把类**通用类型参数**在
  `enterScope()` **之前**（全局作用域）声明——类作用域弹出后 T 残留全局符号表；后续
  同名类型参数泛型类覆盖全局 T。`import collections`（`Set<T>` 无约束 T→Int）+ 用户
  `Processor<T where T:Container>`（T→Container 接口）→ 检查 Set<T> 模板体
  （`val % cap_`、`data_[i] < x`）时 T 解析为 Container → 8 个伪错误
  `expected numeric type, got 'Container'`（行号落 stdlib）。
- **修复**：类型参数注册移到 `enterScope()` 之后（类作用域内），弹出即清除——同名
  类型参数不再跨类泄漏/覆盖。与 BUG-021（current_class_name_ 污染）同类。
- **回归**：`tests/@test/assoc_constraint_import.myp`（collections + `where
  T:Container` + `T::Item` + `Processor<IntBox>` 实例化，1 断言）；`tests/bugs/
  assoc_constraint_import.myp` 移除。自举编译器天然无此 bug（Pass A/B 隔离）。
- 全量回归 **299 通过 / 0 失败**。


### v3.12.35 — 修复 BUG-023：@parallel/@gpu 并行体直接访问 static 属性数组
- **BUG-023 已修复**：`@parallel for` / `@gpu for` 并行体直接读写 `@static class`
  属性数组（`X.arr[i] = i`）→ `emitKernelExpr` 静态属性分支要求类名在 `kernel_vars`
  （并行体只捕获外层局部变量）→ 落到 `i64 0` 占位 → 下标 GEP 基址为整数 0 → LLVM
  verify 失败（`getelementptr i32, i64 0, %0`）；`Atomic.addInt(X.sum,...)` 传 0 占位
  当数组指针 → 运行段错误 139。
- **修复**（`src/codegen/codegen_gpu.cpp`）：MemberAccess 静态属性分支直接以模块全局
  `__myp_static_<Class>` 为基址 GEP 进属性槽（CPU `@parallel` 同模块直取）；`@gpu`
  核函数（独立 PTX 模块）仍走捕获的 kernel arg（`kernel_vars` 命中时优先）。
- **回归**：`tests/@test/parallel_prop_access.myp`（静态属性数组写 + 读 +
  `Atomic.addInt` 原子累加，4 断言，3 次运行稳定）；`tests/bugs/parallel_prop_access.myp`
  移除。
- 全量回归 **298 通过 / 0 失败**；自举 94/94。


### v3.12.34 — 修复 BUG-028：类属性带 ARC 初始化器 → 悬垂/双释放
- **BUG-028 已修复**：`property: Foo f = new Foo();`（class/interface/string/slice/
  数组属性带初始化器）——属性默认值发射（`src/codegen/codegen_expr.cpp`）对 fresh
  `new Foo()` 直接 store 到属性槽，**未 `arcConsumeTemp`** → rc=1 留在语句末临时释放
  列表 → 语句末 release → 属性槽悬垂。读取 use-after-free；setter 重赋值释放悬垂旧值
  → 双释放 → 运行段错误 139。
- **修复**：属性初始化器与 `this.prop = value` 赋值路径同语义——ARC 引用属性
  （class/interface/string/slice/counted-array）`arcStoreRef`/`arcStoreSlice` +
  `arcConsumeTemp`；alias retain、fresh consume（`isFreshArcExpr`）。
- **自举镜像**：`tools/selfhost/src/codegen.myp` 属性默认值发射同样未 consumeTemp（IR
  复核：fresh 对象语句末 `myp_release` 悬垂）→ 同修复：`ft=="ptr" && isArcType` 走
  `storeRef(gep, pv, isFreshTemp(pv))`。IR 复核不再语句末释放；`test_myp_self.sh` 94/94。
- **回归**：`tests/@test/property_init_arc.myp`（初始化器对象存活 + 多次重赋值读取，
  3 断言）。BUG-021 修复验证时暴露（属性初始化器此前无编译通过的用例）。
- 全量回归 **296 通过 / 0 失败**。


### v3.12.33 — 修复 BUG-021：class 含泛型类属性时 `this.prop` sema 解析污染
- **BUG-021 已修复**：`src/sema/sema.cpp` `visitClassDecl`（泛型实例化入口）设置
  `current_class_name_` 后**不恢复** → class H 含 `Option<int> o` 属性时，Pass 2
  `buildCurrentClassMemberTypes` 解析属性类型触发 `Option<int>` 实例化 → 退出后
  `current_class_name_` 残留 `Option_int_inst` → 方法内 `this.v` 解析到实例类 →
  `class 'Option_int_inst' has no member 'v'`（读+写都中）。
- **修复**：`visitClassDecl` 开头保存、末尾恢复 `current_class_name_`（类上下文不污染）。
- **回归**：`tests/@test/this_generic_prop.myp`（`Option<int>` + `ArrayList<int>` 泛型
  属性 + `this.v` 读写 + 泛型属性方法调用，4 断言）；`tests/bugs/this_generic_prop.myp`
  移除。验证时顺带暴露 BUG-028（属性初始化器 ARC）。
- 全量回归 **296 通过 / 0 失败**。


### v3.12.32 — 修复 BUG-017：关联类型接口方法返回 string 经接口分派类型错误
- **BUG-017 已修复**：接口虚表动态分派处（`src/codegen/codegen_expr.cpp` 三处）返回
  类型一律取接口声明的关联类型占位符 → `typeNodeToCodegenType` 回落默认 **i32**，而
  具体类方法返回 string（ptr）→ `call i32 %iface_fn(ptr %4)` 把 string 当 i32 → 调用方
  （期望 ptr）LLVM verify 失败（单方法接口）/ 运行段错误 139（含其他方法时）。`Item=int`
  因默认类型恰为 i32 侥幸通过。
- **修复**：新增 `CodeGen::ifaceDispatchReturnType`——优先从对象已知具体类
  （`var_class_map_` / `array_elem_class_map_`）解析其同名方法返回类型（与 vtable 指向的
  具体方法一致），未知回落接口声明类型；三处分派点统一改用。
- **回归**：`tests/@test/assoc_string_dispatch.myp`（string+int 双关联类型 + 多方法接口
  动态分派，4 断言）；`tests/bugs/assoc_string_dispatch.myp` 移除。泛型单态化路径
  （`Processor<T where T:Container>` 静态直接调用）本就不受影响。
- 全量回归 **295 通过 / 0 失败**。


### v3.12.31 — 修复 BUG-016：void 值赋给变量导致编译器段错误
- **BUG-016 已修复**：`var r = <void调用>();` / `int x = <void调用>();` 此前被 sema
  放行 → codegen 用 Int(i32) alloca 存 void 值 → LLVM `getPrefTypeAlign(void)` 无限递归
  → **编译器段错误**（exit 139）。
- **根因纠错**：原诊断「`main(int argc, string[] argv)` 传参导致类型布局无限递归」不成立
  ——`int main(int argc, string[] argv) { return argc; }` 编译运行正常；真正触发是复现中
  `var r = report(argc, argv);`（report 返回 void）。与 argc/argv 无关。
- **修复**（`src/sema/sema.cpp` visitVarDecl + 自举 `tools/selfhost/src/sema.myp` 镜像）：
  1. 推断路径 `var r = voidCall();` → `cannot infer type of 'var' from a void expression`；
  2. 显式路径 `int x = voidCall();` → `cannot initialize variable 'x' of type 'int' with
     value of type 'void'`。
  两级均用 `diag_.errorCount()` 快照区分「已知 void 调用」（补报）与「未解析表达式
  （已级联报错）」（跳过），避免级联误报。
- **回归**：新增负测试 `tests/negative/var_void_init.myp` + `tests/negative/void_value_init.myp`
  （编译拒绝）；原复现 `tests/bugs/main_argc_argv_crash.myp` 移除。
- 全量回归 **295 通过 / 0 失败**（+2 新负测试）；自举 sema 对拍 **94/94** 全绿。


### v3.12.30 — 修复 BUG-024：相对路径导入去重解析 `..`
- **BUG-024 已修复**：`src/main.cpp` `normalizePath` 此前只移除 `./`/`/./`/`//`，不解析
  `..` —— 同一文件经不同相对路径（直导 `./helper.myp` + 子模块 `../helper.myp`）规范化后
  仍不同 → 双重载入 → `duplicate class name`/`duplicate function name`。
- **修复**：重写 `normalizePath` 为词法组件解析——按 `/` 分段，`.`/空段跳过，`..` 弹栈折叠
  （相对路径保留前导 `..`；绝对路径根 `..` 丢弃），`//` 自然合并；同一文件归一到同一规范键。
- **回归**：复现移入正测试 `tests/@test/relimport_dedup.myp`（+ helpers/b24_helper.myp +
  relimport_sub/sub.myp），直导 + `..` 递归同文件去重、2 断言通过；`tests/bugs/` 原复现移除。
- 全量回归 **293 通过 / 0 失败**。


### v3.12.29 — README.md / README_EN.md 更新对齐当前状态
- **Hello World**：旧的 `int main(){ Console.writeLine(...) }`（现已编译报错）→
  `@startup` + `mypc run` 写法（无需手写 main）。
- **组件与映射**：mapping 节点实例名 → 类名（`Sensor.valueRead -> Display.show`）。
- **标准库**：39 → 40 模块（实测顶层可导入 40 个）。
- **工具链表**：补 `myp_debug`（DAP 调试适配器）、`myp_self`/`myp_self2`（自举编译器，
  含 GPU NVPTX 发射、两级自举成立）、`tools/codegen`（schema 驱动代码生成框架）。
- **测试**：181 → 292 通过 / 0 失败（回归 110 / 负 74 / 测试框架 100 / 自举/LSP 等，
  实测当前汇总）。
- **项目结构**：tools/ 行补 selfhost（自举编译器）与 codegen。
- 中英文两份同步；纯文档，无代码变更。


### v3.12.28 — manual_en.md 内容全面对齐中文版（逐章修正过时/错误表述）
- **§1 Hello World**：旧的 `int main(){ Console.writeLine(...) }`（现已编译报错）→
  三种等价写法（@startup+mypc run / @constructor / @thread）+ main 接线规则注记。
- **§2 字面量**：补二进制/八进制/前导零八进制/下划线/后缀/null 语义；**运算符表**：
  旧 10..0 缺按位/移位/Range/结合性 → 重写为 15..0 + 结合性 + 位运算 + Range。
- **§3 类型系统**：基础表补 bit/bitvector；数字提升改为"仅无损隐式+有损显式"；
  补无符号类型/显式转换/位类型/bitcast/位操作/checkedAdd/parse/Math 多态各节。
- **§4 控制流**：补「枚举与 match」小节（v2.1）。
- **§5 函数**：main 接线 mapping 改类名节点；struct 函数式构造改为"仅位置实参、
  不支持命名实参"；补 nonlocal 按引用捕获 + Man or Boy 测试。
- **§6 Class**：三段式 → 四段式（+ function: 段）；段规则补 struct 行。
- **§8/§9**：mapping 节点统一类名（@thread/@threadpool 实例名/`pool[0]` 改类名）；
  @parallel 限制改"int 或 long 均可" + 补"并行体只捕获局部变量、属性先拷局部"。
- **§10**：补点分模块名（import gpu.hal）+ 相对路径去重按字符串、`..` 未规范化注记。
- **§11 标准库**：修正 4 处过时 API（Math.absInt→Math.abs、__myp_io_*→File 方法、
  stream 迭代器→事件驱动、SDL.init/shouldClose/quit→open/running/close）；补
  import result / setops / gpu（L1+L3）三个模块节。
- **§13**：CLI 表补 --frontend-dump；补「How to Add Tests」节。
- **§14 完整示例**：IoT 示例修正（Timeline 构造器初始化、t.now()、mapping 类名）。
- 纯文档对齐，无代码变更。


### v3.12.27 — manual_en.md 对齐中文版（章节重排 + 新增 codegen/自举/代码生成工具）
- **manual_en.md 全面对齐 manual.md**（英文版此前停留 v3.0 旧结构）：
  - 版本头 3.0 → 3.12；目录加 12. Metaprogramming，Compilation & Tools → §13、
    Complete Example → §14。
  - 元编程从 §13 的 `#### Metaprogramming` 摘出为独立 `## 12. Metaprogramming`，
    并展开为中文版同构的四层总览 + @eval/macro/@macro 详述 + 设计原则。
  - §13 Compilation & Tools 新增：`#### Codegen (LLVM Backend)`（管线/源码分工/
    internalize/-O 管线/myp-pass/--emit-llvm/MYP_FAST_MATH/语义交互）、
    `### Self-Hosted Compiler (myp_self)`（含 GPU 已实现）、
    `### Code Generation Tool (tools/codegen)`（生成器表/schema/--verify）。
  - 环境变量补 MYP_FAST_MATH、MYP_FMT；项目结构 tools/ 加 selfhost/codegen、
    build/ 加 myp_self/myp_self2。
- 纯文档对齐，无代码变更。


### v3.12.26 — 自举编译器 GPU 状态补正：已实现（非"非 GPU"）
- **纠正手册/文档过时表述**：自举编译器 `myp_self` 的 GPU 部分**已实现**
  （v3.12.4–v3.12.5 落地，实测 myp_self2 为 `@gpu for` 生成 NVPTX kernel .ll →
  llc → PTX → GPU/CPU 双路径，真机 launch 验证），但 manual §13 仍写"非 GPU codegen"、
  "GPU 已入自举范围"，roadmap P3-1 写"当前非 GPU"，self_hosting.md 写"不含 GPU"——全部过时。
- **修正**：
  - `docs/manual.md` §13 自举编译器：intro 改「codegen（含 GPU NVPTX 发射）」；
    范围 bullet 改「**GPU：已实现**——@gpu for/tile/scatter/reduce/scan 生成 NVPTX
    kernel（.ll → llc → PTX → 嵌入），GPU/CPU 双路径（MYP_GPU=1 真机 launch，失败
    CPU 回退）；kernel.* 上下文、float4/double2/int4 向量类型均支持」。
  - `tools/selfhost/roadmap.md` P3-1：补 ✅ GPU 已落地（v3.12.4–v3.12.5，60 检查），
    附注 GpuAlgo.sort 自举产物段错误（既有、非 GPU 相关）。
  - `docs/self_hosting.md`：范围改「全自举（含 GPU）」；T5 行改 codegen（含 GPU NVPTX）。
- 纯文档补正，无代码变更。


### v3.12.25 — 修复 BUG-027：tools/codegen 迁移到 BUG-001 属性私有规则，全量回归首次全绿
- **BUG-027 已修复**：`tools/codegen` 代码生成框架（serde/ffi/autodiff/idl/orm/embed/
  dsl/infer_ops）此前未迁移到 BUG-001 属性私有规则（301 个编译错误）。
  - 模型类加 getter（`get<Prop>()`，model.myp 15 类 + gen_autodiff 的 Expr）；统一命名
    使 `x.prop → x.getProp()` 与变量类型无关。
  - Python 脚本迁移跨类读（含 `).prop`/`].prop` 链式形态，跳过字符串/注释），224+6 处。
  - **gen_dsl 生成模板也犯 BUG-001**：生成的 `CalcExpr` 私有属性 + 生成的 `_eval`
    跨类读 → 生成类加 getter + 模板发 getter 调用。
  - **判断：全部加 getter，无 struct 转换**——`Expr` 是递归树（struct 无限大小）；其余
    类都是 `new`+`ArrayList` 堆对象（值语义破坏共享引用）；selfhost AST 先例即 getter。
  - 修复 run_tests.sh 相对 MYPCC 路径解析 + 接入 `tests/run_tests.sh`。
- **§13 补文档**：新增「代码生成工具（tools/codegen）」节（CLI/生成器表/schema 格式/
  --verify/自测），项目结构 tools/ 加 codegen。
- **全量回归 292 通过 / 0 失败（首次全绿）**；`tools/codegen/run_tests.sh` 11 个生成器
  round-trip 全过。


### v3.12.24 — §13 审计发现 tools/codegen 未迁移到 BUG-001 规则（BUG-027）
- **§13 编译与工具完整核对**：确认各工具节（编译器/自举编译器/测试框架/格式化/
  包管理/可视化/LSP/DAP）均有对应二进制；**发现 `tools/codegen` 代码生成框架缺失
  文档**（schema 驱动生成器：serde/ffi/autodiff/idl/orm/embed/dsl/infer_ops）。
- **新 bug BUG-027**：`tools/codegen` **未迁移到 BUG-001 属性私有规则**——模型类
  （`Expr`/`Field`/`TypeDecl`/`ServiceDecl`/`DslOp`/`Resource` 等 15 类）`property:`
  被生成器跨类读取 → `mypc tools/codegen/main.myp` 编译失败（**301 errors**，约 40 组
  类·属性对）；工具（含独立 `run_tests.sh`）整体不可用。根因：BUG-001 修复（08-16）
  后 `tools/codegen`（08-12 停更）未迁移 getter/struct（自举编译器当时已迁）。
- **决策**：按"手册只记录验证可用内容"约定，§13 **暂不**写入不可用工具；问题登记
  BUGLIST（修复后 `tools/codegen/run_tests.sh` 全绿再补文档）。未接入主套件故全量
  回归维持 288 通过。无代码变更。


### v3.12.23 — 手册 §12 元编程展开：四层能力总览 + @eval/macro/@macro 详述
- **§12 元编程章节大幅展开**（从"三层简例"扩为完整小节）：四层能力总览表
  （泛型 monomorphization 类型级 / `@eval` 值级 v3.4 / `macro` 语法级 v3.5 /
  `@macro` 全功能 v3.6）。
- **@eval**：编译期常量示例（FIB10=55/FIB20=6765/HALF=2.5/T5=165/BIGL=1000000，
  实测）、普通函数调用 @eval 常量折叠（`2*fib(10)`=110，实测）、求值时机
  （sema 后 codegen 前）、纯函数约束表（允许/禁止）、诊断错误原文
  （`construct not supported in @eval context`、`recursion depth exceeded`，实测）。
- **macro**：声明式宏语法（`$ident` 元变量 + AST 片段替换）、嵌套/重复展开
  （v=37，实测）、展开时机（parse 后 sema 前 `expandMacros`）、`--macro-expand`
  AST dump 输出形态（实测）。
- **@macro**：`quote {}` 代码模板 + `$` 插值表（标量→字面量/Expr→内联/
  StmtList·Stmt→内联语句）、AST 值类型（Expr/Stmt/StmtList 编译期专属）、
  `StmtList + StmtList` 拼接、makeCalls(3) 生成 3 条 write（实测输出
  x=42 + 0/1/2）、不生成运行时代码、深度/指令上限。
- **注**：`@eval int[8] t = {...}` 表生成（docs/metaprogramming.md §3.1 声称）
  **编译器实际拒绝**（`expected '(' after function name`）——手册不写入未实现
  特性；本地设计文档该行待修正。
- 纯文档展开，无代码变更；全量回归维持 288 通过。


### v3.12.22 — 手册章节重排：元编程从 §12 摘出为独立章节（新 §12），编译与工具 → §13、完整示例 → §14
- **结构**：原 §12「编译与工具」内的 `#### 元编程（@eval/macro/@macro）` 摘出，升级为
  独立 `## 12. 元编程`（置于编译与工具之前）；原 `## 12. 编译与工具` → `## 13.`，
  原 `## 13. 完整示例` → `## 14.`。同步更新目录、章节锚点与交叉引用
  （§1 的「见 §12」→「见 §13」）；元编程章节补「设计与实现详见 design.md §11」。
- 纯文档结构调整，内容无增删（三层元编程 @eval/macro/@macro 示例原样迁移）；
  无代码变更。


### v3.12.21 — 手册 §12 补充：codegen（LLVM 后端）与自举编译器（myp_self）两节
- **新增 §12「codegen（LLVM 后端）」**：编译管线（lexer→parser→sema→codegen→
  opt→目标文件→链接）；`src/codegen/` 源码分工（codegen/class/stmt/expr/gpu/test +
  myp_passes）；非库构建 internalize（仅保留 main 对外）让 LLVM IPO 常量特化+内联；
  `-O0/-O1/-O2/-O3` 优化管线内容（mem2reg/instcombine/GVN/DCE/内联/循环/SROA/向量化）；
  自定义 pass `--passes myp-pass`（`MypRedundantStorePass` 删 codegen 死 store，保守
  规则）；`--emit-llvm` 检查优化与对拍；`MYP_FAST_MATH=1` FP fast-math；优化 pass 与
  异常/协程/arena 的语义交互回归（-O0/-O2 双级别）。
- **新增 §12「自举编译器（myp_self）」**：T5 自举项目（`tools/selfhost/`）用 MYP 完全
  重写编译器本体；stage0 mypc 编 `myp_self`、自编 `myp_self2`；模块清单（token/lexer/
  ast/parser/diag/sema/ir_emit/codegen/link/main）；用法（编译/run/--frontend-dump/
  --emit-llvm/fmt，均已实测：`myp_self2 run` 输出 "self: hello"、`--frontend-dump
  sema` 输出契约头）；codegen 策略（发射 LLVM IR 文本 + llc + gcc）；oracle 对拍
  （frontend-dump 字节对拍 + 运行输出对拍）；两级不动点自举验证（self2→self3→self4
  字节全同 md5 52c81186…）；进度（F0–H1 完成，P2/P3 闭合）。
- **项目结构/环境变量同步**：tools/ 加 `tools/selfhost`、build/ 加 `myp_self,
  myp_self2`；环境变量补 `MYP_FAST_MATH=1`、`MYP_FMT`（myp_self fmt 的格式化器路径）。
- 纯文档补充（无代码变更）；全量回归维持 288 通过（3 个既有环境失败）。


### v3.12.20 — 手册 §12 编译与工具审计：多文件编译 import/struct 合并（BUG-025）+ `--test` 用户 main（BUG-026）
- **新 bug BUG-025（已修复）**：多文件编译 `mypc a.myp b.myp` 合并循环只搬
  classes/interfaces/mappings/functions，**漏了 imports/structs/bitfields/enums/ffis/
  macros/type_aliases**——第二文件的 `import env`/`import test` 静默丢弃
  （`Console`/`Test` 未定义，且错误行号错位到首文件合并区），第二文件的文件级
  struct/enum 变体同样不可见。修复 `src/main.cpp` 多文件分支：合并全部 11 个字段
  （`loadModule` 按模块名/规范化路径去重，跨文件重复 import 只加载一次）。
- **新 bug BUG-026（已修复）**：`mypc --test` + 源码含用户 `int main()` →
  `LLVM verify failed: Basic Block in function 'main' does not have terminator!`；
  且残留空占位使测试运行器 main 被改名为 `main.1` → 测试**静默不跑**（exit 0
  假过）。修复 `src/codegen/codegen_class.cpp`：test 模式跳过用户 main 时
  `func->eraseFromParent()` 擦除占位，运行器 main 保持名字并成为真正入口。
- **§12 工具链逐条实测**：`mypc --help` 全选项存在（-o/-O[0123]/--stdlib/
  --package-path/--trace/--shared/--static/--emit-llvm/--test/-g,--debug/--passes/
  --macro-expand/--frontend-dump/--version/--help）✓ / `mypc run` 自动 main
  （单 @startup 类输出 "Hello from @startup!"）✓ / `mypc run file args` 传参
  （argc=3）✓ / `mypc fmt --check` ✓ / `mypc --emit-llvm` 产出 .ll ✓ / 测试框架
  退出码 1 + 异常隔离 + 汇总 ✓ / 项目结构 ✓ / `MYP_PACKAGE_PATH`（包管理器读取）✓。
- **回归**：新增 `tests/test_multifile.sh`（4 用例：跨文件函数 / 第二文件 import env /
  第二文件 struct+enum+@test / 多文件 @test+用户 main），已接入 `tests/run_tests.sh`
  （`测试框架` 小节）；全量 288 通过（3 个自举工具 build/ 缺二进制的既有环境失败）。


### v3.12.19 — 手册 §10 模块与导入审计：去重 `..` 不规范化（BUG-024）+ 点分模块名补文档
- **新 bug BUG-024**：相对路径导入去重**不解析 `..`**——同一文件经不同相对路径
  （直导 `./helper.myp` + 子模块内 `../helper.myp`）`normalizePath` 后仍不同
  （`/mod/helper.myp` vs `/mod/sub/../helper.myp`）→ 双重载入 →
  `duplicate class name`/`duplicate function name`。design §9 声称「规范化路径去重」
  未真正实现（`normalizePath` 只清 `.`/`//`）。复现 `tests/bugs/relimport_dedup.myp`。
- **文档补齐**：manual §10 导入语法加点分模块名（`import gpu.hal;` → `stdlib/gpu/hal.myp`，
  实测可用但原文档未提）；导入规则加去重注记（按路径字符串、`..` 未规范化）。
- **§10 逐条实测通过**：标准库导入 env/timeline ✓ / 点分模块名 gpu.hal ✓ / 相对路径
  `import "./helper.myp"` ✓ / 绝对路径 ✓ / 同串去重 ✓ / 递归加载 ✓ / 搜索路径
  （--stdlib → ../stdlib/ → ./stdlib/ → --package-path）✓ / 包导入
  （`import foo;` + `--package-path`）✓。
- **回归**：`tests/@test/manual_ch10_myp.myp`（1 test / 2 断言：相对+同串去重+递归+
  点分模块名）+ helpers/ch10_sub.myp；全量 285 通过（3 个自举工具 build/ 缺二进制的
  既有环境失败）。

### v3.12.18 — 手册 §11 标准库抽查：API 全部准确，固化综合回归
- **§11 逐条抽查（编译+运行）**：option（Option()/Option(v)/isSome/get/getOr/set/clear）/
  collections（ArrayList/HashMap/Set）atomic（addInt/subInt/addDouble/xchgInt/loadInt/
  storeInt——确认无 loadDouble，与文档一致）/barrier/future/rtti（typeOf/typeId/
  sameType）/fmt（i/u/x/X/o/b/f/e/g/s/sR 精确输出）/crypto（crc32/md5/sha1/sha256 已知
  向量）/json（getString/getInt/path）/base64/date/regex/args/process/memory（alloc/
  realloc/free/liveObjectCount）——**全部与 manual 文档化签名一致，无文档错误**。
- 先前会话已核对的 §11 部分（env/time/timeline/random/rtti/fmt/crypto/http/net/text/
  atomic/collections/option/result/sync/barrier/future/coro/async/pool/memory/channel/
  fs/process/args/json/regex/base64/date/logger/ui/gpu + math/io/stream/sdl 修正）维持
  不变。
- **回归**：`tests/@test/manual_ch11_myp.myp`（9 tests / 46 断言）；全量 285 通过
  （3 个自举工具 build/ 缺二进制的既有环境失败）。

### v3.12.17 — 手册 §9 并发编程审计：@parallel/@gpu 属性访问 bug + @threadpool 示例修正
- **文档错误修复**：manual §9 `@thread` / `@threadpool` 示例的 mapping 用实例变量名
  节点（`sensor.valueRead -> worker.process`、`sensor.valueRead -> pool[0].process`）
  ——实例名节点函数内 mapping 无法编译（BUG-011），`pool[0].process` 数组下标节点
  parser 直接报错（`expected instance/class name in mapping`）。已统一改为**类名**
  节点（`Sensor.valueRead -> Worker.process`），与 §8 一致。
- **新 bug BUG-023**：`@parallel for` / `@gpu for` 并行体**直接访问 class/static
  属性数组** → LLVM verify 失败（`getelementptr i32, i64 0` GEP 基址为整数 0）/
  `Atomic.addInt` 时运行段错误 139。并行体只捕获外层局部变量；属性访问需先拷到
  局部（manual §9 BNCT 示例 `double[] depthDose = TallyData.depthDose` 已用该模式）。
  manual §9 @parallel for 限制与 @gpu for 限制均加说明。复现
  `tests/bugs/parallel_prop_access.myp`。
- **§9 逐条实测通过**：@thread（独立线程 @startup）/@threadpool（4 worker 启动）/
  @parallel for（int+long 循环变量 + Atomic.addInt，sum=499500；manual 精确示例）/
  sync 同步原语（Mutex/RWLock/CondVar/Semaphore/Once，tests/sync 覆盖）/@gpu for
  CPU 回退（sqrt(4)+sin(1)=2.84147）/import cuda Vectors（add/scale/sum，CPU 回退）/
  构造器/@startup。
- **回归**：`tests/@test/manual_ch9_myp.myp`（3 tests / 11 断言：@parallel for 三种/
  sync API/@gpu+vectors CPU 回退）；全量 283 通过（3 个自举工具 build/ 缺二进制
  的既有环境失败）。

### v3.12.16 — 手册 §8 事件与 Mapping 审计：mapping 节点统一类名
- **文档错误修复**：manual §8（及 §5 main 规则、§13 完整示例）的 mapping 示例
  全部用**实例变量名**节点（`sensor.valueRead -> display.show`）——实测函数内
  mapping 用实例名节点 → LLVM verify 失败（BUG-011，`%tgt = load ptr, ptr
  %display`）。已统一改为**类名**节点（`Sensor.valueRead -> Display.show`），
  与 design.md §7.2 及现有全部测试一致；§8 加注「mapping 节点一律用类名，即使
  声明在函数内（实例级）也如此」。
- **§8 逐条实测通过**（类名节点）：事件声明/类型级映射/实例级映射（函数内类名
  节点）/事件链返回值（`A.event -> B.process -> C.onResult`，result=10）/
  多目标映射（`-> a, b`）/`@scope` 解注册（run 返回后 handler 自动注销，实测
  AFTER 不再触发）/`where` 条件过滤（`where v >= 3`，2 被滤）/`lambda` 变换
  节点/`delay(ms)` 延迟转发/`throttle(ms)` 限频（3 连发只留第一个）。
- **§13 完整示例（iot_monitor.myp）**：类名节点修复后**编译通过**（此前实例名
  节点无法编译）。
- **回归**：`tests/@test/manual_ch8_mapping.myp`（3 tests / 6 断言：类型级/
  事件链/多目标/lambda + where 精确过滤 + 实例级）；升级 `tests/delay_throttle/
  test.myp` 真正用 delay/throttle（原测试未用）+ 更新 expected + 新增
  `throttle_drop.myp`（丢弃语义锁定）；全量 283 通过（仅 3 个自举工具因 build/
  缺二进制 exit 127 的既有环境失败）。

### v3.12.15 — 手册 §7 审计修复：`this.field = value` 写 + 文件级限定 struct 定义
- **BUG-019（C++ codegen）**：`this.field = value`（struct 方法与 class 方法）此前
  编译报 `not a valid assignment target`。根因：`generateAssignment` 的 `if (!op)` 块
  闭合花括号错位，把「struct 方法 this.field」与「class this.prop」两个分支（都需
  **非空** `op` = this 指针）误嵌套进 `if (!op)`（~1988–2271）；`this.x = v` 时 op
  非空 → 整块跳过 → 落到错误。修复：`if (!op)` 在链式/数组元素分支后闭合，struct/
  class 分支移到块外。自举 `myp_self` 本就支持 → C++ 专属。验证：
  `tests/@test/manual_ch7_struct.myp` t_this；`tests/test_smart_building.myp`（大量
  `this.count = s`）转绿；全量回归 281 通过（3 个自举工具因 build/ 缺二进制 exit 127
  的既有环境失败，无关）。
- **BUG-020（C++ parser）**：文件级限定 struct 定义 `struct A::B { }` 此前报
  `expected struct name`（EBNF 与自举 parser 均支持 → C++ 专属）。根因：顶层 struct
  分发 `current_--` 回退到 `struct` 关键字后调 `parseStruct()`，其内部限定检查
  `check(Identifier)` 看到的是关键字而非名称 → 限定分支永不命中。修复：删除回退，
  直接 `parseStruct()`。验证：`tests/@test/manual_ch7_struct.myp` t_nested_qualified。
- **新发现未修复**：BUG-021 class 含泛型类属性（`Option<int>` 等）时 `this.prop`
  sema 解析污染（`class 'X_inst' has no member`，复现 `tests/bugs/this_generic_prop.myp`）；
  BUG-022 `@thread` 用于 struct 实例被静默接受（应拒绝却接受）。均登记
  `tests/BUGLIST.md`。
- **§7 审计回归**：`tests/@test/manual_ch7_struct.myp`（7 tests / 12 断言）覆盖
  文件级 struct/方法/var 推断/this 读+写/返回 struct/兄弟方法/嵌套 struct
  （类内 + `Sensor::Config` 引用 + 文件级限定定义）/struct vs class 值拷贝与引用。

### v3.12.14 — 自举编译器 P3-4/P2 向量化缺口闭合：opt 加 `-mtriple` 启用 TTI
- **症状**：myp_self2 编译产物平均慢 mypc ~14%，最坏数值循环 matmul **2.43x**（SSE2
  `mulpd`/`addpd` vs 纯标量 `mulsd`/`addsd`）。
- **根因（反汇编 + pass-remarks 定位）**：外部 `opt` 无 target machine → 未注册
  TargetIRAnalysis(TTI) → **LoopVectorizer 没有 cost model**，所有循环被判
  “vectorization is not beneficial” 保持标量。mypc 进程内管线给 `PassBuilder` 传
  TargetMachine 故能向量化。`--enable-unsafe-fp-math` 是遗留 flag 不生效；`-O3` 也只
  部分改善（matmul 50→29ms）不向量化。
- **修复**：`link.myp` 的 `opt` 调用加 `-mtriple=<host>`（新增 `findHostTriple()`：
  `llvm-config --host-target` 探测、回退 uname、`MYP_LLVM_CONFIG` 可覆盖）。
- **效果**：matmul **50ms→20ms（2.43x→1.00 持平 mypc）**；多数基准 0.96–1.14
  （dot_f64 0.80、convolution 0.96、nqueens 0.99，部分反超）。
- **正确性**：verify 全一致；run-compare PASS=148 FAIL=0（无输出变化）；
  run_tests 274/275（仅已知 arc_throw -O2 缺陷）；bootstrap 不动点保持。

### v3.12.13 — 自举编译器 P3-2 去委托：`myp_self run`/`fmt` 原生化（不再 shell 到 mypc）
- 此前 `myp_self run`/`fmt` 经 `delegateToMypc` shell 到 C++ mypc。现改为自托管：
  - **`myp_self run <file.myp> [args...]`**（仿 go run）：原生编译+运行+清理，退出码透传。
    无 main 时注入合成 main（单类 @startup）——移植 C++ `Sema::injectAutoMainIfNeeded`
    到自举 sema（新增 `autoMain_` 标志，合成 main 豁免 main() 直接调用限制）；无 @startup /
    多 @startup 报错与 mypc 一致；临时产物（二进制/.o/.ll/.opt.ll）运行后清理。
  - **`myp_self fmt [--check] <file.myp> ...`**：改调自举格式化器 `myp_fmt2`
    （MYP_FMT env 覆盖，缺失时现场用自身编译 tools/fmt/main.myp）。
  - 删除 `delegateToMypc`/`findCompiler`；stdlib 解析统一走 `Cli.selfStdlib()`
    （MYP_STDLIB env → 相对 myp_self 二进制 → cwd 兜底）。
- 验证：test_myp_run.sh 8/8（self2 与 mypc 双跑）、test_myp_self.sh 94/94、
  bootstrap 16/16（不动点保持）、run_tests.sh 274/275（仅已知 arc_throw -O2 缺陷）、
  run-compare PASS=148 FAIL=0 GAP=1(sdl_demo) SKIP=17（无回归）。

### v3.12.10 — 自举编译器接入 LLVM 中端优化（`opt -O2`）
- 自举编译器此前**无任何 IR 优化**：codegen 只发 alloca 形态文本 IR（0 phi，局部
  变量全走栈内存），`llc` 无 `-O`（仅后端 codegen）→ 生成程序性能差。
- **`link.myp` 在 llc 前插入 `opt -O2` 步骤**（mem2reg/SROA/GVN/instcombine/内联）：
  新增 `findOpt()`（opt-21/opt-20 探测，`MYP_OPT` env 覆盖）；**默认开启**，
  `MYP_SELF_OPT=0` 关闭（调试/对比）。实测 opt 后 IR alloca 6→0（全部提成 SSA）。
- **效果对比**（生成程序执行，3 次取最小）：
  - 计算密集循环：46ms → **2ms（23x）**
  - raytracer：1023ms → **326ms（3.1x）**
  - 编译整个自举链：6441 → 7998ms（opt 步骤开销 **+24%**，换取生成的程序 3-23x）
- 正确性：hello/fib/raytracer/showcase/GPU scatter 输出（含 verify 值与 GPU kernel
  launch）与 opt 关**完全一致**；test_myp_self 94/94。
- 权衡：编译 +24% ↔ 运行 3-23x；生产用默认开，快速编译用 `MYP_SELF_OPT=0`。

### v3.12.12 — 自举 codegen：struct 数组元素大小 bug（`new Vec[n]` 分配 8 字节/元素）
- **症状**：soft2 编译 `dotprod`/`slicevec`（`Vec{x,y,z}` struct 数组/切片点积）
  段错误；mypc 正常。
- **根因**：codegen 的 `llvmType(struct)` 返回 `%Vec`，但 `IrEmit.typeSize("%Vec")`
  落**默认分支 8**（ptr 大小）→ `new Vec[n]` 分配 n×8 字节，GEP 却按 `%Vec`（12
  字节/元素，3×i32）索引 → 越界写。类数组（元素是指针）碰巧 8 正确，struct 数组
  （元素内联）错。
- **修复**：新增 `typeByteSize`/`typeAlign`/`structByteSize`/`structAlign`（struct
  按 LLVM 自然对齐递归布局：字段对齐放置、总大小对齐到最大字段对齐），替换 3 处
  `myp_alloc_slice_backing` 的 `IrEmit.typeSize`（`new T[]`、`new slice<T>`、
  `fixedArrayToDynamic`）。
- **验证**：dotprod/slicevec verify 4992059535 与 mypc 一致（ms 8/8 vs C++ 15/16）；
  不动点 stage1==stage2 保持（md5 13a4065b）、test_myp_self 94/94、bootstrap 16/16。
- **附带**：本次用 soft2（stage1 自编译 myp_self）跑全量测试 + C++/Go 基准，见下。

### v3.12.11 — 自举编译提速：M4 原地拼接 + 残留 O(n²) 清扫 + 单趟 `myp_str_join`
- **问题**：自举编译器编译自身很慢（`--frontend-dump sema main.myp` 合并全链约
  96s+）。三个叠加的 O(n²)：
  1. **`StringBuilder.toString` 逐片段 `result = result + x`**：C++ oracle 有 M4
     优化（`s = s + x` 同变量累加发射 `myp_str_append` 原地 realloc），自举
     codegen **没有** → 一律 `myp_strcat` 全量拷贝 → toString O(n²)。
  2. **字符串无长度字段**：`myp_str_append` 每次 `strlen(已累加串)`（O(len)）→
     即使拷贝被省，累加仍 O(n²)。
  3. **per-char `__myp_ord(Str.substring(s,i,i+1))`**：dump/parse/codegen 转义与
     解析路径残留（每个字符 O(n) strlen）。
- **修复**：
  1. **M4 移植到自举 codegen**（对齐 C++ codegen_stmt.cpp M4）：`Assign` 分支识别
     `s = s + x`（左侧同变量字符串局部）→ 发射 `myp_str_append`（rc==1 原地
     realloc，O(1) 均摊）+ `ir_emit.myp` 补 `declare ptr @myp_str_append`。
     **消除 stage0/自举 codegen 行为差异**（性能保真）。
  2. **`myp_str_join(char** arr, int32 n)` 单趟拼接**（runtime.c）：`StringBuilder.
     toString` 改为一次分配 + 两趟（长度求和→拷贝）O(n)，消灭 1.57TB strlen。
  3. **per-char substring → `__myp_charcode`**：15 处（ast Dump.esc、main escape/
     dotToSlash/normalizePath/stripExt、codegen isDigits/escapeLlvmString、
     parser parseInt/插值、sema isIntegerStr、ir_emit parseNum）。
- **实测（编译合并全链 main.myp，3 次取最小）**：
  - sema dump：~96s → **1.7s（56x）**；ast dump：11.9s → **0.47s（25x）**
  - 编译 main.myp：4.4s → **3.1s**；ast dump 内 strlen 扫描 **1.57TB → 54MB（29,000x）**
  - bootstrap 全 3-stage 测试：含 sema dump 阶段从几分钟 → **20s**。
- 修复 `test_myp_pass.sh` 既有 sed 模式 bug（`define internal i32 @compute(...)`
  匹配不上 → 基线误报 0 store）→ PASS 6/6。
- 回归：bootstrap 16/16（不动点 myp_self2==myp_self3 md5 991cec87）、test_myp_self
  94/94、run_tests 275/275、test_myp_gpu 60/60、test_myp_pass 6/6。O2 套件 `arc_throw`
  失败为**既有** -O2×异常展开问题（不经过本次改动的代码路径，见下）。
- 残余（已知）：用户代码里裸 `s = s + x` 累加仍 O(n²)（`myp_str_append` 无长度
  字段每次 strlen）；根治需给字符串头加长度字段（影响 runtime 布局 + 两个 codegen
  字面量，另立任务）。

### v3.12.9 — 自举编译器词法 O(n²) → O(n)（`__myp_charcode` + 缓存长度 + fillLineCol 双指针）
- **根因**：词法器 `peek()/advance()/match()/ordAt()` 对每个字符调用
  `__myp_ord(Str.substring(source_, pos_, pos_+1))`，而运行时 `myp_str_substring`
  每次 `strlen(source_)`（O(n)）→ 每字符 O(n)，整文件 **O(n²)**；`Str.len` 也是
  strlen，守卫里同样 O(n)。
- **修复**：
  1. 新增 **`__myp_charcode(str, i)`** FFI → 运行时 `myp_charcode`（`(unsigned char)s[i]`
     直接下标，O(1) 无 strlen；负数/空返回 0，上界由调用方保证）。C++ oracle
     （sema.cpp/codegen.cpp/CodeGen.h）与自举（sema.myp/ir_emit.myp）同步注册。
  2. 词法器构造时**缓存 `len_`**（一次性 strlen），光标函数改 `__myp_charcode` +
     `len_`，消除逐字符 strlen。
  3. **`fillLineCol` 双指针**：token 按 begin 单调递增 → 行起始指针只前进，
     O(tokens×lines) → O(tokens+lines)。
- **实测（自举编译自身源码，3 次取最小）**：
  - codegen.myp(9995 行) tokens：11086→**672ms（16.5x）**；编译 13304→**2570ms（5.2x）**
  - main.myp（合并全链 ~2.3 万行）编译：19747→**4387ms（4.5x）**
  - tokens 规模扫描由 O(n²)（行数×2 → 时间×4）转为近线性（×2 → ×2.7-3.3）。
- 残余：`--frontend-dump ast/sema` 仍偏慢（dumps 的 `StringBuilder.toString` 是
  O(总输出²)，验证模式）；小文件 myp_self 本就比 mypc 快（启动开销低）。
- 回归：test_myp_self 94/94（字节级对拍保持）、test_myp_gpu 60/60、run_tests 275/275。

### v3.12.8 — 自举编译器 `MYP_LLC` 环境变量覆盖 llc 路径
- `link.myp` `Link.findLlc()` 先读 `MYP_LLC`（非空即用），再回退既有 llc-21/llc-20
  探测 → `llc`（PATH）。跨机器/任意 LLVM 版本无需改源码。
- `codegen.myp` 重复的 `findGpuLlc()` 收敛为委托 `Link.findLlc()`（单一来源），
  GPU/NVPTX 阶段同样支持 `MYP_LLC` 覆盖。
- 验证：默认探测与 `MYP_LLC=/usr/lib/llvm-21/bin/llc` 产物一致；`MYP_LLC` 指向
  不存在路径 → `llc failed`（证明覆盖生效）；GPU scatter 仍真机 launch + PASS；
  test_myp_self 94/94。

### v3.12.7 — 自举编译器清理：删除 5 个空壳占位源文件
- **删除 `tools/selfhost/src/` 下 5 个 F0 时代空壳占位 `.myp`**（各仅 3 行注释
  "当前为空壳（F0 占位）"、不被任何源码 import、无实际代码）：
  `parser_expr.myp` / `type.myp` / `codegen_expr.myp` / `codegen_stmt.myp` /
  `codegen_class.myp`。其职责已分别并入 `parser.myp`（含表达式）/ `sema.myp`
  （含类型表示/提升）/ `codegen.myp`（表达式/语句/类/ARC/异常/泛型/mapping）。
- **同步更新**：`CMakeLists.txt` `MYP_SELF_MODULES` 移除 5 项（原仅作 DEPENDS）；
  `tools/selfhost/README.md` 目录树、`design.md` §4.3/§5 模块表、`roadmap.md`
  总览表与 F3/F4a/G2/G3 小节（标注"实现并入"）。`docs/CHANGELOG.md` 自包含。
- 清理源码树陈旧构建产物（`*.myp.o`、`codegen.out.ll/o`，gitignored）。
- 验证：`myp_self` 重建成功；`test_myp_self.sh` 94/94 全绿（import 链不受影响）。

### v3.12.6 — 自举编译器 `@gpu scatter/reduce/scan` GPU kernel 真机发射（补齐全部 GPU 构造）
- **自举编译器（tools/selfhost）`@gpu scatter` 真正的 GPU kernel**（§8.4）：
  - 固定 **grid-stride 散点 kernel**（无用户 body，直接生成 .ll）：`p ∈ [0, n)`
    grid-stride，`j = idx[loi+p]`（i32→i64 sext），`b[j] = a[loa+p]`；冲突模式
    `atomic_add` 用 **`atomicrmw`**（float `fadd`、i32/i64 `add`，真原子，修掉此前
    load+add+store 的竞态）；`double` 无原生 f64 原子 → 自动 CPU 回退。
  - host 发射双路径（6 参 `(n, loa, loi, a_dev, idx_dev, b_dev)`，grid=ceil(n/block)）；
    标量/数组 arg 全部经 alloca 槽传给 `myp_gpu_launch`；b 的 D2H+free 在 launch 后。
  - 真机验证：6 次 launch，unique/any/atomic_add/slice/int_atomic 全 PASS。
- **`@gpu reduce` 真正的 GPU kernel**（§8.2/§8.6）：
  - **K1 kernel = per-block 串行归约**（`bid=ctaid.x`，块 `[lo+bid*bs, min(..+bs, lo+n))`
    顺序 fold，opExpr 在 kernel 内求值，acc/x 绑定 kernel 局部 alloca）→ `partials[bid]`；
    host 再以 init 顺序折叠 partials（同 opExpr，host 上下文 bindGpuOpLocals）。
  - **CPU 回退改为同构规范分块归约**（§8.6 位级一致）：L1 每 blockSize 顺序部分和 +
    L2/L3 顺序合并——GPU 与 CPU 双跑 **bit 级一致**（`test_gpu_reduce_bit` diff 通过：
    `reduce_bits=1177075684` 等逐字节相等）。
  - 真机验证：sum/slice/max（block(128)）3 次 launch（grid 4/1/8 正确分块）全 PASS。
- **`@gpu scan` 真正的 GPU kernel**（§8.3）：
  - **GPU 两遍**：K1（复用 reduce 同款 per-block 和 kernel，写 blockSum[bid]）→ host
    顺序折叠 `blockPrefix[bid] = init∘blockSum[0..bid-1]` → K2 块内 scan kernel
    （线程 0 串行块内前缀，加 `bp[bid]` 块前缀偏移；inclusive/exclusive 两种写序）。
  - host 折叠需 **ngrid+1 项** bpHost 缓冲（折叠写 bpHost[0..ngrid]，修掉此前越界 1 项
    导致的 glibc `double free or corruption`）。
  - 真机验证：5 个 scan（full/slice/exclusive-full/exclusive-slice/exclusive-init5）
    各 2 次 launch，GPU/CPU 结果完全一致，全 PASS。
- **实现**：`tools/selfhost/src/codegen.myp` 新增 `genGpuScatterKernel`/
  `emitGpuScatterModule`、`genGpuReduceKernel`/`emitGpuReduceModule`、
  `genGpuScanKernel`/`emitGpuScanModule`；`genStmt` 的 GpuScatter/GpuReduce/GpuScan
  分支改为先试 kernel 再回退；`genGpuReduce` 重写为规范分块归约。
- **真机验证汇总**（本机 NVIDIA GPU + CUDA 13.2，`MYP_GPU=1`）：kernel_ctx/stride/vec4/
  tile/tile_degrade/scatter/reduce/reduce_bit/scan 全部真实 launch + PASS（launch 计数
  scatter=6、reduce=3、scan=10）；`test_gpu_block` 的 tile+`Math.abs` 段仍 CPU 回退
  （已知限制：自举 tile 内 Math.abs 解析到宿主静态函数，kernel 无定义）。
- 全量回归 275/275，test_myp_gpu 60/60，test_myp_self 94/94（CPU 模式）。

### v3.12.5 — 自举编译器 `@gpu for` GPU kernel 真机发射（NVPTX 内核 + PTX 嵌入 + 运行时 GPU/CPU 双路径）
- **自举编译器（tools/selfhost）`@gpu for` 真正的 GPU kernel 发射**（此前只有 CPU 回退）：
  - codegen 阶段为规范 `@gpu for`（无 resident/stream/stride、`i < B`/`i <= B`、捕获动态数组）
    生成 **NVPTX kernel**（独立模块，`gid = blockIdx*blockDim+threadIdx`），用外部
    `llc -mtriple=nvptx64-nvidia-cuda` 编译出 PTX，经 `!nvvm.annotations` 标记
    `.visible .entry` kernel，把 PTX 作为字符串全局嵌入主模块。
  - host 发射运行时 **GPU/CPU 双路径**：`myp_gpu_init()` → GPU 可用则
    `myp_gpu_load_kernel` + `myp_gpu_alloc`/`myp_gpu_to_device` + `myp_gpu_launch`
    （grid=ceil(n/block)，block 默认 256 或 `block(n)`）+ `myp_gpu_to_host`/free +
    destroy；否则跳 `gpu_cpu_fallback` 块跑串行回退。数组字节数从 ARC 头
    （obj-24=count、obj-16=elem_size）读取。
  - kernel body 内 `kernel.gid/tx/bx/bd/gx` 复用 CPU 回退的 `genKernelMember` 映射
    （gid=gid、tx=gid%bd、bx=gid/bd、bd=block、gx=ceil(n/bd)）；`kernel.sync()` 空操作。
  - 失败（llc 拒/复杂 body 引用宿主局部）→ 自动 CPU 回退，临时 .ll/.ptx 无论成败清理。
  - 实现：`tools/selfhost/src/codegen.myp` 新增 `genGpuKernel`/`emitGpuKernelModule`/
    `collectGpu{Expr,Stmt}Arrays`/`isDynamicArrayVar`/`findGpuLlc` 等；For 分支抽取
    `genSerialFor`。
  - **真机验证**（本机 NVIDIA GPU + CUDA 13.2，`MYP_GPU=1`）：`kernel_ctx`/`vec4`/
    `math_float`/`static` 均真实 launch kernel 且结果 PASS（`CUDA initialized` →
    `launching kernel` → `kernel done`）；无 `MYP_GPU` 时 CPU 回退结果一致。
  - 范围：`@gpu stride`/`tile`/`reduce`/`scan`/`scatter` 的 GPU kernel 留后续
    （继续 CPU 回退）；`test_gpu_block` 的 tile 段在 GPU 模式因自举 tile 仍为 CPU
    单线程降级（读未写 smem）而 FAIL（测试注释已知，仅 GPU 后端路径）。
- 全量回归 275/275，test_myp_gpu 60/60，test_myp_self 94/94。

### v3.12.4 — 自举编译器 GPU CPU 回退（`@gpu` 语句全量串行对齐 oracle）+ `float4` 向量类型
- **自举编译器（tools/selfhost）GPU CPU 回退落地**——`@gpu for/tile/reduce/scan/scatter`
  不再静默跳过，全部按与 C++ oracle 一致的 CPU 回退语义执行：
  - `@gpu for` / `@gpu stride for`：串行执行 + **模拟 `kernel.*` 上下文**
    （`gid`=循环变量、`bx`=p/block、`tx`=p%block、`bd`=block、`gx`=ceil(bound/bd)）；
    `@gpu stride for` CPU 回退 step 改 +1 顺序遍历（§3.5，同 oracle）。
  - `@gpu reduce`：顺序 fold `out = fold(init, a[lo..hi))`（§8.2）。
  - `@gpu scan`：顺序前缀扫描，inclusive/exclusive + 非零 init（§8.3）。
  - `@gpu scatter`：顺序散点 `b[idx[lo_i+i]] = a[lo_a+i]`，any/unique/atomic_add
    （float fadd / int add 累加）（§8.4）。
  - `@gpu tile`：单线程降级——smem → 宿主栈数组、遍历展平线程网格
    `p ∈ [0, grid*block)`、`kernel.sync()` 空操作（§8.5）。
  - `kernel.sync()` 空操作；`kernel.shfl_down(v,d)`/`kernel.block_reduce_{sum,max}(v)`
    恒等返回 v；`kernel.printk` 空操作；`kernel.assert` 失败 `myp_assert_abort` → exit(1)。
  - 实现：`tools/selfhost/src/codegen.myp` 新增 `genGpuReduce/Scan/Scatter/Tile`、
    `genKernelMember`、`bindGpuOpLocals`、`gpuArrayLoad/Store` 等 + `gpuCpuFallback_` 状态。
- **`float4` 向量类型落地**（§3.6）：`IrEmit.llvmType`/`kindType` 映射
  `float4`→`<4 x float>`、`double2`→`<2 x double>`、`int4`→`<4 x i32>`；`load4`/`store4`
  打包读写（元素偏移 i*4，align 4）；`v.x/y/z/w` 读 `extractelement`、写 `insertelement`；
  `zeroValue`/`alignOf`/`typeSize` 向量分支。
- **回归接入**：新增 `tests/test_myp_gpu.sh`（CPU 回退模式，60 项检查），
  `run_tests.sh` 新增第 10 部分 `RUN_GPU_TESTS=1` 可选启用。全部
  `tests/test_gpu_*.myp` 与 oracle CPU 回退输出对齐（reduce/scan/scatter/tile/kernel_ctx/
  vec4 数值逐项一致）；全量回归 275/275（`RUN_GPU_TESTS=1` 下 276/276）。
- **已知遗留（非本次引入）**：`stdlib/gpu/algo.myp` 的 `GpuAlgo.sort`（嵌套 while +
  `@gpu for`）在自举编译器产物中段错误（旧编译器同样 139，与 GPU 回退无关）；
  已记录待查。

### v3.12.3 — class property 私有化（破坏性语义变更）+ 自举编译器两级自举成立 + Bug 跟踪框架
- **⚠️ 破坏性变更：class `property:` 现为私有**——外部实例访问（读+写）→ 编译错误
  `cannot access property 'X' of 'Y' from outside the class`。此前 sema 允许外部读
  （"Properties — accessible from anywhere"），但 codegen 只正确支持 `this.prop` 与
  单级读，链式 `o.mid.inner.val` 产出垃圾值/段错误、链式写崩溃（BUG-001）。
  - 仍允许：`this.prop`、**同类另一实例**（C++ 私有成员语义，如 `GpuBuffer` 内
    `src.host_`）、`@static class` 的 `Class.prop`。
  - `struct` 字段不受影响（公开可读写）。
  - 修复位置：C++ oracle `sema_expr.cpp` + 自举 `tools/selfhost/src/sema.myp` 双侧同步。
  - 负测试：`tests/negative/external_property_{read,write,chain}.myp`。
- **自举编译器（tools/selfhost）两级自举成立**：
  - F0–F4（前端 oracle/词法/AST/表达式 parser/语义分析）、G1–G4（IR 发射/语句表达式
    codegen/类 ARC 异常泛型/驱动链接）、H1（两级自举验证）全部 ✅。
  - 自举 AST 纯数据 class 迁 getter 访问：跨实例直接读 `e.lhs_` → `e.lhs()`，
    新增 ~360 getter、改写 ~2900 处访问（关键字冲突字段 `ref_→isRef()` 等 8 特例 +
    无下划线字段 `AstPair.k/v`、`AstNonlocalSlot.slot/cell` 改名）。
  - 验证：`test_myp_self.sh` 94/94、`test_myp_bootstrap.sh` 15/15；
    全量回归 270 通过、仅剩 BUG-003 导致的 `generic_traits` 一处不一致。
- **Bug 跟踪框架**：`tests/BUGLIST.md` + `tests/bugs/`（@test 复现 + `run_bugs.sh`）。
  已登记 BUG-002（@coro 增量 spawn）、BUG-003（泛型 string 比较）、BUG-004（`Option<struct>`）。
- **BUG-003 修复：泛型 `T=string` 的 `<`/`>` 按指针比较**——codegen string 比较判定
  新增 `exprResolvedString(e)`（`resolved_kind==String` 优先，泛型类型参数按 alloca 指针
  类型兜底），并让 `exprIsString` 排除动态数组（`T[]` 误判）。`tests/bugs/generic_string_cmp.myp`
  6/6 转绿、`tests/generic_traits` 回归转绿；全量回归 273 通过（`coro_stack` 为既有
  flaky：深递归 3000 层恰在 2048KB 栈边界，非本 bug 引入）。
- **BUG-002 修复：@coro 参数/`this` 悬垂（增量 spawn 帧损坏）**——@coro 方法/函数的
  类引用参数（及 `this`）此前被借用不 retain，协程比调用方作用域长寿 → 主流程释放并
  复用 Channel 对象后，park 中的过滤器读 `in.handle_` 得新对象句柄 → 过滤链错位、
  复合数漏过。codegen 新增 `registerCoroParam`：@coro 入口 retain `this` 与所有 ARC
  参数（class/interface/function/slice/dyn-array/string/含 ARC 字段的 struct），注册为
  作用域槽（正常完成释放）+ 镜像进协程帧注册表（destroy/异常释放）。`tests/bugs/
  coro_incremental_spawn.myp`（go 素数筛）8/8 转绿；全量回归 273 通过。
- **BUG-004 修复：`Option<struct>` 泛型实例化**——struct 字段 `Option<Node>` 此前在
  `generic_classes_` 注册前解析，落到未实例化模板名 `Option` → 赋值/成员访问类型错。
  sema 将 generic 模板注册提前到 struct 字段校验前 + 类声明循环跳过 `is_generic_inst`；
  codegen `memberObjectClassName` 用 `resolved_object_class` + struct 字段类型兜底分发
  到 `Option_Node_inst_*`。`tests/bugs/option_struct.myp` 2/2 转绿；全量回归 274 通过。
  为后续“递归纯数据迁 struct、去掉 class+getter 折中”铺平道路。

### v3.12.2（当前）— 类型系统增强（P0/P1/P2）+ 多态数学 intrinsic（§9.5）+ GPU `__nv_xf` 选型 + 共享 emitConversion
- **类型系统增强（type_system_design §3-§7/§9，P0/P1/P2 全部落地）**：
  - **单一转换权威（§7.1）**：`convertIntegerValue` 收敛 5+ 处内联转换（赋值/属性/数组元素/return/调用实参/变量初始化），无符号源统一 ZExt（修 D1：`long z; z = 0xFFFFFFFFu;` 不再 `-1`）。
  - **隐式转换格重写（§3.2，无损隐式/有损显式）**：移除 `Int/Long→UInt`、`Int/Long→Float` 隐式与 `char↔byte` 互换；`i64/u64→f64` 改显式；`ulong` 补全（小无符号→大无符号隐式 ZExt，跨符号/浮点显式）。
  - **bool 入转换链（D6）**：`int(b)`=b?1:0、`bool(n)`=n≠0、`bool(f)`=f≠0；隐式 bool↔整型仍禁。
  - **char=u8 语义定稿（D7）**：byte=有符号 i8、ubyte=无符号 i8、char=u8 语义别名（0xFF→255 非负）；char 字面量生成 i8；三处符号矛盾消除。
  - **string 转换统一（修 D2/D3/D4）**：`"x"+f32` 不再编译崩溃、无符号拼接成无符号十进制、char 拼接输出字符；runtime 新增 `myp_to_string_u32/u64/float`。
  - **string 能力**：比较操作符 `< <= > >=`（词法）、`s[i] : char`、`bytes(s)`/`str(bytes)`（string↔ubyte[]）。
  - **parse* 全族（§6.2）**：`parseInt/Long/Uint/Ulong/Float/Double(s)` 统一 strtol/strtoull/strtod 语义（`0x` 前缀，失败回 0）。
  - **位操作原语（§5.3）**：`popcount/clz/ctz/bitreverse/rotl/rotr`（LLVM ctpop/ctlz/cttz/bitreverse/fshl/fshr 直映，多态同宽返回）。
  - **bit + bitvector<N>（§5.1）**：`bit`=i1（`bit(x)`=x≠0）；`bitvector<8/16/32/64>`=iN——索引 `v[i]:bit`、`&|^<<>>`、`~` 取反、写索引 `v[i]=x`、`bitvector<N>(uint)`/`uintN(bv)` 互转、`bytesOf(bitvector<N>)`→ubyte[]。
  - **bitfield（§5.1）**：结构体位域打包（背衬整数 ≤8→i8/≤16→i16/≤32→i32/其余 i64）；读=位提取 bit/uint、写=读-改-写；支持类属性 `this.bf` 与数组元素 `arr[i].field`。
  - **bitcast<T,U>（§5.2）**：位保持重解释（同宽 8/16/32/64，跨宽显式错误）——`bitcast<uint>(1.0f)==0x3F800000`。
  - **泛型 where T : Trait（§9）**：内置数值 trait `Numeric/Integer/Float/Ordered` + 泛型函数/静态方法 `T f<T where T : Trait>`，实例化时约束校验（零运行时开销）。
  - **字面量增强（§4.3）**：下划线分隔 `1_000_000`/`0xFF_FF`/`1_000.5`/`1e1_0`/`1_000L`（lexer `scanNumber` 扫描剥离，parser 零改动）+ 二进制 `0b` / 显式八进制 `0o` 前缀（parser `parseIntegerLiteralValue` 统一解析；前导零 `0755` C 风格八进制保留）。
  - **checked 溢出变体（§4.2 P3）**：`checkedAdd/checkedMul(a,b)` 返回 `(value, overflow:bool)` 元组——`@llvm.sadd/smul.with.overflow.iN` 直映（有符号整型，公共类型提升），声明式解构与字段访问均可用；`tests/checked_overflow`。
  - **parseIntOpt（§6.2 P4）**：`parseIntOpt(s)` 返回 `(value:int, ok:bool)` 元组，用 `ok` 区分合法 `0` 与解析失败（`parseInt` 失败回 0 无法区分）；runtime `myp_str_parse_int_opt`；`tests/parse_opt`。
  - **manual.md §3 文档重写（P4）**：类型系统章同步最新落地——无损隐式/有损显式提升格、bool 入转换链、char=u8、显式转换（含 bool/bit）、新增 `bit`/`bitvector<N>`/`bitfield`/`bitcast`/位操作原语/`checkedAdd`/`checkedMul`/`parse*` 与 `parseIntOpt`/数值 trait 与 `Math` 多态、字面量 `0b`/`0o`/下划线；示例全部经 mypc 验证。
  - 测试：`tests/bitvector`、`tests/bitfield`、`tests/bitcast`、`tests/bit_ops`、`tests/parse_family`、`tests/generic_traits`、`tests/stringify_conv`、`tests/string_cmp`、`tests/string_subscript`、`tests/bool_convert`、`tests/char_semantics`、`tests/bytes_str`、`tests/unsigned_convert`、`tests/numeric_underscore` 等。
- **§9.5 多态数学 intrinsic + `Math` 库按 trait 重写**（§9.5 全部落地，CPU + GPU）：
  - `__myp_math_*` 一元实数/abs/trunc intrinsic 类型感知：sema 按实参类型定返回类型
    （f32→f32、f64→f64）；CPU codegen 按实参类型发 LLVM 标量 intrinsic
    （`llvm.sqrt.f32` 等；整型 `abs`→`llvm.abs.iN`、浮点→`llvm.fabs`；`trunc`→`llvm.trunc`）；
    GPU kernel 内按实参类型选 libdevice `__nv_xf`（float）`/__nv_x`（double），
    整型 `abs` 内联 `select(x<0,-x,x)`（返回同宽整型）。
  - `Math` 库泛型化：`T sqrt/exp/log/sin/cos/tan/asin/acos/atan/sinh/cosh/tanh/floor/
    ceil/trunc<T where T : Float>`、`abs<T where T : Numeric>`、`min/max/clamp<T where
    T : Ordered>`（int/double/string 通用）、`lerp<T where T : Float>`；`pow`/`atan2`
    保持 double。
  - **破坏性变更（标准库 API）**：`Math.trunc` 返回类型 `int`→`T`（T→T 向零取整）；
    删除 `Math.absInt`/`minLong`/`maxLong`/`clampDouble`（由泛型 `abs`/`min`/`max`/`clamp`
    取代）。迁移：`int(Math.trunc(x))`→`int(x)`；long 上下文 `Math.trunc(x)`→`long(x)`。
  - f32 数学不再需要 `float(Math.exp(...))` 样板（float 实参直接返回 float，精度/性能更好）。
  - 测试：`tests/math_traits`（CPU 泛型数学）、`tests/test_gpu_math_float`（GPU：float
    sqrt/exp-log 组合/double 回归/kernel 内 int abs/显式转换全 PASS）。
- **GPU/CPU 共享 `emitConversion`（§7.1 单一权威）**：GPU kernel 的 `ExprKind::Convert`
  分支改为调用共享自由函数 `convertIntegerValue`——消除 GPU 内重复转换逻辑（bool↔int/fp、
  char 无符号语义、fp↔int、float↔double、指针 bitcast 全覆盖，避免两套转换漂移）。
- **GPU 原语（gpu_library_design §9 P0 §3.1-3.3 落地）**：
  - **§3.1 `kernel` 执行上下文**：`@gpu for`/`@gpu tile` body 内隐式保留标识符
    `kernel.gid(=p)/bx(blockIdx.x)/tx(threadIdx.x)/bd(blockDim.x)/gx(ceil(n/bd))`（long）
    + `kernel.sync()`（void）；sema 拦截 + codegen NVVM intrinsic 直映；CPU 回退模拟
    （gid=p/tx=p%256/bx=p/256/bd=256）；`tests/test_gpu_kernel_ctx` GPU PASS。
  - **§3.2 `@gpu tile`**：`@gpu tile (T name[dim...]) [grid(nb)] { body }`——块内
    `__shared__`（addrspace 3、编译期常量维度、sema 48KB 上限校验）；协作 body +
    `kernel.sync()` 两阶段；CPU 单线程降级；`tests/test_gpu_tile` GPU PASS。
  - **§3.3 块同步**：`kernel.sync()` → `llvm.nvvm.barrier.cta.sync.aligned.all(i32 0)`
    → PTX `bar.sync 0`；sema 发散分支检查（if/while 内 sync → 警告，防死锁）。
  - **P1 ① 数学 intrinsic 混合方案（§6.3）**：`@gpu for` 内 math 映射改为**混合**——
    native（sqrt/fabs/floor/ceil/trunc）走 `llvm.*` 原生指令（零 libdevice），超越
    （sin/cos/exp/log/tan/pow/atan2 等）保留 `__nv_*`+libdevice（NVPTX 对超越 intrinsic
    无 libcall，纯 intrinsic 会 "no libcall available for flog"）；`tests/test_gpu_math_float`
    GPU 全 PASS。
  - **P1 ② HAL + CPU 一等后端（§7.5/7.6）**：`GpuHAL`（`active()` 探测 cuda→cpu、
    `isGpu/isCpu/vendor`）；`CpuBackend`（available 恒 1、alloc 伪句柄 1、copy no-op、
    流/事件 no-op、sync 恒 1）；`GpuBackend` 按 HAL 分派 cuda/cpu（`backend.myp`）；
    `GpuBuffer/GpuBufferF` 加 `host_` 属性 + CPU 分支（构造 host 直通、copyFromHost/
    copyToHost/copyFromBuffer/async 全逐元素直通）——CPU 成为一等后端；
    `GpuDevice.sync` CPU 模式返回 1；`tests/test_gpu_hal`（手动 @startup 测试，双模式
    PASS：CPU backend=CPU / GPU CUDA）、`tests/@test/gpu_paradigm` 适配双模式 gate
    （流/事件/驻留/kernel-ops 用 `GpuHAL.isGpu()` 判断）。
  - **bug 修复（@gpu for float 标量捕获）**：`analyzeGpuCapturedVars`/`analyzeGpuTileCapturedVars`
    标量类型判断缺 `isFloatTy()` → 捕获 float 标量被置为 i64 → kernel 参数类型错位 →
    GPU 结果垃圾（影响所有捕获 float 标量的 `@gpu for`，如 GpuOps.mapF 的 s 参数）；
    已加 `isFloatTy()` 修复，`gpu_paradigm` GPU 模式 mapBufF 全 PASS。
  - **P1 ④ `@gpu stream(s)`（§4.1，语言级）**：`@gpu for (...) stream(s)` 与
    `@gpu tile (...) stream(s)`——把 kernel 异步排队到 `GpuStream`（launch 不阻塞；
    stream==0 默认流保持同步）。语法（for/tile 子句）+ sema（校验 GpuStream 类型）+
    codegen（launch 点求值 `s.handle()` 传 `myp_gpu_launch(..., stream)`；捕获数组 D2H
    回拷在 stream 模式改用同流异步 `myp_gpu_to_host_async`）+ runtime（`myp_gpu_launch`
    加 stream 参数，stream!=0 去掉自动 `cuCtxSynchronize`）。`GpuEvent.record(s)` +
    `e.wait(s2)` 支持跨流依赖。测试 `tests/test_gpu_stream.myp` GPU PASS（双流并发、
    同流有序、事件跨流依赖）；回归 109/109、gpu_paradigm GPU 57/57（非 stream 同步路径
    不变）。推理框架 `runGpu` 接入（H2D/D2H 重叠）待 P1 ③ 融合后 ⏳。
  - **P2 ① `kernel.shfl_down(v, delta)`（§3.4 warp shuffle）**：`@gpu for/tile` body 内
    块/warp shuffle——sema 拦截（返回 v 类型，支持 double/float/int）+ GPU codegen
    （NVPTX `shfl.sync.down`，LLVM 21 只有 i32/f32 → double 拆 2×i32 重组）+ CPU 回退
    （无 warp 语义返回 v）。
    - **driver 595.84 坑**：`shfl.sync.down` 用 `clamp=-1`（越界返回自身）时**整个
      shfl 不交换**（lane 0 也返回自身）；改用 `clamp=31`（nvcc 同款）保证交换，
      越界 lane（lane+delta>=32）手动用自身 v 替换（`lane >= 32-delta` select）。
    - NVPTX target 从默认 sm_30 改为 **sm_75**（RTX 2070，PTX `.target sm_75`，
      与 `kernel.sync()`/conv3d 兼容，回归全绿）。
    - 测试 `tests/test_gpu_shfl.myp` GPU PASS（double/float/int，delta=16/4/1，
      越界返回自身断言）。调试开关 `MYP_DUMP_PTX=1` 编译时打印 kernel PTX。
  - **P2 ① `kernel.block_reduce_sum/max(v)`（§3.4 块归约）**：块内归约（warp shuffle
    树 → lane0 写 shared[warp] → `bar.sync` → warp0 归约 shared → `bar.sync` →
    broadcast 读 shared[0]）。sema 拦截（返回 v 类型）+ GPU codegen（emitKernelExpr）。
    - **三个关键实现坑**：
      ① shared 必须是**kernel 模块 addrspace(3) GlobalVariable**（真 `.shared`）——
        `alloca(addrspace 3)` 会被 NVPTX 降到 `.local` + `cvta.shared` → error 700
        非法地址；
      ② `CreateGEP` 源类型用**数组类型 `[8 x T]`**（不是元素 T），否则地址算错 →
        PTX verify 失败 → GPU kernel 生成失败 → 静默 CPU fallback（表现为
        "undefined variable 'kernel'"）；
      ③ `smem[warp]`/`smem[0]` 写必须**仅 lane0 条件 store**（同 warp 全 lane 写同一
        slot 竞争 → 结果未定义/0）。
    - 测试 `tests/test_gpu_shfl.myp` 扩展 GPU PASS（block_reduce_sum=256/块、
      block_reduce_max=255/块、float 版）。回归 109/109 + 负测试 58 + 框架 82；
      conv3d vs ORT 7e-7；CPU 回退返回 v。
  - **P2 ② `@gpu stride for`（§3.5 grid-stride）**：`@gpu stride for (long i = 0L;
    i < n; i = i + nTh) { body }`——grid-stride 循环：i = kernel.gid；while (i < n)
    { body; i += nThreads }（nThreads = ntid*nctaid，kernel 内读 gridDim，忽略用户
    step）。语法（parser @gpu stride 分支）+ codegen（kernel 内 PHI 循环头 + body
    回跳 + i 步进；loop_var 映射到循环 PHI）+ CPU 回退（**step 改 +1 顺序遍历全部**，
    因为用户 step 是 GPU 步长，CPU 须遍历所有 i）。普通 `@gpu for` 是 nThreads==n
    的特例。测试 `tests/test_gpu_stride.myp` 双模式 PASS（GPU 覆盖所有 i、CPU 顺序）。
    注：每线程多元素（grid 受限）需配合 P2 ④ `@gpu block(n)` 控制 grid。
    回归 109/109 + 负测试 58 + 框架 82；gpu_paradigm GPU 57/57；conv3d 7e-7。
  - **P2 ④ `@gpu block(n)`（§3.7 块大小/占用率可调）**：`@gpu for (...) block(n)` /
    `@gpu tile (...) block(n)` 用 n 作块大小（默认 256），grid=ceil(n/block)（for /
    stride）或用户 grid（tile）；块大小须为 32 的倍数且 ≤1024（sema 校验，越界报错）。
    - 语法：AST `ForStmt`/`GpuTileStmt` 加 `block_val`；parser 加 `block(n)` 子句
      （tile 顺序：grid → resident → stream → block）。
    - codegen：`generateGpuKernel`/`generateGpuTile` launch 的 `block_i32` 与 grid
      用 block_val；CPU 回退的 `kernel.bd` 模拟（`gpu_cpu_block_` 新成员）随 block_val，
      `gx=ceil(bound/bd)` 同步。
    - 测试 `tests/test_gpu_block.myp` 双模式 PASS：block(128) 每线程写 kernel.bd=
      128 断言；block(512)+stride 每 i 恰一次；tile block(64) 共享归约（GPU 专属）。
      GPU launch 打印 grid/block 正确（64/128、16/512、32/64）。
    - 回归 109/109 + 负测试 58 + 框架 82（自举可视化 1/1，含 myp_viz 重建）。
  - **P2 ⑤ 工具层（§5 计时/错误友好化/静态检查）**：
    - **§5.1 per-kernel 计时**：`runtime_gpu.c` 的 `myp_gpu_launch` 在
      `MYP_PROF_GPU=1` 时用单调时钟量同步 launch（stream==0）耗时，打印
      `kernel done: X.XXX ms`（per-kernel，无需 GpuEvent）。
    - **§5.2 错误友好化**：CUDA 错误码 → 可读字符串表（`gpu_err_str`）；`MYP_GPU=1`
      初始化失败逐点诊断（dlopen/cuInit/无设备/cuCtxCreate）；PTX 加载与 kernel 查找
      失败打印详情；launch/sync 失败映射可读信息；移除过时"PTX kernel parameter
      issues"注释。**codegen 修复**：`load_kernel` 失败改走 CPU 回退（原跳
      `gpu_done_bb` 静默跳过 → 捕获数组未初始化 → 结果错）。
    - **§5.3 静态检查**：tile `block_val` < 共享最大维度 → 警告（防协作覆盖不完/
      越界）；负测试 3 个（block 非 32 倍数 / >1024 / 非 @gpu for 用 block）。
    - 回归 109/109 + 负测试 61 + 框架 82（259/259）；gpu_block 双模式 PASS。
  - **P2 ③ `float4/double2/int4` 向量类型 + `load4/store4` 打包访问（§3.6）**：
    - 语言级向量类型：TypeKind/BuiltinType/Token/lexer/parser/sema/codegen 全链路
      → LLVM `<4 x float>` / `<2 x double>` / `<4 x i32>`（FixedVectorType）。
    - 组件访问 `v.x/y/z/w`：sema 校验 + codegen extract/insertelement（CPU 侧
      generateMemberAccess/generateAssignment + GPU kernel 侧 emitKernelExpr）。
    - 打包原语 `load4(float[] a, long i)` / `store4(a, i, v)`：GEP + `<4 x float>`
      打包读/写（CPU/host 走 emitVec4Access，kernel 走 emitKernelExpr）。
    - **坑**：`ConstantInt::get` 不接受向量类型（未初始化向量清零须
      ConstantAggregateZero）；组件下标 `'w'-'x'` 在 ASCII 为 -1 须显式映射；
      动态数组数据指针不保证 16B 对齐 → 向量访问用 align 4（未对齐，host movups
      安全；NVPTX 对非 16B 对齐拆标量，功能正确）。
    - 测试 `tests/test_gpu_vec4.myp` 双模式 PASS（打包读分量和、打包写 +1000 只改
      每组第 0 分量）；格式化/LSP/viz/tmLanguage 关键字同步。
    - 回归 109/109 + 负测试 61 + 框架 82（259/259）。
  - **P3 ① `@gpu reduce`（§8.2 声明式归约）**：
    - 语法：`@gpu reduce (acc, x) => { return <op>; } init V over a[lo..hi) -> out;`
      （AST `GpuReduceStmt` + parser + sema + codegen）。
    - 语义：`out = fold(init, a[lo..hi))`，op 为 (acc, x) => acc⊕x（须可结合）。
    - sema 校验元素/init/op 返回/out 类型一致（float/double/int）；提取 return 表达式
      到 `stmt.op_expr`；op_body 访问时临时设 `current_return_type_` 为元素类型。
    - codegen：GPU H2D a 范围 → 单 kernel（每块 tx==0 串行归约块内区间 →
      partials[blockIdx]）→ D2H → host 顺序合并 → out；CPU 回退顺序 fold。op 用
      emitKernelExpr（GPU）/ generateExpr（CPU），acc/x 绑定。
    - **坑**：launch args 数组每元素须为"指向参数值的指针"（void** 约定），直接存
      设备指针值导致 cuLaunchKernel 内部 segfault。
    - 测试 `tests/test_gpu_reduce.myp` 双模式 PASS（sum 全量/子区间、max、
      block(128)）。回归 109/109 + 负测试 61 + 框架 82（259/259）。
  - **P3 ② `@gpu scan`（§8.3 声明式前缀和）**：
    - 语法：`@gpu scan (acc, x) => { return <op>; } init V over a[lo..hi) -> b;`
      （AST `GpuScanStmt` + parser + sema 校验 in/out 均 T[] + codegen）。
    - GPU 两遍：K1 块和（`emitBlockSumPtx`，从 emitReducePtx 重构通用化）→ D2H
      partials → host 顺序块前缀 offsets → H2D → K2 块内 scan（acc=offsets[bid]，
      扫块内 acc=op(acc,a[i])、b[i]=acc）→ D2H b；CPU 回退顺序前缀扫描
      （`emitSeqScan`）。
    - 测试 `tests/test_gpu_scan.myp` 双模式 PASS（全量前缀和、子区间前缀）。
      回归 109/109 + 负测试 61 + 框架 82（259/259）。
  - **P3 ③ `@gpu scatter`（§8.4 声明式散点，冲突语义显式）**：
    - 语法：`@gpu scatter [(unique|atomic_add|any)] a[lo..hi) to b by idx[lo..hi);`
      （AST `GpuScatterStmt` + parser + sema 校验 a/b 同 T[]、idx 须 int[]）。
      冲突模式默认 any（实现无关）；unique = idx 无重复（运行时预扫校验，越界/
      重复报错退出）；atomic_add = b[idx]+=a（GPU 原子 / CPU 顺序累加）。
    - GPU：H2D a 范围 + idx 范围 + 整块 b（保留未写槽）→ unique 预扫（host）→
      grid-stride 写/原子 kernel（`emitScatterPtx`，atomicrmw Add/FAdd）→ D2H 整块
      b；CPU 回退顺序写/累加（`emitSeqScatter`）。两区间长度运行时校验相等。
    - 测试 `tests/test_gpu_scatter.myp` 双模式 PASS（unique 全量/逆序、any 冲突、
      atomic_add 浮点/整型、子区间 + 未写槽保持）；负测试 3 个。
      回归 109/109 + 负测试 64 + 框架 82（262/262）。
  - **P3 ④ `@gpu tile`（§8.5 优化降级语义）**：
    - GPU 实现（§3.2 共享内存协作 kernel）已有；本次补齐 §8.5 CPU 降级语义：
      重写 `generateGpuTileCpuFallback` —— smem → host 栈数组；**顺序循环遍历展平
      线程网格 p ∈ [0, grid*block)**（kernel.gid=p、bx=p/bd、tx=p%bd），
      kernel.sync() 空操作；运行时 grid 表达式在降级点 host 求值。
    - 降级对"每线程读写自己/更低槽"的 tile 模式语义不变（协作载入 → 本线程写 →
      读回；thread0 全量载入 + sync；多块 smem 复用无残留污染）。
    - 测试 `tests/test_gpu_tile_degrade.myp` 双模式 PASS（块内前缀和/thread0-load/
      smem-reuse）；`tests/test_gpu_tile.myp` 现 CPU 降级也 PASS（旧回退只写
      out[0] → err=511；新回退覆盖全部输出 → err=0）。回归 262/262。
  - **P3 ⑤ 规范归约顺序 + CPU 回退 + 静态检查（§8.6-8.8）**：
    - §8.6 规范归约顺序（浮点位一致）：reduce CPU 回退改 `emitSeqBlockReduce`
      （L1 每块顺序部分和 + L2/L3 顺序合并，与 GPU 同分块同合并序）→ 位级一致。
      验证 `test_gpu_reduce_bit.myp`：100000 float 归约 block(256)/block(128)，
      `bitcast<int>(s)` 位模式双模式逐字节一致。
    - §8.8 静态检查：三原语（reduce/scan/scatter）空输入 n≤0 运行时守卫
      （reduce → out=init 单位元；scan/scatter → 输出不变；原实现 blocks=0 →
      grid=0 / partials[0] 越界）；tile 48KB 上限负测试 `gpu_tile_shared_too_big.myp`。
    - §8.7 CPU 回退效率：L1 块部分和天然可并行（跨块并行不改变单块计算 → 位一致
      保持），@parallel for 接入留性能类（同 P2⑥/P1③④）。
    - 回归 109/109 + 负测试 65 + 框架 82（263/263）。

### v3.12.1 — 语言内建 @test 套件 + Man or Boy + lambda `nonlocal`
- **语言内建测试套件（`@test`）**：`mypc --test file.myp` 生成测试运行器（主循环经
  setjmp/longjmp 异常隔离），退出码反映失败；`tests/@test/` 目录自动发现 + 汇总
  `tests: N, assertions: X passed, Y failed`；断言 API 全系支持自定义 `msg`
  （assert/assertTrue/False/assertEq/Neq/assertLongEq/Neq/assertFloatEq/Neq/
  assertStrEq/Neq/assertNull/NotNull/fail/report）；`tests/test_myp_test.sh` 17 项检查。
- **Man or Boy 测试（Knuth）**：`tests/@test/man_or_boy.myp`——递归闭包把自身作为 thunk
  递归传递（M-FN-2 `__self`）+ 一等函数实参。以 Go 参考实现为准：`A(10,1,-1,-1,1,0) = -67`
  （k 按值每帧独立 + 按名 thunk）；含 `A(-1..10)` 全序列 12 断言。
- **lambda `nonlocal` 按引用捕获（M-FN-2 additive）**：lambda 内 `nonlocal k;` 显式按引用
  捕获外层函数参数/局部变量（共享可变）。codegen 在函数/action 序言把变量提升为堆 cell
  （隐藏类 `__cell_N` 单属性 `v:T`，ARC 管理），lambda 捕获 cell 对象、`__call` 开头注入
  属性 GEP 别名——读写与外层直达同一存储；函数退出释放本帧引用，闭包逃逸后 cell 仍存活。
  v1 边界：仅标量类型；嵌套 lambda / struct 方法内暂不支持（sema 报错）。Man or Boy 得以
  自然书写（不再需要 slice 盒/每帧副本）。`tests/@test/nonlocal`（5 测试 15 断言）。
- **修复：函数返回闭包 retain-at-return 缺失（M-FN-1 潜在 bug）**：`TypeKind::Function`
  未纳入返回 retain 集 → 作用域退出释放闭包 → 返回悬垂（单闭包场景内存未复用侥幸通过，
  多闭包暴露）。补：返回 fat pointer 的 closure（index 0）retain + `return <lambda>` /
  `return f()`（函数返回值）走 `arc_skip_retain_return_` 干净转移 rc=1。
- **修复：`visitFuncBody`/class action 体访问后 decl/action 引用悬垂（UAF）**：单态化重
  分配 `tu.functions`/`tu.classes` 使 `decl.nonlocal_captures` 赋值越界；改经成员暂存 +
  按索引重取赋值（ASAN 暴露）。

### v3.12.0 — 内存系列收尾（M5–M9）
- **M8 · 全量引用计数**：`string`（`myp_alloc_str`，`MYP_STR_TYPE_ID`）、动态数组
  `T[]` 与 `slice` backing（`myp_alloc_slice_backing`，`MYP_ARR_TYPE_ID`，24B 头
  `{rc, type_id, elem_size, count, cap}`）全部改为引用计数——作用域/覆盖自动释放，
  不再依赖 arena/进程退出回收。字符串拼接/数组字面量/切分均经计数。
- **in-place 字符串拼接**：`s = s + x` 在 `s` 为唯一计数串（rc==1）时 realloc 原位
  扩展（`myp_str_append`）——长串累积 O(n²)→O(n)（bench：808ms→52ms）。
- **M5 · struct 引用字段值语义 ARC**：struct 槽按字段计数（拷贝逐字段 retain / 释放
  逐字段 release，kind-5），struct 不再"引用字段不参与计数"。
- **M6 · 跨线程原子 ARC**：`rc` 为 `_Atomic uint32_t`；`myp_retain`=relaxed
  fetch_add，`myp_release`=release fetch_sub（返回旧值），末次释放 acquire fence 后
  析构；分配/释放列表由进程级自旋锁 `myp_alloc_lock` 保护。class/string/数组可安全
  跨线程传递。验证：移除原子后 TSan 报竞争、恢复后无竞争。
- **M7 · `@weak` 弱引用**：字段注解 `@weak`（仅 class/interface 引用字段，struct 字段
  编译期拒绝）；弱表 64 槽链式哈希 + 自旋锁；目标销毁 `myp_weak_notify_death` 持锁置空
  全部弱槽并重查 rc（防并发升级竞争）。读取 = 弱→强一次性升级。测试 `tests/weak_cycle`、
  `tests/weak_non_ref`（负）。
- **M9 · 内存诊断与严格校验**：`Memory.*` 存活/按类型计数、arena/region 字节、协程槽/
  栈池统计；分配失败注入（`MYP_FAIL_ALLOC=n` / `failAllocEnable`）；strict 头校验
  （rc 下溢、重复释放、非法 `type_id` abort，ASAN 默认开）。`tests/mem_diag`、
  `stress/oom_sweep`。
- **M1/M2 · 协程资源上限**：句柄 `{generation<<32|slot}` 世代化（槽复用安全、旧句柄判
  无效）；栈池字节预算 `MYP_CORO_STACK_POOL_MAX_BYTES=16MiB`、大栈
  `MYP_CORO_STACK_BIG=1MiB` 旁路池。`tests/coro_slot_reuse`、`tests/coro_stack_pool_cap`。
- **if/while/for 条件临时泄漏修复**：条件求值产生的 class 临时（调用/弱升级）被分支体
  语句末 flush 抢占 → 分支后 `arcReleaseConditionTemps` 释放，修另一路径泄漏。
- **`string + 非 string` 拼接泄漏修复**：`stringifyForConcat` 转换临时在
  `myp_strcat` 后显式 release（修 `"s"+i` 每拼接漏 1）。
- **mapping 数据传递测试集（`tests/map_data_*`）**：系统覆盖 mapping 传递的数据特性
  矩阵——标量家族（int/long/float/double/bool）、string、多参数事件、事件链返回值
  （double/string/bool）、where 过滤、lambda 变换、`slice<T>`、tuple（含解构/嵌套/
  lambda）、struct（含 class 引用字段/string/嵌套/链返回）、class 引用、interface 胖
  指针（虚表分派）。随测试修复 7 个真实 bug：
  - slice `.length`/`.size` 字段形式：codegen 生成 `ptrtoint(&s)` 当长度 → extractvalue；
    sema 标成 `()->int` → 字段返回 int、调用形式经 visitCall 拦截。
  - action 参数 slice 登记缺失（`generateClassAction` 漏 `var_slice_types_`）→ `a[i]`
    LLVM verify 崩溃。
  - interface 事件参数 upcast：`paramIfaceName` 不识别 `fire_<Class>_<Event>` → fire
    调用处具体类实参未提升为 `{data,vtable}` fat pointer。
  - struct 内 class 引用字段 i32 占位：`buildStructTypes` 早于 `buildClassStructTypes`，
    `typeNodeToLLVMType` 经 `getClassStruct` 落回 i32 → `h.p.get()` 生成 `Payload_get(i32)`；
    加 TU class 名 fallback 恒返回 ptr。
  - struct 字段 store 缺 interface upcast（单层 `h.s=c` + 链式 `w.h.s=c`）：Circle* 直接
    存进 `{ptr,ptr}` 字段、vtable 未初始化 → 虚表分派 SIGSEGV；两处均补 buildInterfaceFat。
- **ARC 覆盖补强**：`map_data_struct_iface`（struct 含 interface 字段：拷贝共享 retain
  平衡/覆盖释放/嵌套链式/级联零泄漏）、`weak_multi_sub`（多个 `@weak` 槽共享同一目标、
  销毁全置空、先后销毁顺序无泄漏）。
- **fuzz 回归（变异模糊测试 `tools/fuzz_myp.py`）**：3000 迭代 ×2 全 CLEAN；修复 void
  函数 `return <void-expr>;` 生成非法 IR（`emitFunctionReturn` 归零 void 值走 `ret void`）；
  HANG 确认重跑 timeout 4s→20s（排除大 stdlib 种子并行负载假阳性）。
- **回归基线**：release 227/227、ASAN 227/227、ASAN stress 6/6、TSan 6/6、OOM 注入
  13/13、fuzz 3000 迭代 0 崩溃。

### v3.11.20
- **内存生命周期 P0 加固**：
  - `@region` 新增函数级保守逃逸分析：slice/数组经 return、property/global store、
    subscript store、throw 或调用参数逃逸时，不建立 arena mark，防止函数返回后持久引用
    指向已回滚 backing；局部-only 函数继续使用 region 快速回滚。
  - `slice<class>` backing 改用引用计数类数组布局，并由 runtime 建立唯一清理登记；
    region 退出或线程/进程清理时逐元素 release，修复元素写入 retain 后无对应 release 的
    长跑泄漏。slice 的 16 字节 ABI 和浅拷贝语义不变。
  - 新增 `tests/region_escape`、`tests/region_slice_class_arc`。
- **@parallel for / @gpu for 体不支持构造的静默垃圾值 → 编译期干净报错**：
  - 症状：并行体内 `new Node()`（→ 常量 0/null）、类实例字段读写（写被丢弃、读
    恒 0——实测 `n.val=7` 后 1000 次读全错）、字符串拼接（`"iter "+i` → 垃圾指针
    运算 → LLVM verify "Call parameter type does not match function signature!"）。
  - 修复（codegen_gpu.cpp emitKernelExpr）：`new`/`new[]`、字符串字面量、类实例字段
    访问（`var_class_map_` 检测）在 kernel 路径现报清晰错误，提示"在循环外分配/只做
    数值运算"。数值数组读写、slice、struct 元素字段、Atomic 等正常路径不受影响。
  - 新增负测试 `tests/negative/parallel_new.myp`、`tests/negative/parallel_string.myp`；
    195/195 回归通过。
  - 注：`tests/stress/parallel_stress.myp` 的 `workers >= 2` 断言间歇性失败（16 核下
    偶发 workers=1）——4096 迭代太快、首 worker 抢完全部导致的时序竞态，与本次改动
    无关（改动只在编译期加错误分支）。
- **编译期拒绝 @coro 方法递归自调用（把静默垃圾值变成清晰错误）**：
  - 背景：`@coro` 调用 = spawn 新协程返回 handle（long），不是返回值。因此
    `@coro long deep(n) { return deep(n-1) + 1; }` 是对 handle 做运算——实测无论
    n 多大恒返回 2（静默错误）。
  - 修复：sema 跟踪当前方法名（`current_method_name_`），在 `@coro` 体内检测到
    自调用（裸名 / `this.` 形式）且**结果被当值使用**（return/算术/参数/赋值）时
    报错：
    `recursive call to '@coro' method 'X' is not supported: an '@coro' call spawns
    a new coroutine and returns a handle... Move the recursion into a plain function`。
  - **语句丢弃形式**（`deep(n-1);`）仍允许——那是 spawn 链（tests/coro_stack 依赖）。
  - 验证：值使用自调用报错、语句丢弃放行、普通函数递归不受影响、合法嵌套协程
    （coro_nest 模式）不受影响。新增负测试 `tests/negative/coro_self_recursion.myp`；
    193/193 回归通过。
- **修复 slice<类> 元素 / `new Foo().x` 上的链式字段访问（LLVM verify 崩溃）**：
  - 症状：`s[0].val`（slice<类> 元素，s[i] 返回类引用）、`new Node(7).val` 报
    `LLVM verify failed: Call parameter type does not match function signature!`
    或读出垃圾值（链式访问在 codegen fallback 丢字段）。
  - 修复：`generateMemberAccess` 的 sema-记录类分支（`resolved_object_class`）从
    仅 Call 对象扩展到 **Subscript / NewExpr** 对象——对 slice/数组类元素下标结果
    和新鲜 `new` 结果 GEP 属性字段。
  - 验证：`s[i].field`、`new Foo().field`、方法调用结果链式全部正确；新增
    `tests/slice_class_chain/`；192/192 回归 + ASAN 干净。
  - 已知限制（未改）：slice 数据用 `myp_region_alloc` 竞技场分配、无析构——slice<类>
    元素不会 ARC 释放（区域/进程退出才回收）。`slice<Node>` 循环创建会累积泄漏。
- **修复 catch/finally 体内 throw 无限循环（异常 handler 未及时 pop）**：
  - 症状：`try { throw "a"; } finally { throw "b"; }`、catch 体内 `throw`、嵌套
    finally 内抛 → 无限循环（运行时几秒吐出千万行）。根因：本 try 的 handler
    （jmp_buf）直到 `merge_bb`/`rethrow_bb` 才 pop，而 catch/finally **体**执行时
    它仍在栈顶 → 体内 throw 经 `__myp_throw` 长跳到**同一 try** → 反复触发。
  - 修复（codegen_stmt.cpp）：handler 只在 try 体执行期间保持激活——
    - 有 finally：在 `finally_bb` 起点 pop（覆盖 try 结束/catch 结束/propagate/
      return/break/continue 转发全部入口）；
    - 无 finally（仅 catch）：try 体正常结束、catch 体起点、`rethrow_bb` 各 pop；
    - `emitExceptionRethrow`（裸 `throw;`）去掉 pop——`throw;` 只在 catch 内，
      handler 已在 catch 起点 pop 过，再 pop 会弹掉外层 handler（表现为未捕获）。
  - 验证：finally 抛替换原异常传外层、catch 体抛传外层、裸重抛保消息、嵌套 finally
    全正确；新增 `tests/exception_throwin/`；191/191 回归 + ASAN 压力全过。
- **修复条件表达式（`&&`/`||`/ternary）分支内类临时对象 ARC 释放违反支配（LLVM verify 崩溃）**：
  - 症状：`w.get().x == 3 && w2.get().x == 5`（方法调用返回类的链式字段访问）、
    `true ? w.get().x : w2.get().x`、`c ? new Point(1) : new Point(2)` 报
    `LLVM verify failed: Instruction does not dominate all uses!`。链式 `get().x`
    还会丢字段（`Console_write(ptr)` 参数类型不匹配）。
  - 根因一（丢字段）：`generateMemberAccess` 对 `Call` 对象无分支，fallback 直接
    返回调用结果。修复：sema 在 `MemberAccessExpr` 记录对象解析 class，codegen
    用它对调用结果 GEP 字段（新增 `resolved_object_class`）。
  - 根因二（不支配）：`arc_pending_temps_` 扁平列表在**语句末**统一释放；短路 `&&`
    的 merge 块可从 entry 直达（跳过 rhs_bb），ternary 的 merge 可从另一分支直达
    —— 分支块不支配 merge，分支内创建的临时对象在 merge 释放违反支配。
  - 修复：新增 `arcEndBranch(before, result)`——在分支块内释放分支创建的中间临时
    对象；若分支结果是新类引用临时对象则转移所有权给 merge phi（两臂都是新临时时
    推 phi 由语句末释放一次；单臂新临时时消费并泄漏，避免对借用分支双重释放）。
    应用于 `generateShortCircuitLogic`（rhs_bb）与 `generateTernary`（true/false_bb）。
  - 验证：`&&`/`||`/ternary × 链式字段访问/类结果全部通过；新增 `tests/member_chain/`；
    190/190 回归 + ASAN + TSAN 压力测试全过。
- **修复 @macro StmtList 累加拼接 O(n²)（`out = out + quote{...}` 惯用法）**：
  - 症状：文档推荐的 `makeCalls(n)` 风格循环里每次 `+` 都深克隆整个已累加列表 →
    二次方。实测 gen(1000)→0.5s、gen(4000)→7.6s、gen(10000)→**46s**（每翻倍 n
    耗时×4）。
  - 根因：`evalBinary` 的 `Ast + Ast` 无条件 `cloneStmtI` 双侧；`EvalValue::Ast`
    是 `shared_ptr`，累加变量与求值临时量共享同一 vector（refcount=2），无法安全
    move 走左侧。
  - 修复（两处）：
    - `evalExpr` 赋值分支增加快路径：识别 `acc = acc + X`（acc 为 StmtList）时
      **原地 append** X 的语句到 acc 现有列表——摊销 O(n)。用 `use_count()==1`
      守卫：若别的变量别名共享 acc 的列表，则回退到通用克隆路径（保证别名不被污染）。
    - `evalBinary` 的 `Ast + Ast` 在左操作数 `use_count()==1`（如新鲜 quote）时
      移动而非克隆。
  - 效果：gen(10000) 46s → **0.2s**，gen(40000) <1s，线性缩放。输出与文档惯用法
    逐字节一致；别名安全用例（`alias = out; out = out + X;` → alias 不受污染）通过。
  - 新增正测试 `tests/macro_concat/`；189/189 回归通过，ASAN 干净。
- **修复编译期 `const string` 拼接导致 LLVM verify 崩溃（元编程测试暴露）**：
  - 症状：`const string G = "a" + "b";`（或 @eval 函数返回拼接串、`a() + b()`）
    报 `LLVM verify failed: Function return type does not match operand type of
    return inst!`。根因：`evalBinary` 缺字符串 `+` 分支 → 两个 Str 值落入 int 分支，
    读 Str 的 `.i` 字段得垃圾整数 → const 被替换成类型不符的 int 字面量 → codegen
    给返回 `ptr` 的函数生成 `ret i32`。
  - 修复：`evalBinary` 增加 `Str + Str → ofStr(a.s + c.s)`；`eq`/`lt` 增加 Str
    分支（此前字符串相等/比较同样读 `.i` 算错）。编译期字符串拼接/相等现在正确。
  - 新增正测试 `tests/const_string/`（拼接/链式拼接/@eval 拼接/编译期 ==）；
    188/188 回归通过，ASAN 干净。
- **修复前缀 `++`/`--` 嵌套反解的指数级 AST 膨胀（内存耗尽挂死，fuzz 暴露）**：
  - 症状：`-`×50（偶数个 → 25 个 `--` token）使 mypc 100% CPU 自旋 + 内存暴涨挂死
    （`内存占用太大挂了`）；`----1` 等深层前缀链触发。原实现逐运算符反解 `x = x ± 1`
    并 `cloneExpr` 递归副本，每层把整棵子树翻倍 → 节点数 2^n：25 层 ≈ 6700 万节点。
  - 修复：`parseUnary` 先收集**连续**的 `++`/`--` 求和（净增量 N），一次构建
    `x = x ± N`——AST 线性。对纯链语义等价（每层在基址上叠加/对消）：`----x` =
    `x = x - 2`、`++--x` 抵消。
  - 验证：`-`×50/200/5000 全部瞬间完成并干净报错（对字面量赋值）；`--x`/`++x`/
    `----x`/`++--x` 语义正确（新正测试 `tests/prefix_chain/`）；ASAN 干净；
    187/187 回归通过。负测试 `tests/negative/unary_chain_oom.myp`。
- **编译期拒绝在 `@thread` 实例上手动调用 `@startup` 方法（压测暴露的误用 → 编译期诊断）**：
  - 症状：`Worker w = new Worker() @thread; w.run(...)`（`run` 为 `@startup`）在运行时
    SIGSEGV —— `@startup` 已由运行时在 worker 线程自动执行，手动再调一遍 = 双重执行。
  - 修复：sema 记录 `@thread` 注解变量（`VarDecl.has_thread_annotation`），在成员访问
    解析到 `@startup` 方法（`ActionDecl.has_startup`）时报错：
    `cannot manually call '@startup' method 'run' on a @thread instance (auto-invoked in the worker thread)`。
  - 作用域：仅限 `@thread` 实例；普通实例上手动调 `@startup` 仍合法（`mypc run` 依赖此
    路径），回归含 `mypc run` 用例全过。
  - 新增负测试 `tests/negative/thread_startup_call.myp`；185/185 回归通过。
- **协程切换路径剔除 sanitizer fiber 钩子（perf 定位：非 ASan `cpp_long` 的
  `__sanitizer_finish_switch_fiber` NULL 检查占 `__myp_coro_resume`/`__myp_coro_yield`
  自样本 64~80%）**：
  - `__myp_coro_resume`/`__myp_coro_yield`/trampoline 每次上下文切换都做两次
    `if (__sanitizer_start_switch_fiber)` 运行时检查——weak 符号经 GOT 加载 + 分支，
    普通/TSan 构建里恒为 NULL，纯浪费（perf 样本大量聚在切换返回后的检查指令上）。
  - 修复：`src/runtime/runtime.c` 用 `#if defined(__SANITIZE_ADDRESS__)` 守卫——
    只在 ASan 编译（`-fsanitize=address` → gcc 定义该宏）下启用 fiber 钩子；
    普通/TSan 构建编译期整体剔除（行为与 weak-NULL no-op 一致，少 GOT 加载+分支）。
    顺带把 `myp_asan_fake_stack` 从非 ASan 的 TLS 布局里移出（TLS 块小 8 字节）。
  - 效果：`cpp_long`（channel 乒乓 N=10⁷）454→441ms（~3%，交错 A/B 一致）；
    coro_switch 66→62ms。ASan 路径不变：`build-asan` 全局 `-fsanitize=address` →
    `__SANITIZE_ADDRESS__` 定义 → 钩子保留，ASan 冒烟 + 181/181 回归全过。
  - 181/181 正常 + ASan、25/25 Go、33/33 C++ 基准全部通过，无回归。
  - 注：perf 显示剩余热点是 channel send/recv/wake + 2 次上下文切换/消息的固有
    机器开销（切换返回边界的样本涂抹），无单一可安全下刀的大项。

- **修复 `@parallel for` 偶发永久挂起（计数器重置竞态，压测暴露）**：
  - 症状：间歇性 ~1/3 概率 worker 全核空转 + 主线程 barrier 永等；也是此前回归
    `parallel_for` 偶发超时的根因。
  - 根因：`myp_pool_parallel_for` 先推送分块、后重置 `done_count=0`。上次调用
    残留的自旋 worker 在「推送后、重置前」窗口抢到新分块并 `++done_count`，随后
    重置抹掉该增量 → 该分块永久不计 → `done_count` 到不了 `total_chunks` → 挂起。
  - 修复：发布任何新分块前先重置计数器（`runtime.c`）。

- **新增协程/并发压力测试套件 `tests/stress/`**：
  - `run_stress.sh`（-O2 / TSAN=1 / ASAN=1）+ 5 项：`coro_flood`（3.6 万协程创建/
    销毁）、`coro_switch_storm`（400 万次切换）、`channel_stress`（多产多消）、
    `async_io_stress`（loopback TCP）、`parallel_stress`（@parallel for + Atomic）。
  - 独立于 `run_tests.sh`（负载重、含时序数据，按需运行）。

- **可读性重构：拆分 10807 行 codegen.cpp 与 4785 行 sema.cpp / 2952 行 parser.cpp**
  （纯重构，零行为变化，181/181 + 压测 5/5 全过）：
  - `codegen/` → codegen.cpp(核心) / codegen_class / codegen_stmt / codegen_expr /
    codegen_gpu；`sema/` → sema.cpp + sema_expr.cpp；`parser/` → parser.cpp +
    parser_expr.cpp + parser_stmt.cpp。最大文件 10807 → ~2900 行。
  - `convertIntegerValue`/`zextIndexValue` 去 static 并在 `CodeGen.h` 声明（跨 TU）。

- **修复词法层 3 处缓冲区越界读取（fuzz 暴露，畸形输入崩溃 SIGABRT）**：
  - `scanNumber`：`5.`（数字后点 + EOF）时 `source_[offset_ + 1]` 越界（`..` 范围
    符判断未做边界检查）。
  - `scanString` / 字符字面量：`"\` / `'\`（反斜杠是最后一个字符）时转义处理后
    无条件 `advance()` 越过缓冲末尾。
  - 修复：`source_[offset_+1]` 加 `offset_+1 < source_.size()` 守卫；转义处理后
    `if (!isAtEnd()) advance()`。修复前 3036 个边角料输入 3 崩溃 → 修复后 0。

### v3.11.19
- **C 运行时以 -O2 编译（perf 定位：`cpp_long` channel 乒乓 N=10⁷ 的
  `myp_channel_recv` 31.6% / `myp_channel_wake_one` 15.0%）**：
  - `mypc` 链接生成程序时**每次用 gcc 现编 `runtime.c`**，但编译命令没有任何 `-O`
    参数——整个 C 运行时（channel/coroutine/string/ARC/thread pool）一直以 gcc
    默认的 **-O0** 编译：无内联、所有局部变量上栈，连 `myp_channel_get` 这种 3 行
    函数都是独立 call。`mypc -O2` 只优化 MYP 生成的 LLVM 代码，从未作用到 C 运行时。
  - 修复：`src/main.cpp` 链接阶段以 `-O2` 编译 `runtime.c`/`sdl_bridge.c`/
    `runtime_gpu.c`，并把 `-O2` 折进缓存哈希（旧的 -O0 缓存对象不会复用）。
  - 效果（对所有生成程序生效）：
    - channel 乒乓 N=10⁷：**582→453ms（22%）**；`channel_pingpong` bench 6→4ms，
      反超 Go（Go/MYP 1.50）。
    - ARC 压力（3M 对象+字符串拼接）：~515→~450ms（15%）。
    - coro_switch 72→66ms、io_socket 71→69ms；parreduce 反超 C++ 从 2.00→**3.00**
      （1ms vs 3ms），parcomp 也反超 C++（0.88）。
  - 顺手把 channel 环形缓冲的 `% capacity`（每次消息 2 次 idiv）改为有界条件回绕
    （head/count 均 < capacity，单次比较回绕即精确）。
  - 181/181 回归（含 ASan）、25/25 Go 基准、33/33 C++ 基准全部通过，无回归。

### v3.11.18
- **`@parallel for` body 改为 chunk 循环（perf 定位：并行归约 N=10⁸ 的
  `parallel_body_j` 62% / `myp_pool_worker_id` 8.9%）**：
  - 原实现把 body 生成为**每迭代一个函数** `void(i, arg)`，worker 每轮 `call` 一次
    ——每次迭代付 call/ret + 栈帧开销，且 body 是"单条语句"，LLVM 既无法提升
    `Parallel.workerId()` 也无法展开/向量化累加。
  - 改为生成 **chunk 循环体** `void(start, end, step, arg)`（body 内部 `for(i=start;
    i<end; i+=step)`），worker 每 chunk 只调用一次。运行时 `work_fn` 签名由
    `void(int,void*)` 改为 `void(int,int,int,void*)`；`myp_pool_parallel_for` 外部
    签名不变（LLVM IR 中 fn 为不透明 `ptr`），缓存 `.myp.ll` 无需重编。
  - 将 `myp_pool_worker_id` 标记 `readnone nounwind willreturn`（TLS 值在 chunk 循环
    内恒定），让 LICM 把 `workerId()` 提出循环——由此 `perThread[wid]` 地址成为循环
    不变式，LLVM 可把累加器提升到寄存器（打破内存往返依赖链），甚至向量化。
  - 效果：并行归约 N=10⁸：**320→122ms（2.6x）**；N=10⁶ bench 3→1ms；parreduce 反超
    C++ std::thread（1ms vs 2ms，C++/MYP 2.00）。181/181 回归（含 ASan）、25/25
    Go 基准、33/33 C++ 基准全部通过，无回归。

### v3.11.17
- **整数→字符串快速路径（perf 定位：ARC 压力 31% 在 `myp_to_string_i64`）**：
  原实现 `snprintf("%ld")` + `myp_strcat`（glibc 通用格式化 + 内部 malloc + 二次
  strlen/拷贝）。改为手写 2 位查表 itoa：每步 `%100` 反向写两数字（int64 至多 10
  次除法），直接精确分配结果串，无中间缓冲/二次拷贝。
  - 修复过程中发现并修正 i32 路径 bug（负 i32 符号扩展成巨正数 → `-5` 输出错误）；
    边界 ALL PASS（INT64_MAX/INT32_MIN/10^18 等）+ 181/181 回归。
  - 效果：ARC 压力（3M 次对象+字符串拼接）589→~518ms（12%）；主/Go 基准无回归。

### v3.11.16
- **Channel 同步交接（channel_pingpong 21→5ms，反超 Go 1.20）**：`send`/`recv`
  完成缓冲操作后唤醒对端等待者时，若调用方是协程则**立即 `__myp_coro_resume`**
  （Go 式 rendezvous，免一轮 `Coro.scheduler()` 往返）。深度守卫（64）防链式递归
  失控；`close`/`try_*` 保持 ready-only。
  - 前置：v3.11.15 修复的多消费者 count 下溢与句柄槽位复用两个缺陷是本优化的
    安全前提——修复后同步交接 181/181 回归（普通/ASan）+ 4p×2c 压力测试全过。
  - 效果（Go/MYP）：channel_pingpong **0.29→1.20**（5ms vs Go 6ms，MYP 反超）；
    io_socket 1.08、coro_switch 4.22、coro_spawn 0.16 无回归；全 25 基准 verify 全对。

### v3.11.15
- **修复两个协程/通道崩溃级缺陷**：
  - **Channel 多消费者 count 下溢**：`myp_channel_recv` park-resume 路径无守卫
    `count--`——多消费者时第二个消费者被唤醒但值已被第一个取走，count 下溢 -1 →
    环形缓冲越界写 `buf[-1]`（ASan heap-buffer-overflow + munmap 崩溃）。send 的
    park-resume 无守卫 `count++` 同理。修复：send/recv park-resume 后循环重新校验
    缓冲状态（count 不足/已满重新挂起）。回归测试 `tests/channel_multi_consumer/`
    （修复前崩溃）。
  - **协程句柄槽位复用（结果串位）**：`__myp_coro_create` 复用已完成协程的槽位，
    无 await 协程 eager 启动即完成 → 新协程拿到相同句柄 → 已存句柄别名、
    `Coro.result` 读到新协程结果（verify 900≠600）。修复：槽位不复用（句柄唯一）；
    完成协程栈经线程局部 retired 列表延迟回收（create/调度器安全点移回栈池）。
    回归测试 `tests/coro_handle_unique/`。
- **性能**：顺带消除 `__myp_coro_create` 找可复用槽的 O(n²) 线性扫描，
  **coro_spawn 460→24ms（19x）**；channel_pingpong 21ms 无回归；coro_switch 72ms。
  普通/ASan 构建各 **181/181** 回归全过。

### v3.11.14
- **协程上下文切换改为寄存器级汇编（coro_ctx.S，x86-64 SysV）**：
  - 替代 `swapcontext`（ucontext 内部每次 `sigprocmask` syscall，微基准 ~180ns/次）
    为自研寄存器级切换（仅保存/恢复 rsp/rip/rbx/rbp/r12-r15，~13ns/次，13.9x）。
  - 集成：`myp_runtime` 新增 `src/runtime/coro_ctx.S`；`mypc` 链接生成程序时同样
    编译该文件（`src/main.cpp`）；非 x86-64 回退 ucontext；ASan 构建用
    `__sanitizer_start/finish_switch_fiber` 显式通知纤维切换（trampoline 入口补配对）。
  - 效果（16 核 min-of-3，Go/MYP）：coro_switch 400→**72ms**（0.76→**4.26**，
    MYP 反超 Go 4.3x）；channel_pingpong 54→**21ms**（0.11→0.29，Go 快 9x 收窄到
    3.4x）；io_socket 86→**71ms**（0.90→**1.10**，MYP 反超 Go）。
  - 修复过程中的 bug：初始帧定位（`myp_ctx_init` 用 `top-8` 导致保存块越界 48 字节
    堆损坏，改为 `top-64`）；ASan fiber 配对（trampoline 入口补 finish_switch_fiber）。
  - 回归：普通/ASan 构建各 179/179 全过。修复 `run_compare_go.sh` 未调用
    `build_all()` 的缺陷（依赖预编译产物）。

### v3.11.13
- **协程通信/I-O 基准 + 调度器 O(N²) 修复**：新增 `channel_pingpong`（cap=1
  Channel 双向 10⁵ 次收发）与 `io_socket`（回环 TCP 逐字节 ping-pong，@coro +
  `waitFd` vs goroutine + 阻塞 socket）两个 MYP/Go 对比基准，`run_compare_go.sh`
  扩至 25 项。
  - channel_pingpong：MYP 54ms vs Go 6ms（0.11，Go 快 ~9x）——Go channel 双方阻塞
    时直接交接（无栈切换/syscall），MYP 每次 park/resume 走 ucontext 栈切换 +
    调度器驱动（~270ns vs ~30ns）。
  - io_socket：MYP 86ms vs Go 79ms（0.92，基本持平）——都受限于 syscall + 唤醒，
    MYP waitFd 轮询调度代价已与 Go netpoller 相当。
  - **顺带修复真实缺陷**：`myp_coro_waits` 从不压缩，`active=0` 的 wait 记录随等待
    次数线性累积 → 调度器每轮 O(N) 扫描 → 总 O(N²)。io_socket 修复前 6101ms、
    修复后 86ms（71x）；N=50000 回归测试由 ~38s（超时）→ 0.30s（线性）。
    修复：`__myp_coro_scheduler` 入口原地压缩（仅保留 active=1），跨调度器不持有
    表索引，安全。回归测试 `tests/coro_wait_compact/`。

### v3.11.12
- **MYP vs Go 主套件全量对比**：把 21 个主套件基准（sieve..bigint）逐个移植为
  `bench/go/*.go`（由 `bench/cpp/*.cpp` 逐文件翻译，同算法/同规模/同 LCG），
  `bench/run_compare_go.sh` 从 2 个协程专项扩展为 **21 主套件 + 2 协程**共 23 项，
  verify 与 MYP 全部对拍（整数精确、浮点 1e-3 容差）。
  - 结果：MYP 赢 20/21 项，几何平均 ~1.8x；仅 `fft` Go 略快（0.86）。
  - 最大差距在浮点/带宽类：convolution 4.04、kmeans 3.81、matmul 3.47、sobel
    3.17、radixsort 2.20、spmv 2.15——根因是 MYP 走 LLVM O2 **自动向量化**，Go
    默认不向量化。
  - 整数/分支/字符串类 1.1~1.7x：quicksort 1.14、kmp 1.18、heapsort 1.15、
    sieve 1.08、mandelbrot 1.01 等。
  - 移植中修正：sha256 初版用了旧的 64KB 消息长度，与 C++/MYP 的 4MB 不一致导致
    verify 不符（实现本身经 Go 标准库 crypto/sha256 与 sha256sum 双重验证正确）。

### v3.11.11
- **MYP `@coro` vs Go goroutine 协程对比**：新增 `bench/go/`（Go 侧）+ `bench/myp/`
  的 coro_switch/coro_spawn + `bench/run_compare_go.sh`（MYP -O2 vs `go build`）。
  - coro_switch（200 协程 × 10000 次挂起/恢复）：MYP 406ms vs Go 306ms（0.75，
    Go 快 ~33%）——ucontext 交换 vs Go 抢占调度，差距不大。
  - coro_spawn（20000 个只返回协程）：MYP 527ms vs Go 3ms（0.01，Go 快 ~175x）
    ——Go goroutine ~2KB 可增长栈极廉价；MYP @coro 每个分配固定栈（默认 128KB，
    `@coro(stack=KB)` 可调小）+ ucontext 初始化。
  - 结论：MYP 协程适合少量长生命周期任务（I/O/事件），不适合海量短任务。

### v3.11.10
- **`Parallel.setThreads(n)` 超过 CPU 数时打警告**：`myp_pool_set_threads` 在
  `n > sysconf(_SC_NPROCESSORS_ONLN)` 时向 stderr 打一次警告（过订阅通常损害
  吞吐），但仍尊重用户显式指定、不静默 cap。实测 `setThreads(64)` 于 16 核 →
  警告 + 仍 64 worker；`setThreads(8)` 无警告。回归 O0/O2/ASAN 178/178 全过。

### v3.11.9
- **parreduce 0.60→1.00**：并行归约基准曾用 `Atomic.addInt`（每元素原子 RMW），
  但每线程专属槽位（`Parallel.workerId()` 恒定、槽位互斥）**无竞争，普通
  load-add-store 即可**——去掉原子后 MYP 3ms vs C++ std::thread 3ms 完全持平
  （verify 仍精确一致 496532956）。**MYP 并行归约最佳实践：每线程槽位用普通写，
  只有共享槽位才用 `Atomic`**。

### v3.11.8
- **并行归约基准 parreduce**：`@parallel for` + `Atomic.addInt`（每线程
  `Parallel.workerId()` 槽位低竞争）vs C++ `std::thread` 各自累加——verify 精确
  一致（496532956），0.60（C++ 快，因 MYP 每元素原子 RMW vs C++ 线程内普通加）。
  与 parcomp 构成并行基准对（计算/归约）。34 项基准 verify 全一致。

### v3.11.7
- **并行基准 parcomp**：MYP `@parallel for`（16 线程池，写入 slice）vs C++
  `std::thread`（16 线程分块）——0.94 基本持平（MYP 17ms vs C++ 16ms，串行同负载
  180ms+）。首个并行基准，验证 `@parallel for` + slice 的并行效率与手写线程相当。
  33 项基准 verify 全一致。

### v3.11.6
- **`@parallel for` 与 slice 联用**：此前并行体（emitKernelExpr 路径）只支持普通
  数组——`slice<T>[i]` 读写直接 GEP 捕获的 `{data,len}` 结构体值 → LLVM verify 失败；
  `slice<struct>` 字段 `v[i].x` 读/写静默返回 0（verify 错乱）。修复：
  - Subscript 读/写分支识别 slice 变量，解包 `{data,len}` + 边界检查 + GEP
    （`data` 指针线程共享，各线程写不同索引无竞争）。
  - 新增 `emitKernelElementAddr`（slice 解包 / 普通数组直 GEP 统一元素地址）。
  - MemberAccess 读 + Assignment 写处理 Subscript 对象（slice<struct> 与 struct
    数组的字段访问）。
  - 实测：串行 37ms → `@parallel for` 3ms（~12x，16 线程池）；verify 与串行一致。
  - 回归测试 `tests/parallel_slice/`；回归 O0/O2/ASAN 178/178 全过。

### v3.11.5
- **嵌套泛型解析**：`slice<slice<int>>`、`Box<Vec<int>>`、`foo<Bar<int>>(...)`、
  `new Box<int>[]` 此前无法解析——lexer 把 `>>` 合成一个 `GreaterGreater` token，
  泛型收尾期待单 `>`。新增 `consumeGenericClose`：遇 `>>` 消费后压入合成 `>`
  （`pending_` 栈，`peek`/`advance` 优先消费），6 处泛型收尾点统一替换。
- **slice-of-slice 分配大小错误**：`new slice<slice<int>>(n)` 元素大小算成 4 字节
  而非 16（`typeNodeToLLVMType` 无 `slice<T>` 分支，落入内置默认 i32），写穿外层
  slice 数据区 → verify 错乱 + 退出段错误。修复：`typeNodeToLLVMType` 加 slice 分支。
- **嵌套 slice 双下标 `rows[i][j]` 读写 + slice<struct> 字段访问 `v[i].x`**：
  codegen 只处理 Identifier slice / 标量 struct。新增 `sliceTypeOfExpr`（递归解析
  slice 值类型）+ `generateSliceElementAddress`（解包+边界检查+GEP）统一读写路径；
  `generateArrayElementAddress` 扩展 slice 分支。
- 由 slice 类基准（slicevec/slicemat）暴露；回归测试 `tests/nested_slice/`。
- 回归：O0/O2/ASAN 177/177 全过；32 项基准 verify 全一致，slicevec 2.67 /
  slicemat 2.50 反超。

### v3.11.4
- **支持 struct 数组元素字段访问 `arr[i].field`**：此前 `v[i].x` 读/写都报
  "unknown property 'x'"——codegen 只处理标量 struct（Identifier 对象）和链式成员
  （MemberAccess 对象），未处理 Subscript 对象（sema 能解析、codegen 挂）。新增
  `generateArrayElementAddress` 计算 struct 数组元素地址，接入读/写/链式三路径，
  并支持 struct 字段数组（`obj.arr[i]`）。
  - 由新基准 dotprod（struct 数组点积）暴露；回归测试 `tests/struct_array/`
    （字段写/读/循环/整体拷贝/链式嵌套 `o[i].in.a`）。
  - 回归：O0/O2/ASAN 176/176 全过；30 项基准 verify 全一致，dotprod 2.29 反超。

### v3.11.3
- **顶层函数 internal 化（最大一次性能提升）**：此前所有顶层函数发成 external 链接，
  LLVM -O2 内联器因内联成本超阈值拒绝内联（如 convolution 425 > 225）→ 调用点传的
  常量参数无法常量折叠 → 小循环因运行时上界被 cost-model 判"向量化不划算"→ 热点
  内核（convolution/kmp/sha256 等）全部退化为标量执行。
  - 修复：非库构建下把所有函数定义标记 `internal`（仅保留 `main` external），LLVM
    的 IPSCCP/内联器随即常量特化 + 内联 + 向量化；库构建 `--shared/--static` 跳过
    以保持符号导出（顺带修复单文件库构建的 `library_mode` 未传递问题）。
  - 基准效果（MYP -O2 vs C++ -O3，24 项 verify 全一致）：convolution 0.61→1.32、
    kmp 0.63→0.96、base64 0.81→1.29、sha256 0.82→1.20、kmeans 0.83→1.05、
    huffman 0.94→2.12；gol 因常量折叠后 SLP 过度向量化 119→153ms（仍 1.06x 领先
    C++）。
  - 回归：O0/O2/ASAN 175/175、TSan 12/12 全过。

### v3.11.2
- **显式类型转换 `uint8(x)` / `byte(x)` / `long(x)` / `double(x)`**：内置类型名当
  函数调用即转换。宽→窄截断、窄→宽按源符号扩展、double↔int 转换。解决了
  "无法从 `long` 计算填充 `uint8[]`"（base64 基准 0.58→0.81，`uint8[]` 替代
  `long[]` 省 8 倍内存）。
  - 新增回归测试 `tests/convert_expr/`；自举 viz lexer 同步 intN/uintN 关键字。
  - 顺带修复既有缺口：float→double 调用实参（`Console.writeFloat(float 变量)`
    之前 LLVM verify 失败）。
  - 回归：O0/O2/ASAN 175/175 全过。

### v3.11.1
- **修复数组下标窄整数符号扩展 bug**：`cnt[msg[i]]` 里 `msg[i]` 是 `uint8`(i8)，
  作为数组下标被 LLVM GEP **符号扩展**——字节值 >=128（如 190=0xBE 即 i8 -66）变成
  负下标，计数丢失 + 越界写（段错误）。huffman 类基准暴露。
  - 修复：`generateSubscript`/下标赋值/GPU kernel 的 GEP 索引统一**零扩展**
    （`zextIndexValue`：i8/i16/i32 → i64），与 slice 路径的 zext 约定一致。
  - 新增回归测试 `tests/subscript_narrow/`（uint8/uint16 作读+写下标 + int 控制组）。
  - 顺带收益：gol 基准 MYP 165→~120ms（索引变 i64 后 LLVM 对邻居循环优化更好）。
  - 回归：O0/O2/ASAN 174/174 全过。

### v3.11.0
- **无符号整数类型补全（`uint32`/`uint8`/`uint16`/`uint64` 固定宽度别名）**：
  - 新增 `u`/`U` 字面量后缀（`0xFFFFFFFFu`），按值定宽（≤0xFF→`ubyte`、≤0xFFFF→
    `ushort`、≤0xFFFFFFFF→`uint`、更大→`ulong`），可直接初始化无符号变量/数组。
  - 无符号语义：`uint` 的 `>>` 是逻辑右移（`lshr`）、`/`→`udiv`、`%`→`urem`、
    比较用无符号谓词、加减自动回绕；uint→long 拓宽用 ZExt（`0xFFFFFFFFu`→
    `4294967295L`）。`(x>>n)|(x<<(32-n))` 被 LLVM 识别为单条 `rol`/`rorl`。
  - sema：`visitBinaryOp` 无符号类型推断；`typesCompatible` 拓宽表加无符号族；
    codegen：二元运算按 `result_unsigned` 选 UDiv/URem/LShr/无符号比较，新增
    `convertIntegerValue` 助手使调用实参/变量初始化对无符号源做 ZExt。
  - 自举格式化/可视化器同步 `u` 后缀与新类型关键字（对拍通过）。
  - 新增回归测试 `tests/unsigned_types/`（逻辑右移/无符号除/比较/回绕/旋转/ZExt）。
  - 效果：bench sha256 用 uint32 后 32ms→22ms（比值 0.56→0.91），verify 不变。
- **有符号定宽别名 `int8`/`int16`/`int32`/`int64`**（→`byte`/`short`/`int`/`long`，
  与 uint 家族对称补齐）。

### v3.10.2
- **变异模糊测试驱动的 7 项修复**（`tools/fuzz_myp.py`：对 tests/examples/stdlib 种子做
  行级/表达式级变异，用 ASAN 编译的 `mypc` 编译 + 超时分类输出 ——
  CLEAN / CRASH(ASAN) / VERIFY(LLVM verify failed) / INTERNAL / HANG；本次跑 12000
  次迭代收敛为 0 发现）：
  - **未知类型导致 sema/codegen 签名分歧**（`propertyvoid`/`UnknownType`）：sema
    `typeNodeToTypeInfo` 之前对未注册类名静默回退 Void，而 codegen 把同名解析成 i32 →
    函数签名不一致 → LLVM verify "return type does not match"。Fix：未找到类时报
    `unknown type 'X'`（@macro 的 AST 类型 StmtList/Stmt/Expr 保留 Void 兜底）。
  - **非 void 函数缺失 return 落到 `ret void`**（verify 崩溃）：sema 新增保守的
    `stmtGuaranteesTermination`（Return/Throw/Block 末句/If+else 双支/Match 全臂/
    Try+finally/while(true)/for(;;)）+ `checkMissingReturn`，在 `analyze` 与
    `visitFuncBody` 的 action/function/struct 方法/顶层函数各调用点报
    `missing return statement`（跳过空体 FFI 桩与 @coro/@async 的 Void 保护）。
  - **空体非 void 函数/action/static 返回 `ret void`**（`int helper() {}`、
    `@async long sleep() {}`）：codegen 三处 fall-through（generateClassFunction /
    generateClassAction / generateStaticAction）按返回类型补零值 `CreateRet`。
  - **编译器 use-after-free（ASAN）**：`visitFuncBody`/action 的
    `visitStmt` 触发的单态化会 realloc `tu.functions`/`tu.classes`，读
    `decl.range`/`decl.body` 悬垂。Fix：visitStmt **前**捕获 `SourceRange` 与
    `shared_ptr` body。
  - **`throw <void 表达式>`**（`throw Console.writeString(...)`）：codegen 会发
    `myp_throw_object(<badref>)` → verify。Fix：visitThrowStmt 对真 void 报
    `throw requires a string or class instance, got 'void'`（仅在有既有错误时静默恢复）。
  - **泛型模板体 use-before-decl 的 codegen 晚期错误**（`map<T,R>` 体内 `r` 未定义，
    sema 跳过模板体）：codegen 之前用 i32 0 占位 → LLVM verify 崩溃。Fix：
    `CodeGen::generate` 在 finalizeDebugInfo 后 `diag_.hasErrors()` 即干净中止
    （不再把类型不兼容占位送入 verify）。
  - **枚举体错误恢复死循环（HANG）**（`enum { a.b; }` / `enum { 1; }` / `enum { "s"; }`）：
    `parseEnumDecl` 体内 token 既不能被 parseIdentifier 也不能被 consume(Semicolon)
    消费时（两者失败都不前进）→ 无限循环。Fix：循环内记录 `before = current_`，
    迭代无前进则 `advance()` 一次保证前向进度。
  - **嵌套 struct 成员赋值静默失效（顺带发现的 wrong-code bug）**：
    `o.inner.val = 3.14` 之前落到 codegen `unknown property` 兜底（写 i32 0），
    且 mypc 退出码为 0 → 错误 expected 文件掩盖（`inner.val=0`）。Fix：新增
    `generateStructMemberAddress`（递归解析 `v.a.b` 链地址），赋值处理器在
    `if(!op)` 层级处理 `MemberAccess` 目标并 GEP/store；`tests/nested_struct`
    expected 更新为真值 `inner.val=3.14`。
  - 验证矩阵：**-O0/-O2/ASAN 全套 173/173、TSan 12/12**；12000 次 fuzz 收敛为 0
    CRASH/VERIFY/INTERNAL/HANG。

### v3.10.1
- **系统探测驱动的 8 项修复**（`tests/probe.sh`：编译+运行每个 `.myp` 的 -O0/-O2
  输出与崩溃对比；覆盖枚举/类数组/泛型/命名 lambda/字符串/slice/异常/协程/接口/静态/
  数值运算符，~20 个探测用例全部 PASS）：
  - **`new Generic<Arg>[n]` 解析失败**（`new Box<int>[2]`）：parser 解析完泛型实参后
    遇 `[` 未走数组分支。Fix：type_args 后检查 `[` → NewArrayExpr（元素带 type_args）。
  - **泛型类数组元素类型解析成 i32**：`typeNodeToLLVMType`/`typeNodeToCodegenType`/
    `isArcClassType` 未对 class_name+type_args 做实例 mangling（`Box<int>`→`Box_int_inst`）。
  - **数组元素方法分派到模板**（`boxes[0].take()` → 模板签名未定义）：新增
    `array_elem_class_map_`（数组变量→元素类名），`best_class` 的 obj_cls 对 Subscript 用它。
  - **struct 字段数组下标元素类型解析失败**（`bg.nodes[0]` 元素 i32 错读）：generateSubscript/
    赋值下标的 MemberAccess 分支只查类、漏 struct 字段；加 `findStruct` 字段类型解析。
  - **`new Box<Node>().make2()` 方法解析到模板**：`memberObjectClassName` 辅助（Identifier/
    this/Subscript/NewExpr/Call → 类名，泛型 mangling）。
  - **内联调用类返回方法泄漏 retain-at-return 的 +1**（`obj.take().get()` 每次泄漏 1）：
    `generateCall` 包装器对 `callReturnsArcRef`（callee 返回类/类数组）的结果 push 语句末
    临时；`return f()`（f 返类）作 fresh 转移（consume+skip retain）。
  - **三元表达式数值字面量分支类型不统一**（`x>0 ? 1 : x` byte vs int 报错）：sema
    `isNumericKind`/`commonNumericKind` 统一到较宽数值类型；codegen `generateTernary`
    两分支 SExt/FP 拓宽到共同类型再建 phi。
  - **链式泛型方法调用**（`pp.first().first()` 外层对象是 CallExpr）回退到模板：
    `callReturnClassName` 解析调用返回类型类名；`memberObjectClassName` 加 Call 分支；
    两处 obj_cls + 名字兜底统一用它。
  - 验证矩阵：**-O0/-O2/ASAN 全套 173/173、TSan 12/12**；showcase O0==O2==ASAN 逐字节一致。

### v3.10.0
- **showcase 差分测试驱动的 4 项修复**（`examples/showcase.myp` 作为语言能力展示 +
  差异测试工件，暴露并回归了以下缺陷）：
  - **枚举带数据变体载荷恒为 0**：`Shape.Circle(2.5)` 之前只存判别值、match 绑定硬编码 0
    （`data(v)=0`/`radius=0`）。修复：枚举 LLVM 类型从 `i32` 改为结构体
    `{ i32 disc, [N x i8] payload }`（N = 最大变体载荷字节）；构造打包载荷
    （`buildEnumVariant`，与 `getEnumStructType` 同字节偏移）、`generateMatchStmt`
    提取判别 + 按偏移解包载荷绑定、等值比较按判别（Eq/Ne/Lt..）；`typeNodeToLLVMType`/
    `typeNodeToCodegenType`/局部变量分配均识别枚举。`tests/enum_match` 期望更新为真值。
  - **`ArrayList<T>`（T 为类）销毁时元素不释放** + **动态/固定类数组元素泄漏**：
    `new T[n]`（T 为类）之前走裸 `myp_region_alloc`，无长度、无释放。修复：**引用计数类数组**
    ——运行时 `myp_alloc_class_array` 分配 24 字节头
    `{ count:u64, elem_size:u32, pad:u32, rc:u32, type_id=MYP_ARR_TYPE_ID }`
    （rc/type_id 与类对象头同偏移，`myp_retain`/`myp_release` 统一可用；
    `myp_release` 见 magic 即逐元素释放再 `myp_free_class_array`）；
    codegen `isArcRefType` 对动态类数组返回 true（销毁桩/字段存储/作用域退出/临时释放
    自动接管）、`generateNewArrayExpr` 类元素走 `myp_alloc_class_array` + 语句末临时、
    局部动态类数组注册 ARC 槽、固定 `[N x T]` 栈数组注册 kind-3 槽（`myp_release_fixed_class_array`
    按 count 释放元素不 free 栈缓冲）、`return` 转移/retain-at-return 覆盖类数组、
    `heapCopyArrayReturn` 对固定类数组做引用计数深拷贝。
    `tests/arc_m2` 期望更新（`after=2` 不再编码数组泄漏）。
  - **泛型模板体 for-in 崩溃**（sema 跳过模板体 → ForInStmt 注解未计算 → codegen 读默认值
    → LLVM 对齐栈溢出）：模板体递归注解（`annotateForInsInStmt`）+ codegen 兜底报清晰错误
    （不再崩溃；迭代泛型集合仍建议索引循环）。
  - **命名 lambda 自引用失败**（`fn fact(n) => ...` 递归本名解析不到）：AST 加
    `LambdaExpr::name`/`ClassDecl::lambda_name`；sema 在 `__call` 作用域声明自名
    （函数类型、不捕获），调用解析 `resolved_call_name = <cls>__self`；codegen 识别
    `__self` 后缀走 `this` tramp 递归。`/tmp/nl2.myp`（含捕获 + 自递归）O0/O2 通过。
  - 验证矩阵：**-O0/-O2/ASAN 全套 173/173、TSan 12/12**；`arclist.myp`（d1=0）、
    `arrleak.myp`（fixedarr=0/dynarr=0）、`arrfull.myp`（传参/返回/覆盖/字段级联，leak=0）。

### v3.9.0
- **异常 × -O2 修复（§五-3 × 优化管线）**：`-O2` 全套复验（套件涨到 173 后首次）暴露
  `result` **段错误** + `arc_throw` **泄漏**——异常 dispatch/propagate 读 try 内 ARC 槽，
  其唯一 def 在 try_block（不支配 longjmp 路径），LLVM 把 load 折叠成 `undef`
  （`MYPC_DUMP_OPT_IR=1` 可见 `call myp_release(ptr undef)`）。修复：运行时
  `myp_release_slot(槽地址, kind)` 读**物理槽位**再释放（对 LLVM 不透明）+ `registerArcSlot`
  内 `myp_try_escape` 让槽逃逸保住 try_block 的 store（协程帧镜像仍先 `emitCoroFrameClear`）。
  **附带**：`mypc run` 支持子命令前 flag（`-O2 run file.myp`，提取 -O 级传给编译）；
  5 个自举子脚本（pm/gitee/fmt/viz/run）`"$MYPCC"` 引号在 `MYPCC` 带参数时失效→去引号 +
  `MYP_ABS` 取首个词。验证矩阵：**-O0/-O2/ASAN 全套 173/173、TSan 12/12**。
- **反射 / RTTI（§五-4，additive）**：class 对象头 `{rc, type_id}` 自带运行时类型 id。
  新增 `stdlib/rtti.myp` 静态类 `Rtti`——`typeOf<T>(obj)`（运行时类名）、
  `typeId<T>(obj)`（运行时类型 id）、`sameType<T,U>(a,b)`（同类型判定；null → id 0 / 空名）。
  实现：codegen 并列生成 `__myp_type_name_table`（type_id → 类名，ExternalLinkage 常量表，
  无类 TU 退化为 `[null]`）；新内建 `__myp_type_id`/`__myp_type_name`（sema 注册 +
  `intrinsic_map_` 解析）→ runtime `myp_obj_type_id`/`myp_obj_type_name`。`tests/rtti`。
  - **§三 类型系统增强（additive）**：
  - **类型别名 `type X = ...`**：上下文关键字（仅顶层 `type <Id> = <Type> ;` 形态），
    别名可在任何类型位置使用、支持别名套别名、递归别名编译报错。`tests/typealias` + 负测试。
  - **`Option<T>` 可空容器 + `T?` 语法糖**：stdlib `option.myp`（`Option()`=none /
    `Option(T v)`=some + `isSome/isNone/get/getOr/set/clear`）；`Type?` ≡ `Option<Type>`
    （类型位置）。`tests/option`。
  - **泛型函数 `T foo<T>(T x)`**：显式类型实参或实参推断（含 `T[]` 推元素类型）；
    按类型实参单态化（`foo_int_inst`），模板不生成运行时代码。`tests/generic_func`。
  - **一等函数与闭包（M-FN-1/2/3）**：函数类型 `(A,B)->R`（胖指针 `{closure, call_fn}` +
    统一 tramp）；lambda 按值捕获（标量/字符串深拷贝、class 引用浅拷贝、嵌套、上下文类型
    推断）；命名 lambda `fn name(...) =>` 递归；泛型高阶函数 `mapOpt`/`foldInt` + `Option.map`。
    设计见 `docs/function.md`；`tests/function`。
  - **元组 + 解构（TUP-1/2）**：元组类型 `(int, string)` + 字面量 + **多值返回** +
    声明式/赋值式/嵌套解构 + 字段访问 `t.N`；与函数类型/lambda/元组变量声明四路消歧。
    设计见 `docs/tuple.md`；`tests/tuple` + 3 负测试。
  - **泛型 `@static` 类方法（M-FN-3 stdlib 落位）**：`static:` 段方法名后带类型参数
    （`List.map<T,R>`），跨模块可见；`resolveGenericStaticCall` 单态化到 `tu.functions`
    （`__gs_<Class>_<method>_<types>_inst`）。`tests/generic_static`。
  - 全库回归：141/141（`-O0` + ASAN）。
  - **trait 默认实现（§三-5，additive）**：
  - 接口方法**带默认体** → 实现类可省略该方法，虚表回退默认函数；类覆盖则用覆盖。
  - 实现：按类特化默认函数 `__ifdef_<Iface>_<method>_<Class>`（预声明 + generateClass 生成），
    `this` 绑定具体类 → 默认体内 `this.method()`/裸方法调用**静态解析到类方法**（含默认调默认）；
    sema `checkInterfaceImpl` 放行带默认体的接口方法，纯签名方法仍强制实现（负测试）。
  - `tests/interface_default`（默认/覆盖/this 分派/默认调默认）+ 负测试。
  - 全库回归：144/144（`-O0` + ASAN）。
  - **关联类型（§三-5，additive）**：接口 `type Item;` 声明抽象关联类型，实现类
    `type Item = int;` 绑定（**必须绑定**，负测试 `assoc_unbound`）；接口方法参数/返回
    引用关联类型；绑定经 `X::Item` 直接引用（局部变量/参数/返回类型）；泛型
    `where T:I` 内 `T::Item` 实例化后单态化为具体绑定（约束类型参数注册为接口类型、
    `T::Item` 替换、Assoc 通配；codegen `::` 拦截 + `resolveAssocType` + 类参数
    `var_class_map_` 注册使 `c.method()` 精确解析到具体实例类）。
  - `tests/assoc_types`（基本引用 + 泛型 T::Item + 关联类型参数/返回）+ 负测试。
  - 全库回归：146/146（`-O0` + ASAN）。
  - **class 实例 ARC（§五-1，M-ARC-1，additive 无新语法）**：自动引用计数回收中寿命对象。
    - 对象头 `{rc:u32, type_id:u32}`（数据指针前 8 字节）；`myp_alloc_object/retain/release/
      free_object`；每类销毁桩 `__myp_destroy_<Class>`（级联释放类/接口引用字段）+ 按
      type_id 分派的 `__myp_release_table`（ExternalLinkage，运行时按需读取）。
    - 插桩：作用域退出释放局部类/接口引用槽（参数/`this` 借用不释放）；函数返回
      **retain-at-return**（借用返回/新对象均覆盖，调用方转移接管）；赋值 retain-new
      （fresh new/call 转移）release-old（自赋值安全）；属性存储（`this.prop`/裸 `prop`/
      静态属性/映射全局）retain；`slice<T>` 类元素 retain/release；`var x = new` 归槽。
    - 修复潜在 bug：类结构体**两遍构建**（自/交叉类属性引用解析为 ptr，原为 i32 致
      指针字段损坏，ARC 测试暴露）；`myp_release` free 前缓存 rc（原 free 后读头 → UAF）。
    - 诊断：`Memory.liveObjectCount()`（当前线程存活实例数）。
    - `tests/arc`（生命周期/级联/自赋值/借用返回/循环不累积）；全库回归 147/147（-O0+ASAN）。
    - **M-ARC-2（2026-08-06）**：`T[]` 数组元素 retain/release（局部/this.arr/obj.arr，
      slice 同）；语句末临时释放（`new` 作实参/丢弃不累积，强槽 store 消费）；`return new T()`
      转移（跳过 retain-at-return，修 M-ARC-1 fresh-return 泄漏）；函数 epilogue release
      （修 return 结尾局部泄漏）；`@thread`/`@threadpool` 实例在 `myp_thread_destroy` 释放
      startup_arg（线程池改 `myp_alloc_object` 带头）。修复：lambda 闭包/`@thread` 实例
      临时消费（ASAN 捕获的过早释放）；emitFunctionReturn 顺序（retain 先于 release、
      main 的 release 先于 `myp_free_all`）。`tests/arc_m2`；全库回归 148/148（-O0+ASAN）。
    - **M-ARC-3（2026-08-06）**：**闭包释放**——函数值局部注册为 ARC 槽（fat pointer
      index 0 = 闭包），作用域退出释放；`LambdaExpr` 视为 fresh（闭包是新分配的 class
      实例）；别名赋值 retain 闭包；捕获的 class 引用在 `generateLambda` **retain**（闭包
      拥有自己的引用，销毁桩级联释放平衡——修外层局部释放后闭包持悬垂借用的 UAF）。
      `tests/arc_fn`；全库回归 149/149（-O0+ASAN）。
    - **剩余**：异常/throw-catch 展开释放（v1 泄漏安全，`myp_free_all` 兜底）、协程帧
      释放、`@region` 逃逸精修。设计见 `docs/arc.md`。
    - **同步原语 stdlib（§五-2，additive）**：`sync.myp`——`Mutex`（普通+可重入
      `PTHREAD_MUTEX_RECURSIVE`，tryLock）、`RWLock`（pthread_rwlock，try rd/wr）、
      `CondVar`（wait 关联 Mutex handle + signal/broadcast）、`Semaphore`（POSIX sem_t，
      tryWait）、`Once`（enter/done call-once 惯用法）。全部 handle 模式（同 `Barrier`，
      固定数组+分配表+槽位互斥，每类 64 槽）。`tests/sync`（4 `@thread` worker 用 Mutex
      保护 `@static` 共享计数 400 确定性 + CondVar 生产者/消费者 + tryLock/递归/RWLock/
      Semaphore/Once API 检查）；全库回归 150/150（-O0 + ASAN）。
  - **for-in / 集合迭代（§四-2，additive）**：`for (x in coll)` / `for (T x in coll)` /
    无括号 `for x in coll`。四种可迭代源：**固定数组** `T[N]`（编译期长度）、**slice**
    `slice<T>`（`.size()`）、**集合类**（需 `size()`+`get(int)`，de-facto 迭代器协议，
    如 `ArrayList<T>`）、**range** `for (i in a..b)`（右开 `i<b`，等价索引式 for）。
    循环变量每次迭代声明（作用域级），`break`/`continue`/嵌套支持；迭代源只求值一次。
    ARC：类迭代集合适用临时持有 + 循环末释放；类元素（数组/slice 下标借用→retain、
    类 get 结果 fresh 转移）循环变量作用域末释放，实测零泄漏。负例：动态数组 `T[]`/
    非可迭代类/非集合类型/集合元素为数组均编译期报错。`tests/for_in`；全库回归
    151/151（-O0 + ASAN）。
  - **默认参数 / 命名实参（§四-1，additive）**：`Param ::= Type Identifier ('=' Expression)?`
    默认参数——顶层函数/类方法（action:/function:）/静态方法/构造器/struct 构造；事件与
    枚举数据字段不允许默认值。调用点 `f(name = value)` 按赋值表达式解析，sema 按「目标
    标识符匹配形参名」重解释为命名实参（乱序可用；与宏的赋值实参 `$n/$body` 无歧义——
    宏参数名永不匹配普通标识符）。位置实参按序填前 N 形参、命名按名填入、缺失且有默认值
    者克隆默认表达式在调用点求值；声明期校验默认值类型兼容。泛型函数/泛型静态方法亦支持
    （实例参数克隆保留默认值 + 实参规范化）。负例：未知/重复命名、位置+命名重叠、必填
    缺失、实参过多、默认值类型不匹配。`tests/defarg` + 6 负测试；全库回归 158/158（-O0+ASAN）。
  - **值式错误传播 Result<T,E>（§五-3，additive）**：`stdlib/result.myp`——`Result<T,E>`
    二态容器（`Result()`=err / `Result(T v)`=ok + `isOk/isErr/get/getErr/getOr/setOk/setErr`）；
    顶层泛型工厂 `resultOk/resultErr`（部分类型实参推断）；组合子 `resultMap/resultAndThen/
    resultMapErr`（无异常错误传播）；异常桥 `resultTry<T>((() -> T) f) -> Result<T,string>`
    （`catch (string s)` 优先 + `catch (Error e)` 用 `e.message()`）。`error.myp` 补
    `StringError.setMsg`。`tests/result`；全库回归 159/159（-O0+ASAN）。
    **附带修复 5 个既有 bug**：①lambda 捕获分析误捕全局函数/类名（`collectExprCaptures`
    加 `isGlobalName` 过滤，lambda 内可直接调用顶层函数）；②`string + bool` 拼接把 i1 传
    给 `myp_to_string_i32`（改 `myp_to_string_bool` + sext）；③泛型函数内 `new G<T>(args)`
    带参构造不调用构造器（codegen 按具体实例类名 + 实参个数重建 ctor）；④接口值来自函数
    返回值赋给接口变量时把胖指针当实例指针（interface passthrough）；⑤`catch (string)`
    绑定共享 `myp_error_msg` 缓冲、后续 throw 覆写导致存下的错误消息漂移（绑定前
    `myp_strdup` 拷贝——`resultTry` 多连调用即踩中）。
  - **stdlib 缺口补强（§六-4，additive）**：
    - **crypto/hash**：`stdlib/crypto.myp`——`Crc32.crc32/crc32Hex`（IEEE 802.3，
      原始 32 位值 + 8 位无符号十六进制显示）+ `Hash.md5/sha1/sha256`（小写十六进制）。
      核心在 C 运行时（`myp_crc32`/`myp_hash_md5`/`myp_hash_sha1`/`myp_hash_sha256`，
      MYP 侧静态类封装）。已知测试向量回归（含 56 字节跨块消息）。
    - **sprintf 格式化**：`stdlib/fmt.myp`——`Fmt` 静态类（默认参数）：`i`（十进制）、
      `u`（无符号十进制）、`x/X`（无符号十六进制大小写）、`o`（八进制）、`b`（二进制）、
      `f/e/g`（定点/科学/最短，精度）、`s/sR`（字符串左右对齐），全部支持宽度 + 填充字符。
      运行时补 `myp_fmt_u64_base`/`myp_fmt_double_f/e/g`（snprintf）。
    - **随机分布补强**：`random.myp` 加 `range(lo,hi)`（[lo,hi) 均匀）、`exponential(lambda)`
      （逆变换采样）、`poisson(lambda)`（Knuth 算法）。
    - **HTTP 客户端**：`stdlib/http.myp`——基于 `net.myp` TCP 的 HTTP/1.1 客户端
      （仅 `http://`，无 TLS）：`Http.get/post/request` + `HttpResult`（status/body/
      header 大小写不敏感/isOk）。URL 解析（scheme/host/port/path/query）；响应状态行/
      头解析；`Content-Length` 定长体、`Transfer-Encoding: chunked` 分块体、关闭定界体。
      连接失败抛 NetError、非法 URL/非 http scheme 抛 string。自包含测试：本地
      `TcpServer` 响应线程（@thread）验证 GET/404/chunked/POST/scheme 校验。
    - `tests/crypto` + `tests/fmt` + `tests/random_dist` + `tests/http`；全库回归
      **166/166**（-O0+ASAN）。
  - **`mypc run`（仿 `go run`）+ 单类文件自动 `main`（additive）**：
  - `mypc run file.myp [args]`：编译到临时产物 → 链接 → 直接运行 → 清理；退出码=程序
    退出码；args 透传（`main(argc,argv)`/构造器）。
  - **单类文件无 `main` 也可 run**：sema Pass 1 后注入合成 `main()`（实例化类并触发其
    `@startup` 入口）；`FuncDecl.is_auto_main` 豁免 main() 直接调用限制；无 `@startup` /
    多 `@startup` 类编译报错。正常编译（非 run）仍要求显式 main。
  - `tests/test_myp_run.sh` 8 断言；全库回归 142/142（`-O0` + ASAN）。
- **类构造器（M1-M4，additive）**：`@constructor` 注解 + **函数名==类名隐式构造器**。
  - M1 语法 + AST：`parseActionDecl`/`parseFunction` 识别 `@constructor`；`@constructor`
    构造器**无返回类型**（`@constructor Window(...)`，不写 `void`）且名称**必须==类名**
    （编译校验）；类/struct 中方法名==类名 → 隐式构造器（可省略注解，C++/Java 风格）；
    struct 新增 `action:` 节（方法/属性前瞻区分）。
  - M2 class 构造器绑定：`new C(args)` 优先绑定构造器（重载解析 + 数字提升
    `int → long → double`，歧义/无匹配编译报错）；泛型 `new Box<double>(1.5)` 绑定
    单态化实例类的构造器（根治 `@startup` 泛型分发 bug 的根因）；构造器重载
    mangling（`Class_Action_<paramtypes>`）；legacy `@startup init` 回退保留。
  - M3 struct 构造器：函数式构造 `Struct(args)`（栈临时 + 构造器；声明初始化/赋值/
    `return Struct(...)`/与 `operator:` 共存）。
  - M4 `copy()` 深拷贝约定（纯约定方法，无新语法）；manual/design/grammar/CHANGELOG 同步。
  - 修复既有 bug：`generateMemberAccess` 对类实例局部变量的 `.property` 访问错误返回
    实例指针（`c.data_[i]` 写入实例内存）；`checkStructMethods` 排除构造器为兄弟方法
    （其名==struct 名遮蔽 struct 类型名）。
  - 设计内联见 design.md §6.5；测试 `tests/constructor/` + `tests/copy/`。
- **@startup → 构造器迁移（不留 legacy）**：
  - stdlib 8 文件（`json`/`fs.Path`/`logger`/`net.TcpServer`/`net.TcpClient`/`regex`/
    `text.StringBuilder`/`time.Timer`）+ `stream`×3 与 `layers`×2 空占位删除。
  - 41 个测试 + examples + docs/examples + deeplearning + BNCT 的 legacy `@startup`
    （非 @thread）→ `@constructor`；`tests/startup` 重写为新语义（构造器 + @thread 线程入口）。
  - **移除** codegen 中 `new C(args)` 自动调 `@startup` 的 legacy 绑定；
    `@startup` 严格只作启动信号（@thread 线程入口）。
  - 验证：`-O0`/ASAN 全套 121/121。
- **`@parallel for` 多线程检测 + 线程池 API 扩展**：
  - 运行时新增 TLS `myp_pool_worker_id()`（当前 worker 索引，`@parallel for` body 内
    返回 0..N-1，非池线程 -1）；`emitKernelExpr` 支持其直接调用；
    `tests/parallel_for/` 用 `Parallel.workerId()` 检测多线程真正启动。
  - 池运行时新增 `myp_pool_worker_count()`（实际 worker 数）、`myp_pool_is_active()`
    （是否初始化）、`myp_pool_set_threads(n)`（池大小，首次创建前生效，0=自动）；
    `myp_pool_init_global` 遵循设定的线程数。
  - `stdlib/pool.myp` FFI 集中于此文件，`Parallel` 静态类扩展为完整查询/配置 API：
    `threadCount()`/`workerCount()`/`workerId()`/`isActive()`/`setThreads(n)`。
  - 新增 `tests/pool/`（`setThreads(2)` 确定性断言池大小、worker 索引区间、池内外 workerId）。
  - 验证：`-O0`/ASAN 全套 123/123。

### v3.8.0
- **切片类型 `slice<T>` + `@region` 两级 arena（P4，additive）**：
  - P4a `slice<T>` 值类型：`{ T* data; int64 len }` fat pointer；`new slice<T>(n)`、
    `s[i]`（运行时边界检查）、`.size()`/`.length()`、值传递/返回（逃逸→进程级）、
    浅拷贝共享 data、空 slice；grammar v1.0 列为内置类型。
  - P4b 两级 arena + `@region` 注解：region-local arena（`emitRegionEnter` + TLS 追踪），
    `@region` 函数退出时回收本 region 分配，返回引用类型时逃逸到上层。
  - P4c slice 集合二元：`@op("+")`/`@op("*")` 元素级运算 + 标量广播（A+B / A*k / k*A）。
  - P4d grammar 增量（`slice<T>`/`@region` 入规格）+ region 动态作用域（嵌套/逃逸规则）。
  - 测试：`tests/slice/`、`tests/slice_binop/`、`tests/slice_more/`、`tests/region/`、
    `tests/region_chain/`。设计见 `docs/slice.md`；算子见 `docs/operators.md` P4。
- **集合动态扩容**（`stdlib/collections.myp`）：`ArrayList`/`HashMap`/`Set`/`Deque`/`Queue`/
  `Stack`/`PriorityQueue`/`LinkedList`/`StrHashMap` 全部突破固定 1024 上限。
  - 惰性分配（首操作时 `new T[cap_]`）+ 容量翻倍扩容；哈希类 75% 负载因子翻倍重建；
    环形缓冲 grow 重排；LinkedList 节点池翻倍。
  - 规避 `@startup` 泛型分发 bug（`new Box<double>()` 曾误调模板 init 致堆损坏）。
- **泛型 `new T[n]` 端到端支持**（additive，新语言能力）：
  - parser：`new Ident[n]` → 动态数组；局部声明歧义消除补 `Ident [] name`。
  - codegen：单态化类型参数映射（`current_type_params_`），`new T[n]` 用真实元素类型分配。
  - 支持 `new Foo[n]`（类数组）与 `new T[n]`（泛型参数）。
- **`function:` 段跨方法调用修复**：sema 声明类作用域符号表 + codegen 预声明函数符号，
  方法可调用段中任意位置的方法（此前只能调用靠前的）。
- **LSP 解析错误恢复死循环修复**（内存爆炸根因）：
  - `parseBlock`/`parseMapping` 的 body 循环加"必须前进"保证（`current_` 未变则强制 `advance`），
    畸形输入（如函数体内孤立 `class`、`mapping =>`）不再无限循环耗尽内存（曾致 `myp_lsp`
    膨胀至 35GB）。
  - 模糊测试 5 seed × 600 例随机畸形输入 = 3000 例 0 挂起；全 stdlib 31 文件 LSP 实测 4MB。
- **`stdlib/memory.myp` 修复**（预存坏文件）：`ffi void*` 语法不支持 → 改用 MYP 惯例
  **指针以 `long` 承载**（同 json/regex 的 handle）；类名 `Mem` → `Memory`（对齐 manual）；
  移除基于不存在的 `__myp_ptr_write/read` 的 `DynamicArray`（动态数组统一走
  `collections.ArrayList<T>`）。`Memory` 提供 alloc/free/realloc/release（FFI 裸内存 +
  确定性释放）。新增 `tests/memory/`。
- 验证：`tests/collections_grow/`（全集合超 1024）；`-O0`/`-O2` 全套 115/115；ASAN 115/115。

### v3.7.0
- **DAP 调试支持（M7）**：`src/dap/dap_server.cpp` → `myp_debug`（DAP ↔ gdb MI2 桥）。
  - 支持：initialize/launch/setBreakpoints/configurationDone/continue/next/stepIn/
    stepOut/threads/stackTrace/scopes/variables/evaluate/pause/disconnect。
  - 复用 `-g` DWARF：VS Code 设断点/单步/查局部变量。
  - VS Code 扩展 `vscode-myp` 注册 `myp` 调试器（`myp.debuggerPath` 配置 + 自动探测）。
  - 验证：`tests/test_dap.py`（launch→断点→stopped→栈/局部变量/evaluate，15 断言）；
    `-O0`/`-O2` 全套 114/114。
- **文档手册完善**：`docs/manual.md` 第 12 章补完整命令行选项表、优化（`-O`/`--passes`）、
  调试（`-g`/gdb）、元编程（`@eval`/`macro`/`@macro`+`quote`）章节。

### v3.6.0
- **元编程 M4：过程宏 `@macro` + `quote`**（`src/eval/eval.cpp` 解释器扩展 + `src/macro/macro_expand.cpp` 集成）。
  - `@macro` 注解修饰函数（sema 跳过 body / codegen 不生成）；`quote { ... }` 上下文关键字
    （仅 `quote {` 识别为 AST 模板，`char quote = ...` 变量不受影响）。
  - 解释器 `EvalValue` 加 AST 值类型：`StmtList + StmtList` 拼接、`quote` 求值、`$x` 插值
    （数值→字面量、字符串→标识符（变量名 `int $name = ...` / 赋值目标 `$x = ...`）、AST→内联）。
  - 展开 pass：`@macro` 调用编译期执行（`evalProcMacro`）→ AST 替换；`main.cpp` Phase 3b
    对 `@macro` 也触发展开（修复：`ast.macros` 空但含 `@macro` 时未展开的 bug）。
  - 验证：`tests/proc_macro/`（`genAssign("x",42)` → `int x = 42`；`makeCalls(3)` → 3 条
    `Console.write(0/1/2)`）；`-O0`/`-O2`/ASAN 全套 114/114。
  - 设计见 `docs/metaprogramming.md` §5。

### v3.5.0
- **元编程 M3：声明式宏 `macro`**（`src/macro/macro_expand.cpp` + `include/mylang/Macro.h`）。
  - `macro name($a, $b) { ... }` 顶层声明；`$param` 为模板占位（新 token `Dollar` + `Keyword_macro`）。
  - 宏体是普通 MYP 块；展开为 AST 深拷贝 + `$param` → 实参 AST 替换（parse 后、sema 前）。
  - 支持表达式参数（`log($a)`）、语句参数（`repeat($n, $body)`）、赋值参数（`$x = ...`）、
    嵌套宏（`twice(addN(v,10))`，迭代展开 + 深度上限）。
  - `--macro-expand`：展开后 AST dump 调试。
  - 验证：`tests/macro/`（repeat+addN+twice+log → v=37）；`-O0`/`-O2` 全套 113/113。
  - 设计见 `docs/metaprogramming.md`。

### v3.4.0
- **元编程 M1：`@eval` 编译期求值**：`src/eval/eval.cpp` 轻量 MYP 解释器。
  - `@eval` 标记纯函数（标量/递归/条件/循环/`@eval` 互调/const 引用），编译期执行。
  - 顶层 `const int X = <expr>` 折叠：`const int FIB10 = fib(10)` 生成 `ret i32 55`。
  - 求值器在 sema 后、codegen 前运行（`main.cpp` Phase 4b）；非折叠初始化保持运行时行为。
  - 验证：`tests/eval/`（FIB10=55/FIB20=6765/HALF=2.5/BIG=true/T5=165/BIGL=1000000）。
- **元编程 M2：泛型约束 `where T : Interface`**：
  - 语法 `<T where T : Shape>`；sema monomorphization 时检查类型参数实现接口。
  - `DrawList<Circle>` 通过、`DrawList<int>` 编译期报错；`tests/generic_constraint/` + negative。
  - 验证：`-O0`/`-O2`/ASAN 全套 112/112。
  - 设计见 `docs/metaprogramming.md`。

### v3.3.0
- **自定义 LLVM pass（M6）**：`src/codegen/myp_passes.cpp` + `include/mylang/MypPasses.h`。
  - `MypRedundantStorePass`（FunctionPass）：消除同基本块内相邻同址死 store
    （如 `int x = 0;` 生成的双 store，实测 -O0 IR 每个变量多一次重复 store）。
  - `mypc --passes myp-pass` 可调用（`runMypPasses` 直接分发）；未知 pass 名报错。
  - `registerMypPasses`：追加到 -O 管线末尾 + 注册 `-passes` 解析回调（opt 风格嵌套）。
  - LLVM 21 备注：`parsePassPipeline` 顶层不解析自定义 pass 名（需嵌套 `module(...)`），
    故 `mypc --passes` 走自建分发。
  - 验证：`tests/test_myp_pass.sh`（6 项断言：可调用/语义/死 store 10→7/-O2 组合/未知拒绝）；
    `-O0`/`-O2` 全套 109/109。
  - 设计见 `docs/optimization_debugging.md` §3.5/M6。

### v3.2.0
- **DWARF 调试信息（M3-M5）**：`-g/--debug` 生成 gdb 可用调试信息。
  - M3：编译单元/文件/函数 DISubprogram + 逐语句行号（`break foo.myp:N` 命中）；
    `main.cpp` 全链路 `-g` 传参（单文件/多文件）。
  - M4：参数（`createParameterVariable`）+ 局部变量（`popScope` 集中 `dbg.declare`）
    变量映射（gdb `print a/b/sum/x/y` 正确）。
  - M5：类型映射（int/long/double/float/bool/char/指针/struct 成员/数组子范围）。
  - 验证：`tests/test_debug.sh`（gdb 批处理 6 项断言）+ `-g -O0`/`-g -O2` gdb 实测；
    `-O0`/`-O2` 全套 109/109、ASAN 109/109。
  - 说明：LLVM 21 已移除 `Type::getPointerElementType`（opaque pointer），
    指针类型统一映射为 `void*`；类实例显示为指针。
  - 设计见 `docs/optimization_debugging.md`。

### v3.1.0
- **IR 优化管线（M1）**：`-O1/-O2/-O3` 真正生效——`writeObjectFile` 用 PassBuilder NewPM
  运行 `buildPerModuleDefaultPipeline`（此前 `-O` 只影响后端指令选择，IR 无优化）。
  修复优化暴露的两个真实 bug：`setjmp` 缺 `returns_twice` 属性、`myp_throw` 误标 `noreturn`
  （后者导致 try/catch 在 `-O1/-O2` 下失效）。
  验证：`-O0` 与 `-O2` 双跑全套 109/109（`tests/run_tests_O2.sh`）+ ASAN 109/109。
  设计见 `docs/optimization_debugging.md`。

### v3.0.0
**里程碑版本**：整合 v2.4 系列全部累积，语言与运行时能力大幅扩展。所有变更 additive，语言规格保持 1.0。

- **协程完整体系（C1-C10）**：`@coro` 方法/顶层函数 + `await` 挂起/恢复 + 值传递 + 返回值槽 +
  自动调度器（`Coro.scheduler`）+ 事件等待（`await ClassName.eventName`）+ 超时（`await event timeout N`）+
  多事件等待（`Coro.waitAny`）+ 嵌套协程（`ret_ctx` 上下文链）+ 诊断（`Coro.status/current/count`）+
  协作式取消（`requestCancel`）+ 协程内异常边界（未捕获异常安全结束协程，不崩进程）+
  栈池复用 + `@coro(stack=N)` 栈可配置 + TLS 线程并用
- **协程间通信**：`stdlib/channel.myp` 缓冲通道（协程阻塞 send/recv + 唤醒）；协程 await Future（非阻塞等待）
- **并发/运行时**：动态事件等待表与动态事件队列（无 1024 硬上限）、工作窃取线程池（条件变量唤醒）、
  Barrier / Future / Promise
- **语言**：class 顶层 const、property 默认值修复（`int x = 5` 在 `new` 时生效）、Range for、
  类型推断 `var`、`long` 字面量后缀
- **语法**：算子系统（struct `operator:` 节 + 顶层 `@op("...")`）、管道 `|>`
- **工程**：异常机制完善（finally 全路径/`throw;` 重抛/对象异常/接口匹配）、LSP 稳定性修复、
  测试基础设施（普通 + ASAN 全套 109/109）

> **V2.0 → V3.0 升级说明**：见 [docs/UPGRADE_V3.md](UPGRADE_V3.md)。

### v2.4.3
- **协程（C1-C4 完整落地，additive）**：`@coro` 类 action 方法 + `await` 挂起/恢复
  - C1：spawn（create/set_entry/入口参数槽/首启）+ 手动 `resume`；`await;` 简单挂起
  - C2：`await` 值传递（`int v = await expr;`）+ `@coro` 返回值槽（`Coro.result`）
  - C3：自动调度器（就绪队列 + `Coro.scheduler()` round-robin）
  - C4：事件集成（`await ClassName.eventName` 阻塞等待 + 事件派发通知）
  - 用户 API 为编译器内建静态类 `Coro`（scheduler/resume/yield/isActive/destroy/result/waitEvent），
    `__myp_coro_*` 符号未注册（用户调用即 undefined）；`stdlib/coro.myp` 无 FFI 声明
  - 回归测试 `tests/coro/` + `tests/coro_auto/` + `tests/coro_event/`（普通 + ASAN 全套 94/94 通过）
  - 设计规范见 `docs/coro.md`
- **异常机制完善（additive）**：`finally` 全路径传播（return/break/continue）、`throw;` 重抛、
  未处理异常消息+abort、标准异常类（FileError/JsonError/NetError 等库接入）、`catch (Error e)` 接口匹配
- **LSP 稳定性**：修复 `didOpen`/`didChange` JSON 解码 bug（`\n`/`\t` 转义处理），消除 35GB 内存爆炸
- **测试基础设施**：`run_tests.sh` diff 改用临时文件（规避 process-substitution FIFO 不可靠）；
  `time`/`timeline` 测试改为确定性断言（消除 sleep 精度 flaky）

### v2.4.2
- **管道 `|>`（P3，additive）**：算子组件流水线
  - `A |> Op`：Op 为算子类名（自动实例化）或实例（复用），调用其 `transform`
  - 左结合链式 `A |> Op1 |> Op2`（= `Op2.transform(Op1.transform(A))`）
  - 新增 token `|>`；低优先级（高于赋值）；配合 SetOp 契约
  - 回归测试 `tests/pipe/`
- **CI + 测试确定性**：
  - 新增 GitHub Actions（`.github/workflows/ci.yml`）：构建 + 全套测试 + fuzz + ASAN
  - `date`/`process` 测试改为确定性（移除墙钟日期与原始 PID）——套件从"84 通过 2 抖动"到"84 通过 0 失败"

### v2.4.1
- **算子系统（P1+P2，additive）**：`运算符 = 算子` 统一模型实施
  - struct `operator:` 节 + `@op("+")` 注解 → 值类型数学算子（`v + u`、`v * s`）
  - 顶层 `@op("+")` 函数 → 外部算子（内置类型 `A + B`、对称二元 `s * v`、跨模块）
  - 重载解析顺序：内建 → struct 内 → 外部（标量热路径不受影响）
  - 新增关键字 `operator`；设计规范见 `docs/operators.md`
  - 回归测试 `tests/operators/`（正常 + ASAN 套件通过）

### v2.4.0
- **语法冻结**：正式发布 `docs/grammar.md`（EBNF 语言规格 v1.0），建立版本策略
  与变更日志；`mypc --version` 同时输出编译器版本与语言规格版本。
- **稳定性**：安装 LLVM 致命错误处理器，把 `LLVM ERROR: ...` 崩溃转为干净的
  内部错误诊断（编译器"永不崩溃"保证的一部分）。
- **先前累积（自 v2.3 起）**：
  - GPU 计算：`@gpu for` 支持数学函数（编译时链接 CUDA libdevice）、`stdlib/cuda.myp`
    （设备信息 / GPU 数学 / Vectors 归约 / Matrix）。
  - 稳定性：ASAN/UBSAN 构建、修复 3 个真实内存 bug、fuzz 崩溃判定、no-crash 回归。
  - 标准库：`process`、`args`、`env`、`json`、`regex`、`base64`、`net`、
    `collections`（PriorityQueue / LinkedList）、字符串增强、`date`、`logger`。
  - 内存管理：字符串/JSON/文件系统函数改用 arena 追踪分配，修复泄漏。
  - LSP：惰性解析 + 循环提前退出，修复服务器死机。
  - BNCT：原生 MYP 引擎 + 蒙特卡洛示例 + 并行计算支持。
  - 模块化：新增模块测试体系。

### v2.3
- 接口多态 + mapping 增强（文件级函数作为链节点）+ 标准库流类型。
- `stdlib/atomic.myp`：原子操作（LLVM atomicrmw）。
- `stdlib/ui.myp`：终端 TUI 框架。
- SDL2 GUI 支持（FFI + C 桥接层 + `stdlib/sdl.myp`）。
- 逗号分隔多变量声明 `int a=1, b=2;`。
- Barrier / Future/Promise / `await` / `@coro` 初步实现。
- `const` 关键字（语法层面）。
- 位运算符 `& | ^ << >>` 完整支持。
- struct 方法完整支持 + `@static` 类属性访问。

### v2.2
- 内置测试框架（`--test` + `stdlib/test.myp` 的 `Test` 类）。
- `myp fmt` 格式化工具。
- 标准库扩充；非标准库文件移除 `__myp_*` 内部调用。

### v2.1
- 泛型、枚举、lambda、FFI、包管理器（`--package-path`）、LSP、VS Code 扩展。

### v2.0
- `var` 类型推断、字符串插值、区间 `a..b`、`myp_viz`。

### v1.x
- 事件驱动组件语言原型：`class/action/event/property/mapping` 核心。

---

## 破坏性变更记录

（规格 v1.0 前无记录义务。此节自 v1.0 起必须维护。）

- 无（尚无破坏性变更）。

---

## 待定 / 路线图

- 编译器"永不崩溃"保证的其余部分：fuzz 持续集成、断言覆盖。
- 自举（用 MYP 实现编译器前端）所需的最小语法子集审计。
- 算子系统（`运算符 = 算子` 统一模型）：struct `operator:` 节 + 外部 `@op` 函数 +
  `|>` 管道。设计提案见 `docs/operators.md`，实施为 additive 变更。
