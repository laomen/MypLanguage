# runtime myp化 · 协程层设计与栈增长模型

> 状态：**设计稿 + Phase A/B 已实施（2026-08-25 #36/#37）**。
> 已 MYP 化：create/set_entry/yield/resume/set_result/result/status/is_active/
> count/current_handle/cancel*/destroy/scheduler/trampoline + 表/代际/栈/退役 +
> **Phase B 事件/等待层（wait_event(_timeout)/wait_any/wait_any_of/sleep/wait_fd +
> 事件 notify + scheduler 截止期/fd poll/压缩）+ 帧表 ARC 镜像 + coro 诊断**。
> 待实施：通道/未来（myp_coro_wait_future/wake_future）、exec worker、栈池
> （Phase C，见 §4）。
> 目标：把 C 协程运行时（`@coro`/通道/未来/调度泵）MYP 化进 `runtime_myp/`（shadow
> 机制：`--shared` 外部符号 + 前置链接 + `--allow-multiple-definition` → MYP 定义优先），
> 并在写 MYP 协程运行时期间**同步设计栈增长模型**（避免 retrofit 成本）。
> 背景：迁移系列目标 = 去 gcc 工具链（mypc → lld → 预编译 runtime → runtime MYP 化）；
> 能力加在**自举编译器**（MYP），mypc 冻结。

---

## 1. C 协程运行时机制全景（MYP 化的参考基线）

以下均为 `src/runtime/runtime.c` + `src/runtime/coro_ctx.S` 已核实的实现。

### 1.1 上下文切换（核心，纯汇编，零 syscall）

- `coro_ctx.S` 的 `myp_ctx_switch(myp_ctx_t* save, myp_ctx_t* load)`：
  - 只保存/恢复 **callee-saved 寄存器**（rbx/r12-r15/rbp）+ **rsp/rip**（正是 C 函数级切换所需）。
  - 保存块 7 槽：`[rsp → rip, r15, r14, r13, r12, rbx, rbp]`；`myp_ctx_t = { void* rsp }`。
  - **零 syscall、无信号掩码**，单次切换 ~20-40ns。刻意替代 `ucontext` 的
    `swapcontext`（后者每次切换做 `sigprocmask` syscall ~200ns，慢 5-10x）。
- `myp_ctx_init(ctx, stack, stack_size, entry)`（x86-64）：
  - `top = stack+size` 16 对齐；`rsp = top-64`（保证入口 `rsp%16==8` 函数入口对齐）。
  - 保存块 `frame[0]=entry`（trampoline），`frame[1..6]=0`（r15..rbp，入口立即改写）。

### 1.2 协程生命周期

- `__myp_coro_create(stack_bytes)`：栈池取/malloc 栈 → `ctx_init(trampoline)` →
  槽位分配（空闲槽复用 / 扩容），`generation++`（句柄 = `generation<<32|slot`，
  复用槽代际递增使旧句柄失效）。`result_pending` 门控槽回收。
- `__myp_coro_set_entry(handle, fn_ptr)`：fn_ptr 是 **64 位**（绝不能 int32 截断）。
- `__myp_coro_yield(val)`：`yield_val` 存本协程槽（BUG-013：每协程独立，非共享）→
  切回 `ret_ctx`；恢复后返回 `resume_val`。
- `__myp_coro_resume(handle, val)`：写 `resume_val` → 切到协程 `ctx` → 返回
  `yield_val`。支持嵌套 resume（协程 resume 协程，调用方上下文存进对方 `ret_ctx`）。
- `__myp_coro_trampoline`：首次进入协程栈的入口——
  ASan fiber 切换 → `setjmp(jb)` + `myp_exception_push(&jb)`（**异常边界在协程栈上**）
  → 跑 `fn()` → `pop`；未捕获异常 longjmp 回边界 → 打印 + 正常收尾（不 abort 进程）。
  收尾：`__myp_coro_release_frame`（释放帧槽对象）→ 退役栈（延迟回收列表）→
  `active=0/ready=0` → `result_pending=1`（或被 discard）→ `try_recycle` →
  切回 `ret_ctx`。
- 调度：**合作式**。`myp_exec_pump_results` 逐个 resume ready 协程；另有**同步交接**
  （`__myp_coro_resume` 内联跑对端一步）省一轮 `Coro.scheduler()` 往返。

### 1.3 通道 / 未来（用户态阻塞 = 协程 park，非 OS 阻塞）

- 通道：环形缓冲（`head/count`，单次回绕免 idiv）+ 等待表
  （`send_waiters/recv_waiters`，`MYP_CHANNEL_MAX_WAITERS` 上限）。
  - send：满 → 若调用者是协程 → 入 send_waiters + `ready=0`（park）→ `__myp_coro_yield`；
    否则返回 -1。唤醒 = `myp_channel_wake_one`。
  - recv：空 → 同样 park；**park-resume 后必须重新校验 count>0**（多消费者，
    唤醒我们的值可能被别的消费者先取走 → 曾致 count 下溢 + 越界）。
- 未来：`myp_coro_wait_future`（park 当前协程）/ `myp_coro_wake_future`
  （re-ready 同线程等待者）。
- **关键：阻塞等待不依赖 futex/syscall**，是纯用户态合作式挂起。非协程调用者不阻塞。

### 1.4 帧表（ARC 清理镜像，对栈增长模型决定性）

- `__myp_coro_frame_set(slot_id, obj)` / `clear(slot_id)`：
  - **`obj` = 堆对象指针（ARC 引用值）** —— 是堆指针，不是栈内地址。
  - **`slot_id` = alloca 地址（栈内指针），但只作 set/clear 相等匹配键，从不解引用**。
- `__myp_coro_release_frame`：destroy/异常收尾时逐个 release 帧表里的 `obj`（堆对象）。

### 1.5 栈管理（当前 C 版）

- 默认 **128KB**（`MYP_CORO_STACK_SIZE`）；`__myp_coro_create(stack_bytes)` 可指定。
- 栈池（`MYP_CORO_STACK_POOL_MAX_BYTES=16MiB` 上限）；`>=1MiB`（`MYP_CORO_STACK_BIG`）
  绕过池直接 free。
- 退役栈延迟回收（不能就地 free，正运行其上）：`myp_coro_retired_add` + 下次
  create/调度器 `retired_drain`。
- 栈是 `malloc` 的（**无 guard page**）→ 溢出会静默写坏相邻内存。

### 1.6 线程边界（真正依赖 OS 的部分）

- `@thread` → `pthread_create`（= **clone syscall 56**）。
- `@async` exec worker 池 → `pthread_create`（`myp_exec_worker_loop`）。
- `@parallel for` worker 池 → `pthread_create`（`myp_pool_*`）。
- 定时睡眠 → `nanosleep`（**#23 已 MYP 化**）；定时器 → `myp_now_ms`（已 MYP 化）。

---

## 2. MYP 化可行性

| 组件 | 方式 | 依赖 |
|------|------|------|
| 上下文切换 | 自举 codegen 加 **`__myp_ctx_switch` 内联汇编内建**（复用 `__myp_syscall` 的 `asm sideeffect` 发射模式），发射同样的 push/pop + 换栈汇编 | **自写汇编内建** → 可去掉 `coro_ctx.S` |
| 协程表/槽位/代际句柄/空闲列表 | 纯用户态 MYP 数据结构（`@static class` 全局 + 数组） | 无 syscall |
| 栈池/退役列表 | 纯用户态 + `myp_arena_alloc`/mmap | 无 syscall |
| 调度泵 | 纯用户态事件泵 | 无 syscall |
| 通道/未来 | 环形缓冲 + 等待表 + park/wake，原子内建（`__myp_atomic_*`） | 无 syscall |
| 异常边界（trampoline setjmp） | **前置依赖**：要么 MYP 化异常层（setjmp 等价 = 保存寄存器到 25 槽），要么暂时保留 C 的 setjmp helper | 见 §5 风险 |
| 帧表 | `@static` 数组 + 原子 | 无 syscall |
| `@thread`/`@async`/`@parallel` 线程创建 | **保留 C 薄 pthread 包装**（clone 裸发繁琐：栈/信号掩码/TLS），或 `__myp_syscall(56)` 裸 clone | **OS 边界** |

**结论**：单线程协程核心（创建/yield/resume/调度/通道/未来/栈池/帧表）**可完全 MYP 化**，
唯一编译器改动是 `__myp_ctx_switch` 内联汇编内建；异常层是前置；线程创建是 OS 边界（留 C 薄包装）。

---

## 3. 栈增长模型设计

### 3.1 目标与约束（已核实的事实）

- 目标：避免固定 128KB 栈溢出（当前无 guard page，溢出 = 静默内存破坏）。
- **约束性事实**：
  1. 帧表 `obj` 是堆指针 → 栈搬移**不影响**（安全）。
  2. 帧表 `slot_id` 是栈内地址但**只作相等键**，从不解引用 → 栈搬移会使键失效
     （新 alloca 地址 ≠ 旧键 → set/clear 失配 → destroy 漏清/重复释放）。
  3. 保存的 `rsp`（在 `myp_ctx_t`/保存块里）是栈内指针 → 搬移需调整。
  4. **异常 jmp_buf 在协程栈上**（trampoline `setjmp`），保存寄存器含 rsp → 搬移
     需重定位活动 jmp_buf（运行时看不到、难追踪）。
  5. MYP 无 `&local`（指针经 long/FFI）→ 用户代码不产生栈内部指针（好消息）。

### 3.2 三模型对比

| 模型 | 机制 | 需编译器支持？ | 栈内指针安全性 | 评价 |
|------|------|----------------|----------------|------|
| **分栈 segmented** | 函数入口栈探针（rsp vs 段上限）→ 新段链接下方；**段不移动** | ✅ 自举 codegen 发探针 | ✅ 保存 rsp/帧 slot_id/jmp_buf 全保持有效 | **推荐**：无搬移/重定位，非 GC 语言唯一安全选择 |
| **栈拷贝 copying** | 接近溢出 → 2x 分配 → 拷贝 → 统一搬移内部指针 | ❌ 理论上可纯运行时（合作式 + 无 &local） | ⚠️ jmp_buf 链重定位难追踪；帧 slot_id 键搬移后失配 | 风险高，不推荐 |
| **固定栈+检测** | 保持固定 + guard page（mmap PROT_NONE + SIGSEGV）→ 干净报错 | ❌ 纯运行时 | —（不增长，只检测） | 第一版打底 |

### 3.3 推荐路径（分层）

**第 1 层（现在，写 myp_coro_t 时就设计进去）——结构预留 + 检测**：
- `myp_coro_t`（MYP 版 `@static class CoroEntry` 或 struct）**预留段字段**：
  `seg_top`（当前段顶）/ `seg_limit`（当前段下限）/ `seg_chain`（段链，0=单段）。
  即使第一版固定 128KB，字段先放好（retrofit 比先设计贵得多）。
- 固定栈 + 可配置（`__myp_coro_create(stack_bytes)` 已有）+ **guard page 检测**：
  - 栈区 mmap `PROT_READ|WRITE`，栈底一页 `mprotect PROT_NONE`。
  - SIGSEGV handler（`rt_sigaction`=13 + `sigaltstack`=131，raw syscall 可做）：
    若 fault 地址在协程 guard 页 → **干净报错**（打印协程 id + "stack overflow"）
    而非静默写坏；否则 re-raise。
  - 注：MYP 裸 syscall 做信号处理可行但繁琐（handler 需在 altstack 上跑、
    修改 ucontext 重定向），可先只做「检测+abort」最小版。

**第 2 层（后续里程碑）——真正的增长 = 分栈 segmented**：
- **codegen 栈探针**：每个 `@coro` 函数入口发射——
  `rsp = movq %%rsp`（内联 asm）→ `cmp rsp, seg_limit` → 低于则
  `call __myp_coro_stack_grow`。探针读当前协程的 seg_limit：暴露
  `__myp_coro_seg_limit()` FFI（返回 `myp_coro_current` 对应协程的段下限；
  `myp_coro_current` 已是运行时全局）。
- **运行时段链**：`__myp_coro_stack_grow` 分配新段（栈池标准尺寸段），**链接在
  当前段下方**（栈向下增长），更新 `seg_top/seg_limit`；**landing pad**——新段
  帧 unwind 出段底时经返回蹦床切回旧段续点（Go 后来弃分栈正是这段复杂度，须小心）。
- **不变量**：段不移动 → 保存 rsp、帧 slot_id、jmp_buf 全部保持有效（分栈相对
  拷贝的最大优势，也是选它的理由）。
- 栈池：缓存标准尺寸段；退役逻辑不变（按段粒度）。

**第 3 层（OS 边界）——线程**：`@thread`/`@async`/`@parallel` 保留 C 薄 pthread
包装（clone 裸发涉及栈/信号掩码/TLS，MYP 化收益低风险高），文档化限制。

### 3.4 必须接受的不一致

- 栈探针/增长是**自举 codegen** 改动，而 **mypc 冻结** → 旧编译器编译的程序不支持
  增长（mypc 只保证旧程序不回归）。与迁移系列「能力加在自举编译器」方向一致，需在
  CHANGELOG 注明双编译器差异。

---

## 4. 分阶段实施计划

| 阶段 | 内容 | 验证 |
|------|------|------|
| **A 协程核心（单线程）** | `__myp_ctx_switch` 内联汇编内建；协程表/槽位/代际句柄/空闲列表/栈池/退役；create/set_entry/yield/resume/trampoline（异常边界先保留 C setjmp helper 或 MYP 化异常层）；调度泵 | `tests/coro_stack`、`coro_bench`、`coro_slot_reuse`、`coro_handle_unique` 在 shadow 下全过 |
| **B 通道/未来** | 环形缓冲 + 等待表 + park/wake + 同步交接 | `tests/coro_channel`、多消费者/多发送者、`channel` 测试 |
| **C 栈检测** | `myp_coro_t` 预留段字段 + guard page + SIGSEGV 干净报错 + 可配置栈 | 深递归协程（如 200k 层）在 128KB 段内溢出 → 明确报错；`coro_stack_pool_cap` |
| **D 分栈增长（可选）** | codegen 栈探针 + 运行时段链 + landing pad | 深递归协程超 128KB 正常增长运行；`coro_stack` 深递归；性能对比（探针开销） |
| **E 线程边界** | `@thread`/`@async`/`@parallel` 留 C 薄 pthread 包装 | `threadpool`、`async_file`、`parallel_stress`（不 shadow 或部分 shadow） |

**前置**：异常层 MYP 化（`myp_exception_*`/`myp_throw`/`myp_try_escape` + setjmp 等价）
——否则 trampoline 异常边界需保留 C setjmp helper（§2 表）。

---

## 5. 风险与开放问题

1. **jmp_buf 重定位（拷贝模型）**：活动 jmp_buf 在协程栈上、运行时不可见 → 拷贝模型
   不可行，故选分栈（段不移动）。
2. **mypc 冻结** → 增长/探针仅自举编译器编译的程序受益；双编译器行为不一致（文档化）。
3. **探针开销**：每 @coro 函数入口 `mov rsp + cmp + 条件 call`（~几 ns），需基准测量
   （`coro_bench` 对比）。
4. **landing pad 复杂度**：分栈「新段返回旧段」的蹦床衔接是 Go 弃分栈的原因；MYP 版
   需仔细设计返回链（新段帧 unwinds 到段底 → 切回旧段续点）。
5. **信号处理（guard page）**：MYP 裸 syscall 做 rt_sigaction/sigaltstack/修改
   ucontext 复杂；先做「检测 + abort」最小版，增长留分栈。
6. **多线程**：C 用全局 `myp_coros` + `__thread myp_coro_current`；MYP 的
   `@static class` 是进程级（非 TLS，region.myp 已文档化此限制）→ 多线程协程表
   共享需自旋锁 + 文档化，或保留 TLS 访问 C helper。
7. **`myp_coro_current` 的 MYP 读取**：探针/调度需要读"当前协程"——MYP 侧需暴露
   `__myp_coro_current()` FFI 或自管 `@static` 当前协程槽（yield/resume 时维护）。

---

## 7. 运行时↔程序生成代码边界：release_table 全 MYP 化（2026-08-25 决策）

> **策略变更（用户定）**：mypc 不再冻结，**不留 C 胶水**——硬边界也全 MYP 化，必要就
> 改 mypc。以下记录 release_table 分发的 MYP 化方案（协程 setjmp 异常边界同理待解）。

### 7.1 现状与难点（已核实）

- C `myp_release_class_obj_ex`（runtime.c ~1660）：`myp_weak_notify_death`（已 MYP）→
  查 `__myp_release_table[tid]` → 间接调用 destroy stub → 否则 `myp_free_object`（已 MYP）。
- `@__myp_release_table` 是**程序生成**的全局（`[N+1 x ptr]`，按 type_id 索引 destroy
  stub）；两个编译器都发射**同名外部全局**：
  - mypc：`codegen_class.cpp:447` `ExternalLinkage`。
  - 自举：`codegen.myp emitArcSupport` `@__myp_release_table = global [N+1 x ptr]`。
- 难点：MYP 无 extern 全局声明、无裸函数指针间接调用（函数值是 fat pointer vtable
  分发）；runtime_myp 模块是独立编译单元看不到程序表。

### 7.2 方案（自举内建 + 外部引用，无需改 mypc）

利用「程序 .o 链接在前 + `--allow-multiple-definition` → 程序表胜出」：

1. **自举 codegen.myp 加两个内建**（runtime_myp 模块由自举编译，mypc 不用）：
   - `__myp_rtable_addr()` → long：发射 `ptrtoint ptr @__myp_release_table to i64`。
     运行时模块自身也定义了同名表（`--shared` 模块 emitArcSupport 发射），但链接时
     被程序定义覆盖（程序在前），引用解析到程序表。
   - `__myp_call_ptr(long addr, string obj)` → void：发射
     `%fn = inttoptr i64 %addr to ptr` + `call void %fn(ptr %obj)`（LLVM 21 opaque
     ptr 下 `call void %fn(ptr)` 合法）。
2. **alloc.myp 实现 MYP `myp_release_class_obj_ex`**：
   ```
   void myp_release_class_obj_ex(string obj) {
       long addr = __myp_str_ptr(obj);
       if (addr == 0) return;
       if (myp_weak_notify_death(obj) == 0) return;   // 弱观察者存活
       int tid = __myp_mem_load_i32(addr - 4);
       if (tid > 0) {
           long table = __myp_rtable_addr();
           long stub = __myp_mem_load_i64(table + long(tid) * 8);
           if (stub != 0) { __myp_call_ptr(stub, obj); return; }
       }
       myp_free_object(obj);
   }
   ```
   删除 alloc.myp 的 `ffi void myp_release_class_obj_ex` 委托；C 版留在 runtime.c 仅作
   非 shadow 回退（不算胶水——整个迁移系列都是 C 原版 + MYP shadow 双份）。
3. **验证**：shadow 测试（含引用字段的类对象 release 级联 + weak + Live 计数回基线）；
   bootstrap 16/16（新 fixpoint）；全量 323/323。

### 7.3 同类边界待解

- 协程 trampoline 的 setjmp/jmp_buf 异常边界（§2 前置依赖）——同理要么 MYP 化异常层
  （setjmp 等价 = 保存寄存器到 25 槽），要么自举加 `__myp_setjmp/__myp_longjmp` 内建。
- `@thread`/`@async` 线程创建（clone/pthread）——OS 边界，可留 C 薄包装或裸 clone 内建。

---

## 8. 内联汇编探针结论（2026-08-25 已验证 ✅）

`__myp_ctx_switch(save, load)` 内联汇编上下文切换**探针通过**（`bench/freestanding/
rt_ctx_probe.myp`，main→worker→main 双切换 + entryHit 标记）。**协程核心的上下文切换
可用 MYP 内联汇编实现，消除 coro_ctx.S 依赖。**

### 8.1 自举内联汇编能力（实测结论）

- **有**：自举 codegen 经 LLVM IR `call ... asm sideeffect "..."` 发射内联汇编
  （`__myp_syscall` 先例）。**没有源级通用内联汇编**——但已抽象为**通用内建 + 标准库**：
  - 通用内建（自举新增，mypc 无）：
    - `__myp_asm(asmStr, consStr, ...args)` → void / `__myp_asm_r(...)` → long：
      编译期常量 asm/constraints 串（`constStrVal` 递归求字面量/拼接）+ 后续操作数按
      各自 LLVM 类型作实参，发射 `call asm sideeffect`。**具体汇编用途不再在 codegen
      写死**。
    - `__myp_fn_addr("name")` → `ptrtoint ptr @name to i64`（取函数地址，设协程入口帧）。
  - 标准库封装：`runtime_myp/coro.myp` 的 `myp_ctx_switch(save, load)` 经 `__myp_asm`
    实现（`;` 分隔、`{rdi}/{rsi}`、caller-saved clobber、`1f/1:` 数字标签落空 epilogue）。

### 8.2 内联汇编配方（实测踩坑，全部已验证）

| 项 | 结论 |
|----|------|
| 指令分隔 | **`;`**（`\n` 不被 LLVM 内联 asm 解析——实测报 unknown token；`%=` 唯一标签也不被该版本处理） |
| 寄存器 | **`%reg`**（`%rbp`/`%rsp` 合法；**`%0` 报 invalid register name**） |
| 操作数 | **`{rdi}`/`{rsi}` 硬编码约束**（同 `__myp_syscall` 风格）。**不能用 `$0`/`$1`**——MYP 字符串里 `$` 是插值前缀，`$0` 报 undefined symbol '0' |
| 恢复地址 | `leaq 1f(%rip), %rax` + `jmpq *%rax` + **数字局部标签 `1:` 落在 asm 块末尾** → resume 时 jmp 1: 落空到 LLVM 后续 epilogue → 正常返回调用者。**不发 `ret`**（内联 asm 的 ret 会从被内联函数错误返回） |
| clobber | **必须列全部 caller-saved**：`~{rax},~{rbx},~{rcx},~{rdx},~{r8},~{r9},~{r10},~{r11},~{r12},~{r13},~{r14},~{r15},~{rbp},~{memory}`——上下文切换只保存/恢复 callee-saved，worker 执行会破坏 caller-saved；不列则 LLVM 以为保留、不 spill，resume 读垃圾（实测 run() 的 rdx 活值被破坏 → 段错误） |
| ctx 布局 | save/load 是 **8B 槽**（myp_ctx_t = {rsp}）；asm 把 7 值推上栈并把 rsp 写进 `[save]`；目标块 = 栈顶 56B，`[0]=入口`、`[8..56]=0`（**入口不是 rsp**——实测把入口当 rsp 会跳到代码段崩溃） |
| 双 push | LLVM 函数 prologue 已 push callee-saved + asm 再 push → 保存块捕获「prologue 后栈态」；resume 时 jmp 1: = LLVM epilogue 首 pop → 正确返回调用者（对称往返，LLVM 帧假设成立） |

### 8.3 探针设计（rt_ctx_probe.myp）

- `workerEntry` 顶层函数（`@workerEntry`，internal 同模块可 `ptrtoint`）跑在切换后的
  栈上，写 `Probe.entryHit` 后 `__myp_ctx_switch(workCtx, mainSave)` 切回。
- 验证：main→worker→main 往返，entryHit 检查；worker 栈 4KB（arena 分配）。
- 运行：`bash runtime_myp/build.sh`（shadow 11/11）。

### 8.4 对协程核心 MYP 化的意义

- `__myp_ctx_switch` 已可用 → 协程 yield/resume/调度可全 MYP 化（§2 可行性表中
  「上下文切换 = 内联汇编内建」从「需探针」升为「已验证」）。
- 剩余前置仍为**异常边界 setjmp**（trampoline）与**线程创建**（clone/pthread，
  OS 边界）。
- 性能参考：C coro_ctx.S 单次切换 ~20-40ns（零 syscall）；内联 asm 同构，预计同级。

---

## 6. 参考（符号/位置）

- `src/runtime/coro_ctx.S`：`myp_ctx_switch`（7 槽保存块，零 syscall）。
- `src/runtime/runtime.c`：`myp_ctx_init`(~4268)、`__myp_coro_trampoline`(~4148)、
  `__myp_coro_create`(~4243)、`__myp_coro_set_entry`(~4280)、`__myp_coro_yield`(~4297)、
  `__myp_coro_resume`(~4313)、`myp_coro_wait_future`(~5485)/`wake_future`(~5499)、
  `myp_channel_send`(~5361)/`recv`(~5395)、`__myp_coro_frame_set/clear`(~4434/4458)、
  `__myp_coro_release_frame`(~4473)、`myp_exec_pump_results`(~5078)、
  `myp_exec_worker_loop`(~5068)。
- 宏：`MYP_CORO_STACK_SIZE`(128KB, ~3846)、`MYP_CORO_STACK_BIG`(≥1MiB, ~4028)、
  `MYP_CORO_STACK_POOL_MAX_BYTES`(16MiB)。
- 自举 codegen 内联汇编先例：`tools/selfhost/src/codegen.myp` ~9336 `__myp_syscall`
  （`asm sideeffect "syscall"` 发射模式）。
- 帧表发射：`tools/selfhost/src/codegen.myp` `emitCoroFrameSet`(~810)/`emitCoroFrameClear`(~818)。
- 对象头/异常：preamble `%myp_jmp_buf = type { [25 x i64] }`（`ir_emit.myp`）。
- 通道 park 语义：`__myp_coro_yield`（§1.3）——用户态合作式挂起，非 OS 阻塞。
