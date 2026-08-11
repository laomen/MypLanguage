# MYP 引用计数内存管理设计（ARC on class 实例）

> 状态：**M-ARC-1/2/3/4 已实施**；异常展开、协程帧引用释放、`@region`
> 引用逃逸保护与 `slice<class>` backing 级联释放均已实施
> 关联：`docs/slice.md` §3（两级 arena 内存模型）、`docs/next_improvements.md` §五-1、
> `docs/grammar.md`（规格 v1.0 冻结——本设计 **additive**，无新语法）
> 决策背景：析构器已讨论排除（`docs/constructor.md`）；完整 GC 过重；手动 free 会打破
> MYP"天然无悬垂"根基。结论：**class 实例走引用计数**（自动、确定性、安全）。

---

## 0. 实施状态更新（2026-08，内存系列 M5–M9）

> 本文下方 §1–§15 为 v1 设计过程记录，保留供追溯；**以下为本系列实施后的最终状态**，
> 与 v1 冲突处以本节为准。提交：`46da555`(in-place 拼接)、`0a22eab`(M6)、
> `9c156d3`(M7)、`1966900`(M9)、`3250416`(M1/M2)、`04abe79`(文档标记)。

- **M8 · 计数对象扩大**：`string`、动态数组 `T[]`、`slice` backing 全部改为**引用计数**
  ——`myp_alloc_str`（`MYP_STR_TYPE_ID=0xFFFFFFFE`）、`myp_alloc_slice_backing`
  （`MYP_ARR_TYPE_ID=0xFFFFFFFF`，24 字节头 `{rc, type_id, elem_size, count, cap}`）。
  所有字符串拼接/数组字面量/`slice` 切分均经引用计数，作用域/覆盖自动释放；
  不再依赖 arena/进程退出回收。字符串 `s = s + x` 走 **in-place 拼接**（`myp_str_append`，
  rc==1 唯一字符串 realloc 原位扩展），长串累积由 O(n²) 降到 O(n)（实测 808ms→52ms）。
- **M5 · struct 引用字段参与 ARC**：struct 具值语义但被计数（kind-5 槽，值拷贝按字段
  逐个 retain、释放时按字段逐个 release）——struct 不再"引用字段不参与计数"。
- **M6 · 跨线程原子 ARC**：`rc` 为 `_Atomic uint32_t`；`myp_retain` 用
  `atomic_fetch_add(relaxed)`，`myp_release` 用 `atomic_fetch_sub(release)`（返回旧值），
  最后一次释放（旧值==1）`atomic_thread_fence(acquire)` 后再析构。分配/释放列表
  `myp_alloc_head` 由进程级自旋锁 `myp_alloc_lock` 保护（`@thread` 可安全跨线程传对象）。
  strict 校验下 rc 下溢/重复释放立即 abort。
- **M7 · `@weak` 弱引用**：字段注解 `@weak`（仅 class/interface 引用字段），不计数、
  目标销毁自动置空。运行时弱表：64 槽链式哈希 + 自旋锁；`myp_weak_store/load/clear`、
  目标死亡 `myp_weak_notify_death`（持锁置空全部槽并重查 rc，防并发升级竞争）。
  `return weakRead;` 弱→强一次性升级，不重复 retain。测试 `tests/weak_cycle`、`weak_non_ref`。
- **M9 · 内存诊断与严格校验**：`Memory.*` 静态诊断 API（存活计数/按类型计数/arena 与
  region 字节/协程槽与栈池统计）+ 分配失败注入（`MYP_FAIL_ALLOC=n` 或 `failAllocEnable`）+
  strict 头校验（release 下溢、非法 `type_id`、重复释放 abort，ASAN 默认开）。
- **M1/M2 · 协程资源上限**：协程句柄 `{generation<<32|slot}` 世代化（槽复用安全），
  TLS 空闲槽表；栈池字节上限 `MYP_CORO_STACK_POOL_MAX_BYTES=16MiB`、大栈
  `MYP_CORO_STACK_BIG=1MiB`（`@coro big` 旁路池）。诊断可查
  `coroSlotCount/stackPoolBytes/retiredCount`。
- **回归基线**：release 219/219、ASAN 219/219、ASAN stress 6/6、TSan 2/2、
  OOM 注入 13/13、BNCT 正常。

---

## 1. 背景与动机

### 1.1 现状（两级 arena，无 GC 无手动管理）

- 所有 `new` 经 `myp_alloc`（进程级，退出才释放）或 `myp_region_alloc`（`@region`，区域结束整块释放）。
- **无 GC、无手动 delete、无 RAII/析构**；`Memory` FFI 只对裸 `long` 指针，管不到 MYP 对象。
- `@region` 是唯一回收路径，但只覆盖**事务型短寿命**（每帧/每请求）。

### 1.2 缺口：中寿命对象

寿命**比 region 事务长、比进程短**的对象（持续累积的缓存/状态/会话/几何）：
既进不了 `@region`（跨事务存活），又无法手动释放 → **只增不减，长跑进程内存单调增长**
（BNCT 等实锤风险）。这正是"没有 GC 也不能手动管理"的问题。

### 1.3 为什么是 ARC

| 候选 | 否决/采纳 |
|------|-----------|
| 析构器（RAII） | ❌ 已讨论排除 |
| 完整 GC | ❌ 大工程、有暂停、不契合实时/GPU |
| 手动 free + 对象池 | ⚠️ 破坏"天然无悬垂"根基，靠纪律 |
| **引用计数（ARC on class）** | ✅ **采纳**：自动、确定性、无暂停、防悬垂 |

**关键契合**：MYP 组合而非继承（无 vtable 于具体类）、分配有唯一入口 → ARC 可做到
**低开销、无暂停**。`rc` 自 M6 起为**原子**（`_Atomic uint32_t`，relaxed/release 语义），
支持跨线程安全传递；同一对象仅在单线程内使用仍近乎零开销。无继承链使"按静态类型销毁"
多数可行（interface 经 `type_id` 动态派发）。

---

## 2. 设计边界（什么计数、什么不计数）

| 类型 | 是否 ARC | 理由 |
|------|----------|------|
| **class 实例** | ✅ **计数** | 中/长寿命主力（组件/状态/缓存）；分配有唯一入口、身份稳定 |
| `string` / `T[]` / `slice` | ✅ **计数**（M8） | 拼接/切片/数组字面量均为临时值，作用域自动释放；`rc` 在头中、热路径仅 `atomic_fetch_add/sub` |
| `struct` | ✅ **计数**（M5，值语义） | 值类型仍按字段计数：拷贝逐字段 retain、释放逐字段 release（kind-5 槽） |
| `@static class` 属性 | ❌ 不计数 | 进程级生命周期，全局单例，不参与回收 |
| `Memory` 裸缓冲 | ❌ 不计数 | 显式 FFI malloc/free，照旧 |

> **v1 边界（已扩展）**：v1 只对 class 实例计数；实测确认 string/数组累积成为瓶颈，
> 已于 M8 扩展到全部动态对象（引用槽位单一，机制复用——见 §0）。

---

## 3. 对象布局与运行时

### 3.1 对象头（公开 ABI 8 字节）

```
┌───────────────────┬──────────┬──────────┬──────────────────┐
│ tracking next/prev│ rc:u32   │ type_id  │ 对象数据（字段）  │
└───────────────────┴──────────┴──────────┴──────────────────┘
                    ↑ data - 8            ↑ data
```

- **`rc`**：引用计数，**`_Atomic uint32_t`（M6，跨线程原子）**。`myp_retain` 用
  `atomic_fetch_add_explicit(relaxed)`；`myp_release` 用 `atomic_fetch_sub_explicit(release)`
  返回**旧值**，旧值==1（最后一次释放）先 `atomic_thread_fence(acquire)` 再走销毁桩。
- **`type_id`**：复用现有 `class_type_ids_`（顺序分配的 per-class id，error vtable 已用）。
  用于 `myp_release` 按类型动态派发销毁桩 → **interface 引用也可安全释放**。class 的
  `type_id` ∈ 1..`__myp_max_type_id`；字符串为 `MYP_STR_TYPE_ID=0xFFFFFFFE`，数组/slice
  backing 为 `MYP_ARR_TYPE_ID=0xFFFFFFFF`（见 §0 M8）。
- **tracking 前缀**：双向链节点与对象同一次 `malloc`，释放时 $O(1)$ 摘链；无需单独节点
  分配，也无需扫描全部存活对象。它位于 ARC 头之前，对 codegen 不可见。M6 起该链由进程级
  自旋锁 `myp_alloc_lock` 保护（跨线程安全）。
- **ABI**：`rc/type_id` 始终位于数据指针前 8 字节，所有属性 GEP 和既有生成代码不变。

### 3.2 运行时新 API（`runtime.c`）

```c
void* myp_alloc_object(size_t size, uint32_t type_id); // rc=1 的 class 分配
void  myp_retain(void* obj);                           // rc++（原子，relaxed）
uint32_t myp_release(void* obj);                       // rc--；旧值==1 时析构并 free
void  myp_free_object(void* obj);                      // O(1) 摘 tracking 节点 + free
```

- `myp_release` 旧值==1 → 查**全局释放表** `__myp_release_table[type_id]`（每 TU 数组，
  复用 `class_type_ids_` 索引）→ 调该 class 的**销毁桩** → 桩内级联释放引用槽位 +
  `myp_free_object`。销毁前先 `myp_weak_notify_death(obj)` 置空所有 `@weak` 槽（M7）。
- `myp_retain/myp_release` 对 NULL 空操作。

---

## 4. 所有权约定（codegen 插桩规则）

采用**借用-存储-转移**模型（ObjC/ARC 同款，最易正确实现）：

| 事件 | 操作 | 说明 |
|------|------|------|
| `new T()` | `rc=1` | 新建，唯一强持有 |
| 局部变量声明 `T a = expr` | **转移**（不 retain） | a 接管 expr 的持有 |
| 赋值 `a = b`（a,b 为引用槽） | `retain(b); release(a_old); a=b;` | 先 retain 后 release，**自赋值安全** |
| 函数实参 | **借用**（不 retain） | 被调函数仅在**存储**时 retain |
| 被调函数把参数存 property/全局/数组/事件 | 存储点 `retain` | 新持有者 +1 |
| 函数返回引用值 | **转移**（不 retain） | 调用方接管 |
| 覆盖引用槽 / 槽离开作用域 | `release` 旧值 | 归还 |
| 纯临时值（`new T()` 直接当实参/参与表达式） | 语句结束 `release` | 临时生命周期 = 语句 |
| 存入 `string[]`/`T[]` 数组元素 | 写入点 `retain`；覆盖时 `release` 旧元素 | 数组是引用槽容器 |

### 4.1 自赋值与别名

先 retain 再 release 使 `a = a`、`a = a.next` 等自/别名赋值天然安全。

### 4.2 异常安全（实现要点）

try/catch 栈展开路径必须对**展开经过的作用域引用槽逐个 release**（与现有异常机制集成）；
未捕获异常抛到 main 外 → 现有 `myp_free_all` 兜底，不泄漏不双 free。

### 4.3 协程/线程帧

`@thread`/协程帧持有的引用槽在帧销毁时 release（与协程栈池回收集成）。

---

## 5. 级联释放

### 5.1 每 class 自动生成销毁桩

```myp
class A { property: B b; C c; }
// codegen 生成：
//   void __myp_destroy_A(A* self) {
//       myp_release(self->b);   // class 引用字段
//       myp_release(self->c);
//       myp_free_object(self);
//   }
```

- 引用槽位 = **class 实例类型字段**（含 `interface` fat pointer 的 data 部分）。
- **动态类数组字段**（`T[]`，T 为 class）：引用计数类数组（M-ARC-4）——数组头
  `{ rc, type_id=MYP_ARR_TYPE_ID, elem_size, count, cap }`（24 字节，M8），
  `myp_release` 见 magic 即逐元素释放再释放头；销毁桩/字段存储/作用域退出/临时释放统一经
  `myp_release` 接管。
- **固定栈数组 `[N x T]`**（T 为 class）：kind-3 槽在作用域退出按 count 释放元素
  （`myp_release_fixed_class_array`，不 free 栈缓冲）。
- 非类数组（`int[]`/`double[]`）与 `string`：**M8 起同样计数**（`myp_alloc_slice_backing`/
  `myp_alloc_str`），作用域退出自动释放。
- `slice<class>` 保持 16 字节值 ABI；其 backing 使用引用计数类数组布局，并由 runtime
  建立唯一清理登记。region 退出或线程/进程清理时释放 backing，继而逐元素 release；
  slice 浅拷贝不复制清理登记，避免双重释放。

### 5.2 struct 引用字段：值语义 ARC（M5 起参与计数）

struct 是值类型、可浅拷贝共享同一 class 引用。**M5 规则**：struct 槽为 kind-5 计数槽——
值拷贝时对拷贝内每个 class 引用字段逐个 `retain`，作用域/字段释放时逐字段 `release`，
使各拷贝独立持有引用（无悬垂、无泄漏）。struct 内嵌 struct 递归处理；`@weak` 字段
**暂不支持**（编译期拒绝，见 §0 M7 约束）。

---

## 6. 与 `@region` / 逃逸分析的交互

- **class 实例一律不分配进 region**（进程级 + ARC，rc 决定生死）；M8 起 `string`/`T[]`/
  `slice` backing 也走**进程级引用计数分配**（`myp_alloc_str`/`myp_alloc_slice_backing`），
  不再依赖 region 整块回滚。`@region` 现只覆盖显式 region 语义（arena 计费/调试诊断）。
- `@region` 内 `new T()` 返回/存储 class 引用 → 一律计数管理，rc 转移，无悬垂。
- region 批量释放逻辑（`myp_arena_mark/release`）保持 chunk 回滚兼容；`myp_alloc_object`
  走进程级分配并进进程级追踪链。`@region` 函数若检测到 slice/数组经 return、property/
  global store 或调用参数逃逸，会保守禁用该函数的 region，防止 backing 在返回后悬垂。

---

## 7. 线程语义

- **M6 起：原子 rc（`_Atomic uint32_t`）**，`myp_retain/myp_release` 为无锁原子操作；
  分配/释放追踪链 `myp_alloc_head` 由进程级自旋锁 `myp_alloc_lock` 保护。**class 实例、
  string、数组可安全跨线程传递**（`@thread`/`@parallel` 任务、事件、channel）。
- `@weak` 弱表 `myp_weak_table[64]` 同样持锁保护；`myp_weak_notify_death` 在目标销毁时
  持锁置空全部弱槽并重查 rc（防"读取弱槽升级强引用的同时销毁"竞争）。
- 线程退出清理（`myp_free_all`）只释放本线程 TLS 区域/arena 与未出队对象；进程级全局
  分配列表由 `atexit` 兜底释放（注册顺序：全局列表 → 弱表 → 协程清理）。

---

## 8. 环与弱引用

- **无继承、组件模型为 DAG**（父→子、事件携带临时数据）→ 强环罕见。
- **M7 已实施 `@weak`**：字段注解 `@weak Parent parent;`（仅 class/interface 引用字段，
  struct 字段/`string`/`slice`/数值编译期拒绝）。弱引用不计数、目标销毁自动置空；
  读取 = 弱→强一次性升级（活着返回强引用，已销毁返回 `null`，调用方判空）。
  测试：`tests/weak_cycle`（双向 parent/child 环，销毁后弱读为 null、零泄漏）、
  `tests/weak_non_ref`（非引用类型 @weak 编译期报错）、`stress/cross_thread_arc`
  （弱表并发读/销毁竞争，TSan 验证）。
- 无环场景仍用默认强引用；`@weak` 只用于打破环的一侧。

---

## 9. 错误 / 异常路径

- `throw` 对象：进程级（现有约定），`throw` 时 +1，`catch` 接管（转移），匹配失败释放。
- 异常对象持有引用字段 → 销毁桩同样适用。

---

## 10. GPU 边界

- GPU kernel 路径保持**裸指针 + 位运算**（`kb.*`），不插 rc；kernel 内不允许 class 实例
  （或按值传入），与现有 slice/数组传输语义一致。
- 对象在 host 侧释放时机须在 GPU 拷贝完成后（与 `array_byte_sizes_` 协调，同 P4b 记录）。

---

## 11. 编译器改动清单

| 模块 | 改动 |
|------|------|
| `runtime.c` | `myp_alloc_object`/`myp_retain`/`myp_release`/`myp_free_object` + 释放表；`myp_free` 安全化 |
| `codegen` | 对象布局 +8 头；分配走新 allocator；赋值/参数/返回/存储/临时/数组元素 6 类插桩；生成 per-class 销毁桩 + `__myp_release_table`；异常/协程展开 release |
| `sema` | 计算每 class 的"引用槽位"（class 实例字段，透过 struct 递归但 v1 不释放 struct 槽）；`type_id` 全量覆盖 |
| `stdlib` | （可选）`Memory.freeObject` 别名；文档 |
| `tests` | `tests/arc/`：生命周期、级联、自赋值、别名、异常路径、`@region` 交互、线程本地、长跑累积回归 |

---

## 12. 调试与诊断

- **strict 头校验（M9）**：`myp_release` 下溢（rc==0）、重复释放、损坏头（非法 `type_id`）
  立即 `abort` 并打印对象指针/类型名；`Memory.setStrictChecks(1)` 或 ASAN 构建默认开。
- **存活诊断（M9）**：`Memory.liveObjectCount/liveStringCount/liveArrayCount/liveTotalCount/
  liveObjectCountByType`（按 `Rtti.typeId`）——泄漏/峰值回归回归的可观测面；arena/region
  reserved/used 字节、协程槽/栈池/retired 统计。
- **分配失败注入（M9）**：`Memory.failAllocEnable(n)` 或环境变量 `MYP_FAIL_ALLOC=n`——
  第 n 次分配 abort，确定性 OOM 路径测试。
- 调试构建 / `--trace`：`myp_release` 下溢 assert；`myp_free_object` 后内存填 `0xDEAD`；
  可选"释放后访问"poison 检测。
- 可选编译期警告：检测可疑强环构造（静态难全判，仅提示；M7 起有 `@weak` 可循）。

---

## 13. 迁移与兼容性

- **additive、新语法仅 `@weak` 注解**：现有程序零改动，编译后自动获得计数回收
  （class/string/数组/struct 字段）。
- 行为差异：原先"活到进程退出"的对象现在可能**提前被回收** → 依赖"对象永不释放"的代码
  （如注册表缓存长存）需确认持有引用，否则会释放——这是**唯一需要回归**的点（全库测试覆盖）。
- 全回归：-O0 / -O2 / ASAN / TSan / fuzz 全绿为准。

---

## 14. 决策点（M5–M9 均已定案）

- **D-A1**：对象头放对象**前部**（统一 +8 偏移）vs 后部/独立表（前部最简，**采纳**）。
- **D-A2**：是否扩展计数到 `string`/`T[]` → **M8 已扩展**（实测字符串/数组累积成瓶颈）。
- **D-A3**：struct 引用字段**不释放** vs 所有权规则 → **M5 值语义 ARC 已定案**（逐字段
  拷贝 retain / 释放 release）。
- **D-A4**：跨线程支持 v1 还是 v2 → **M6 已原子化**（`_Atomic rc` + 全局自旋锁分配链），
  跨线程开箱即用。
- **D-A5**：`weak` 引用 v1 还是 v2 → **M7 已实施**（`@weak` 注解 + 运行时弱表 + 自动置空）。

---

## 15. 里程碑建议

- **M-ARC-1**：runtime API + 对象头 + 分配路径切换 + `tests/arc` 生命周期/级联/自赋值 —— 最小可用。**✅ 已实施（2026-08-06）**
  - 实际落点：对象头在数据指针前 8 字节（data = base+8，字段 GEP/this/vtable 零改动）；
    `__myp_release_table` 为 ExternalLinkage 每程序一表；作用域退出释放 + retain-at-return +
    赋值/属性/静态/映射全局插桩；类结构体两遍构建（修自引用 i32 布局 bug）。
- **M-ARC-2**：全插桩（数组元素 + 临时 + 线程帧）。**✅ 已实施（2026-08-06）**
  - `T[]` 数组元素 retain/release（局部数组/this.arr/obj.arr；slice 同）；语句末临时释放
    （`new` 作实参/丢弃不累积，强槽 store 消费）；`return new T()` 转移（跳过 retain-at-return，
    修 M-ARC-1 fresh-return 泄漏）；函数 epilogue release（修 return 结尾局部泄漏）；
    `@thread`/`@threadpool` 实例在 `myp_thread_destroy` 释放 startup_arg。
  - 修复：lambda 闭包临时消费入胖指针；@thread 实例临时消费；emitFunctionReturn 顺序
    （retain 先于 release；main 的 release 先于 `myp_free_all`）。`tests/arc_m2`。
- **M-ARC-3**：与 `@region`/逃逸分析简化整合 + 全库回归 + 文档定稿。**✅ 已实施（2026-08-06）**
  - **闭包释放**：函数值局部注册为 ARC 槽（fat pointer index 0 = 闭包），作用域退出释放；
    `LambdaExpr` 视为 fresh（闭包是新分配的 class 实例）；别名赋值 retain 闭包；捕获的
    class 引用在 `generateLambda` **retain**（闭包拥有自己的引用，销毁桩级联释放平衡）。
    `tests/arc_fn`。
  - **异常/throw-catch 展开释放**：setjmp/longjmp 直接跳到 handler，跳过中间作用域——
    泄漏安全（未捕获走 `myp_free_all` 兜底，不泄漏不双 free）。
  - **协程帧引用释放**：✅ **已实施**——`@coro` 体内每个局部 ARC 槽在 store 时把**对象指针**
    镜像进协程帧登记表（`__myp_coro_frame_set`，slot_id 作键），正常释放经 `releaseArcSlot`
    触发 `__myp_coro_frame_clear` 移除；`Coro.destroy` 与未捕获异常（trampoline）时
    `__myp_coro_release_frame` 释放帧表内仍存活对象。**追踪对象指针而非栈地址**。
    `tests/coro_frame_arc`。
- **M-ARC-4**：`slice<class>` backing 级联释放 + `@region` 逃逸保护。**✅ 已实施（2026-08-06）**
- **M-ARC-5（M5）**：struct 引用字段值语义 ARC（kind-5 槽）。**✅ 已实施（2026-08-13）**
- **M-ARC-6（M6）**：跨线程原子 rc + 全局自旋锁分配链。**✅ 已实施（2026-08-13，`0a22eab`）**
- **M-ARC-7（M7）**：`@weak` 弱引用（注解 + 弱表 + 自动置空）。**✅ 已实施（2026-08-13，`9c156d3`）**
- **M-ARC-8（M8）**：`string`/`T[]`/`slice` 引用计数 + in-place 字符串拼接。**✅ 已实施（2026-08-13，`46da555`）**
- **M-ARC-9（M9）**：内存诊断 + 失败注入 + strict 校验。**✅ 已实施（2026-08-13，`1966900`）**
