# MYP 引用计数内存管理设计（ARC on class 实例）

> 状态：**M-ARC-1 已实施（2026-08-06）**；M-ARC-2（异常/数组/线程帧释放）待办
> 关联：`docs/slice.md` §3（两级 arena 内存模型）、`docs/next_improvements.md` §五-1、
> `docs/grammar.md`（规格 v1.0 冻结——本设计 **additive**，无新语法）
> 决策背景：析构器已讨论排除（`docs/constructor.md`）；完整 GC 过重；手动 free 会打破
> MYP"天然无悬垂"根基。结论：**class 实例走引用计数**（自动、确定性、安全），
> `string`/`T[]`/`slice` 保持 arena + `@region`。

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

**关键契合**：MYP 组合而非继承（无 vtable 于具体类）、分配有唯一入口、对象线程本地
（TLS arena 惯例）→ ARC 可做到**非原子、低开销**，且无继承链使"按静态类型销毁"多数可行。

---

## 2. 设计边界（什么计数、什么不计数）

| 类型 | 是否 ARC | 理由 |
|------|----------|------|
| **class 实例** | ✅ **计数** | 中/长寿命主力（组件/状态/缓存）；分配有唯一入口、身份稳定 |
| `string` / `T[]` / `slice` | ❌ 不计数 | 临时值（`@region` 已覆盖）或值拷贝；避免热路径 rc 开销 |
| `struct` | ❌ 不计数 | 值类型、无堆身份；其引用字段**视为借用**（见 §5.2） |
| `@static class` 属性 | ❌ 不计数 | 进程级生命周期，全局单例，不参与回收 |
| `Memory` 裸缓冲 | ❌ 不计数 | 显式 FFI malloc/free，照旧 |

> **v1 边界**：只对 class 实例计数。若实测发现对象持有的 string/数组累积成为瓶颈，
> 再扩展计数（它们引用槽位单一，机制可复用）。此为文档化的已知限制。

---

## 3. 对象布局与运行时

### 3.1 对象头（新增，8 字节）

```
┌──────────┬──────────┬──────────────────┐
│ rc:u32   │ type_id  │ 对象数据（字段）  │
└──────────┴──────────┴──────────────────┘
```

- **`rc`**：引用计数（非原子，见 §7）。
- **`type_id`**：复用现有 `class_type_ids_`（顺序分配的 per-class id，error vtable 已用）。
  用于 `myp_release` 按类型动态派发销毁桩 → **interface 引用也可安全释放**。
- **布局变更**：class 实例数据指针前移 8 字节；codegen 所有属性 GEP 偏移 +8。
  一次性改动，集中在对象布局/分配代码。

### 3.2 运行时新 API（`runtime.c`）

```c
void* myp_alloc_object(size_t size, uint32_t type_id); // rc=1 的 class 分配
void  myp_retain(void* obj);                           // rc++
uint32_t myp_release(void* obj);                       // rc--；为 0 时调释放表并 free
void  myp_free_object(void* obj);                      // 摘追踪链节点 + free（防退出双 free）
```

- `myp_release` 为 0 → 查**全局释放表** `__myp_release_table[type_id]`（每 TU 数组，
  复用 `class_type_ids_` 索引）→ 调该 class 的**销毁桩** → 桩内级联释放引用槽位 + `myp_free_object`。
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
- 数组字段（`T[]`/`slice`/`string`）：**不释放**（不计数，arena 管理）——v1 保守策略。

### 5.2 struct 引用字段：不参与计数（保守，宁可泄漏不悬垂）

struct 是值类型、可浅拷贝共享同一 class 引用；若计数释放，一份拷贝释放会令其余拷贝悬垂。
**v1 规则：销毁桩不释放 struct 字段内的 class 引用** → 最坏是泄漏（安全），绝不悬垂。
后续可引入 struct 所有权规则（additive）再收紧。

---

## 6. 与 `@region` / 逃逸分析的交互

- **class 实例一律不分配进 region**（进程级 + ARC，rc 决定生死）——这**简化**现有逃逸分析：
  class 不再参与"逃逸→进程级"判定，region 只管 `string`/`T[]`/`slice`。
- `@region` 内 `new T()` 返回/存储 class 引用 → 一律计数管理，rc 转移，无悬垂。
- region 批量释放逻辑（`myp_arena_mark/release`）**不变**；`myp_alloc_object` 走进程级分配
  并进进程级追踪链（与 region 链互斥，TLS 分离）。

---

## 7. 线程语义

- **v1：对象线程本地，rc 非原子**（普通 load/add/store）——与现有"arena 为 TLS、对象不跨
  线程传"的文档约定一致，近乎零开销。
- **跨线程传递**（`@parallel` 任务带 class 引用、事件跨线程）：属高级用法。v1 文档约定
  **禁止或要求显式深拷贝**；后续如需支持，再引入**原子 rc 模式**（`myp_retain_atomic`/
  `myp_release_atomic`）或跨线程前"冻结+转移"约定。

---

## 8. 环与弱引用

- **无继承、组件模型为 DAG**（父→子、事件携带临时数据）→ 强环罕见。
- **v1 约定：禁止构造强环**（文档明示；环将导致泄漏而非崩溃——安全但需注意）。
- **后续（additive）**：`weak` 引用（对象头加 weak 计数位 + 置空回写），解除环限制。

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

- 调试构建 / `--trace`：`myp_release` 下溢 assert；`myp_free_object` 后内存填 `0xDEAD`；
  可选"释放后访问"poison 检测。
- 可选编译期警告：检测可疑强环构造（静态难全判，仅提示）。

---

## 13. 迁移与兼容性

- **additive、无新语法**：现有程序零改动，编译后自动获得中寿命对象回收。
- 行为差异：原先"活到进程退出"的对象现在可能**提前被回收** → 依赖"对象永不释放"的代码
  （如注册表缓存长存）需确认持有引用，否则会释放——这是**唯一需要回归**的点（全库测试覆盖）。
- 全回归：-O0 / -O2 / ASAN / TSan / fuzz 全绿为准。

---

## 14. 待评审决策点

- **D-A1**：对象头放对象**前部**（统一 +8 偏移）vs 后部/独立表（前部最简，采纳）。
- **D-A2**：v1 是否扩展计数到 `string`/`T[]`（建议先不，看实测）。
- **D-A3**：struct 引用字段**不释放**（保守）vs 引入所有权规则（建议 v1 保守）。
- **D-A4**：跨线程支持放 v1 还是 v2（建议 v2，v1 非原子）。
- **D-A5**：`weak` 引用放 v1 还是 v2（建议 v2，v1 文档禁止强环）。

---

## 15. 里程碑建议

- **M-ARC-1**：runtime API + 对象头 + 分配路径切换 + `tests/arc` 生命周期/级联/自赋值 —— 最小可用。**✅ 已实施（2026-08-06）**
  - 实际落点：对象头在数据指针前 8 字节（data = base+8，字段 GEP/this/vtable 零改动）；
    `__myp_release_table` 为 ExternalLinkage 每程序一表；作用域退出释放 + retain-at-return +
    赋值/属性/静态/映射全局/slice 元素插桩；类结构体两遍构建（修自引用 i32 布局 bug）。
- **M-ARC-2**：全插桩（赋值/参数/返回/存储/临时/数组元素）+ 异常/协程展开。**待办**
  - 剩余：异常/throw-catch 展开释放、`T[]` 数组元素 retain/release、`@thread`/协程帧
    销毁释放、临时表达式语句末释放、`@region`/逃逸分析简化整合。
- **M-ARC-3**：与 `@region`/逃逸分析简化整合 + 全库回归 + 文档定稿。**待办**
- 与自举路线关系：T4/T5 大量 AST 节点（class 实例）将自动受益；`Option`/空安全（§三-1）
  与 `Result`（§五-3）可与 ARC 并行，互不冲突。
