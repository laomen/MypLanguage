# MYP 切片类型设计（Slice）

> 状态：**设计提案 v0.1**（待评审后实施）
> 关联：语言规格 v1.0（`docs/grammar.md`）、变更策略（`docs/CHANGELOG.md`）、
> 算子系统（`docs/operators.md` P4：元素级提升 + 集合二元）
> 本文档提出带运行时长度的集合类型——**切片（slice）**，并配套两级 arena +
> `@region` 注解 + 逃逸分析的内存模型，作为 P4 的地基。实施前请先评审。

---

## 1. 背景与动机

P4（`docs/operators.md` §8）需要集合类型具备**运行时长度**（§7 难点①）。
当前 `double[]` 是 arena 裸分配，**无长度元数据**，`A.size()` 无法实现。
本文档提出**切片（slice）类型**作为解决方案。

### 1.1 现状核实

`src/codegen/codegen.cpp` `generateNewArrayExpr`（约 5088 行）：

```cpp
auto* ptr = builder_.CreateCall(alloc_fn, {byte_size});   // myp_alloc(n * elem_size)
builder_.CreateMemSet(ptr, 0, byte_size, llvm::Align(8)); // 零填充
return ptr;                                               // 返回裸 double*
```

即 `double[]` = 裸 `T*`；长度仅存在于编译期局部表（`array_byte_sizes_`，按变量名），
运行期不可见。这就是 §7 难点① 的根源。

### 1.2 候选方案对比

| 方案 | 做法 | 破坏性 | 实现量 | 对现有 `T[]` 影响 |
|---|---|---|---|---|
| A 长度头 | 数据前放 `int64 len` 头，返回 data 指针 | 非破坏 | 小 | 零（指针兼容） |
| **B 切片（推荐）** | `{ T* data; int64 len }` fat pointer | 非破坏 | 中 | 零（新增类型） |
| C 容器 | stdlib `Array<T>` 泛型容器 | 非破坏 | 中 | 零 |

**长度头（A）被否原因**：长度读取需 `((int64*)p)[-1]` 隐式 hack，类型系统不可见；
外部 FFI 传入的裸指针无头，无法区分。切片（B）把长度作为类型的显式一部分，更干净。

---

## 2. 推荐：切片类型 `slice<T>`

### 2.1 类型表示

```
slice<T> = { T* data; int64 len }
```

Rust `&[T]` 同款 fat pointer。**MYP 使用 arena 内存（`myp_free_all` 统一释放），
无借用/所有权概念，因此 slice 就是纯值拷贝的胖指针，没有 Rust 的 lifetime 复杂度。**

### 2.2 语法与操作

| 操作 | 语法 | 实现 |
|---|---|---|
| 创建 | `slice<double> s = new slice<double>(n);` | 分配 `n*8` + 构造 `{ptr, n}` |
| 下标 | `s[i]` | 解包 `data` → GEP（可选边界检查） |
| **长度** | `s.size()` / `s.length` | `extractvalue` 读 `len`（**纯 GEP，零 runtime**） |
| 裸指针 | `s.data()` | 取 `data` 字段（FFI/GPU 用） |
| 传参/返回 | 值传递 | 2 字段 struct，LLVM 自动处理 |

**对比长度头的最大优势**：`A.size()` 是字段读取，类型系统全程可见，
不需要隐式内存 hack。

### 2.3 与现有 `T[]` 共存（非破坏）

| | 语法 | 语义 |
|---|---|---|
| 现有 | `double[]` | 裸 `double*`（无长度），**保持不动** |
| 新增 | `slice<double>` | `{data, len}` fat pointer |

`T[]` 的所有现有代码（下标/传参/GPU/FFI）一行不改。

### 2.4 P4 集合二元（目标用法）

```myp
@op("+") slice<double> add(slice<double> A, slice<double> B) {
    int n = A.size();                        // 天然运行时长
    slice<double> C = new slice<double>(n);
    int i = 0; while (i < n) { C[i] = A[i] + B[i]; i = i + 1; }
    return C;
}
```

---

## 3. 生命周期与内存模型

### 3.1 现状：双层内存模型

| 变量类别 | 分配位置 | 生命周期 | 释放 |
|---|---|---|---|
| 标量局部变量 | 栈（LLVM `alloca`） | 函数作用域 | 栈帧自动 |
| struct 值 | 栈（`alloca`） | 函数作用域 | 栈帧自动 |
| 数组 `new T[n]` | **堆**（`myp_alloc` = `malloc`） | 进程/线程结束 | `myp_free_all()` |
| class 实例 `new C()` | **堆**（`myp_alloc`） | 进程/线程结束 | `myp_free_all()` |
| 字符串 | 堆（`myp_alloc`） | 进程/线程结束 | `myp_free_all()` |

核心机制（`src/runtime/runtime.c`）：`myp_alloc` = `malloc` + 压入**线程本地链表**
（pthread_key TLS）；`myp_free_all()` 在 main 退出（或线程退出时 pthread_key 析构）遍历链表整块释放。
**无 GC、无手动 delete、无 RAII**；arena 是**线程本地**的（`@thread` 各有其 arena）。

### 3.2 为什么 slice 天然无悬垂

slice 的 `data` 指向**堆（arena）数组**，class 实例也在堆上 → 两者同生命周期
（进程/线程结束才释放）。因此 **class 的 property 持有 slice 字段完全安全**；
slice 不会指向栈数据（MYP 无栈数组）。

**这正是 MYP 不需要 Rust lifetime 的根本原因**：Rust 栈对象可被借用、需显式生命周期；
MYP 引用型数据全在 arena 堆上，统一存活，天然无悬垂。

### 3.3 内存爆炸问题 → 两级 arena

**风险**：进程级释放意味着长期运行程序（游戏主循环/服务器/GUI）若每帧/每请求
`new` 对象，arena 只增不减 → 内存单调增长。

**解法：两级 arena（非破坏性新增 runtime API）**

```c
int  myp_arena_mark();            // 记录当前分配水位
void myp_arena_release(int mark); // 释放 mark 之后的所有分配（同线程）
```

- **默认 `new`** → 进程级 arena（现状，跨事件存活，兼容）
- **`@region` 内 `new`** → 当前 region（区域结束自动回收）

链表为"头插"，`mark` 记链表头、`release` 从标记处一路 free——实现约 20 行 C。
**slice 与管道链式让中间对象激增，region 自动回收是 P4 的必需品**（而非可选项）。

### 3.4 `@region` 注解（简洁无感）

```myp
class Pipeline {
    action:
        @region void processFrame() {          // 一帧 = 一个事务
            slice<double> tmp = new slice<double>(n);
            var r = tmp |> ScaleOp |> OffsetOp; // 中间产物全是事件临时
            ... // 用 r
        }   // ← 返回时自动释放本 action 内 new 的所有对象（含中间 slice）
}
```

- **一个注解 = 无感**：只在"每帧/每请求/批量处理"的 action 上加 `@region`
- **非破坏性**：默认行为不变，只有标注的 action 启用事件级回收
- **实现**：codegen 在 `@region` action 入口插 `mark`，所有出口（含 return 路径）插 `release`

**`@region` 统一语义**：
- **适用对象**：任何有调用作用域的函数——class 的 **action**、**function**、以及**顶层函数**
  （如外部 `@op` 函数）；语义一致 = 该函数调用作用域为 region（动态 extent）
- **单一语法**：只保留 `@region` 注解一种写法，**不提供** `region { }` 块级语法
  （避免两套写法歧义）
- **嵌套 region**：`@region` 函数内调用另一 `@region` 函数 → 嵌套 region，
  **栈式 LIFO**：内层先释放、外层后释放，互不干扰
- **不适用**：struct 方法（值类型、无堆对象），`@region` 无意义

### 3.5 逃逸分析：统一引用逃逸模型

**问题**：`@region` 内**返回任何携带引用的值**（slice / `T[]` 裸数组 / struct 的引用字段），
region 结束时其指向的数据会被误释放吗？

**不会。** 编译期逃逸判定按"**引用槽位 + 传播闭包**"进行，两级分配分流。

#### 引用槽位（reference slot）

每个类型在**类型层面**定义"它携带哪些引用"：

| 类型 | 引用槽位 |
|---|---|
| `T[]`（裸指针） | 1 个：`data` |
| `slice<T>` | 1 个：`data` |
| `string` | 1 个：`data` |
| struct | **递归**：各字段引用槽位的并集 |
| class 实例 | 1 个（自身）+ 深层实例字段 |

#### 逃逸传播闭包

```
规则 1（出口）：return V / 存 property / 传参 → V 逃逸
规则 2（传递）：V 逃逸 ⇒ V 的所有引用槽位指向的数据逃逸
规则 3（回归）：数据逃逸 ⇒ 创建它的 new 提升为进程级
```

#### 三类返回的处理

① **`return new T[]`**（new 在 return 位置）→ 该 `new` 直接标记逃逸 → 进程级

② **`return structVal`**（struct 值拷贝、引用字段浅拷贝）→ struct 逃逸
   ⇒ 其引用字段（如 slice / 数组）指向的数据逃逸 → 对应 `new` 提升进程级

③ **`return class 实例`** → class **默认进程级**（引用类型、常跨事件存活），
   region 收益聚焦临时数组 / slice

| 对象去向 | 逃逸？ | 分配 |
|---|---|---|
| 仅局部变量引用、本地计算 | ❌ 未逃逸 | **region**（自动回收） |
| `return` 出去（slice/数组/struct 引用字段） | ✅ 逃逸（含传递） | **进程级** |
| 存入 property / 全局 | ✅ 逃逸 | 进程级 |
| 作为参数传给其他函数 | ✅ 保守视为逃逸 | 进程级 |

#### 实现（codegen，单 action 内分析）

1. 收集 `@region` action 内所有 `new` 的**持有者图**（变量 → 其引用槽位指向的 `new`）
2. 从逃逸出口（return / 存属性 / 传参）出发，沿引用槽位做传播闭包，标记所有可达 `new`
3. 标记的 `new` → 进程级 `myp_alloc`；其余 → `myp_region_alloc`
4. action 所有出口（含 return 路径）统一 `myp_arena_release`

**运行时零额外逻辑**：判定在编译期完成，release 是整块释放。
**保守兜底**（v1.0）：跨函数 / 别名复杂分析不清 → 该 `new` 提升进程级，100% 安全。
P4 链式场景（中间 slice 全为本地、不返回、不传参）仍是主要受益者。
可选编译期诊断：`@region` 返回 region 内对象 → 提示"已自动提升为持久"。

### 3.6 边界与线程语义

#### ① 字符串（string）逃逸

`string` = `char*`（裸指针），是**引用类型**。`@region` 内 `return` 字符串
（如 `"prefix_" + id` 经 `myp_strcat`）→ 逃逸 → 进程级（§3.5 引用槽位表已含 `string`）。

#### ② region 动态 extent

```myp
@region void processFrame() {
    helper();          // 普通函数，内部 new 归当前 region
}
void helper() {
    slice<double> t = new slice<double>(4);   // ← 属于当前 region（动态 extent）
}
```

**region 是动态作用域**：`@region` action 调用栈内（含普通函数）所有 `new` 都进
当前 region。`myp_region_alloc` 读取"当前 region"TLS（线程本地），天然实现动态 extent。
普通函数里的临时对象也因此可回收。

#### ③ 事件链跨 region：传递 = 逃逸

```myp
@region void onData() {
    slice<double> s = new slice<double>(n);
    event.ready(s);          // fire event 携带 s 给另一个 action
}   // ← s 被事件带出 → 视为逃逸 → 进程级，接收方无悬垂
```

**规则：fire event 携带的对象 = 逃逸**（如同传参），分配进程级。
否则事件接收方拿到悬垂 slice。这是 MYP 特有的逃逸出口（事件驱动核心场景）。

#### ④ 线程语义

- `myp_arena_mark/release` **线程本地**（TLS）；`@region` 在各线程独立成区域
- 跨线程 slice：A 线程 region 内对象传给 B 线程 → A 的 region 释放即悬垂 →
  **视为逃逸（进程级）**或约定"不跨线程传 region 内对象"

#### ⑤ GPU 边界（实现记录）

region 内 slice 用于 GPU 传输：`array_byte_sizes_` 是编译期表，与 region 动态回收
需协调（数据须在 region 释放前完成拷贝）。P4b 实现时再定。

---

## 4. 设计取舍（待评审拍板）

### 4.1 `new double[n]` 的返回类型

- **选项 1（推荐，非破坏）**：`new double[n]` 保持返回裸指针；
  slice 用显式 `new slice<double>(n)` 创建 → 新旧完全隔离，零破坏
- **选项 2（Go 风格，破坏性）**：`new double[n]` 直接返回 slice；
  `double[] A = new double[n]` 需 slice→裸指针隐式降级 → 更统一，
  但触碰所有现有赋值点，需升语言规格 v2.0

### 4.2 语法命名

- **`slice<T>`（推荐）**：与现有泛型 `ArrayList<T>` / `HashMap<K,V>` 风格一致
- `&[T]`（Rust）：MYP 无借用概念，引入会误导
- `[]T`（Go）：与 MYP 现有后置 `T[]` 语法冲突

### 4.3 下标边界检查

- 安全（bounds check → 越界报错）vs 性能（直接 GEP）
- 建议：默认不做（与现有 `T[]` 一致），作为后续可选开关

---

## 5. 实现面评估

| 层 | 改动 |
|---|---|
| **Sema** | 类型系统加 `Slice` 类型（`slice<T>` 走已有泛型解析），`var` 推断支持 |
| **Codegen** | LLVM 类型 `{ T*, i64 }`；`new slice<T>(n)`；下标解包 `data`；`.size()`/`.data()` 字段读取 |
| **GPU** | kernel 参数解包 `data` + `len`（与现有 `array_elem_types_` 机制并行） |
| **Runtime** | 几乎零（slice 是纯编译期胖指针，不需要 `myp_arr_len` 这类 helper） |
| **现有 `T[]`** | **完全不碰** |

**风险**：改动集中在新增分支（slice 类型走新路径，`T[]` 走旧路径），
不修改现有代码路径，符合 v1.0 非破坏性约束。

---

## 6. 待决问题（评审清单）

1. **`new double[n]` 返回裸指针还是 slice？**（§4.1 选项 1 vs 2）
2. **语法命名**：`slice<T>` 还是其他？（§4.2）
3. **下标是否做边界检查？**（§4.3）
4. **是否提供 `T[]` → `slice<T>` 转换？**（裸数组无长度，转换语义需谨慎）
5. **`@region` 适用对象**：action / function / 顶层函数均可注解；**单一语法**
   （不用 `region { }` 块）；嵌套栈式 LIFO？（§3.4 统一语义，推荐）
6. **逃逸分析保守策略**："传参即逃逸"是否接受？（§3.5，保证安全但区域回收覆盖有限）
7. **region 动态 extent**：普通函数内 `new` 归当前 region？（§3.6 ②，推荐动态）
8. **事件传递 = 逃逸**：fire event 携带的对象提升进程级？（§3.6 ③，推荐）
9. **与 P4 的衔接**：slice 落地后，`docs/operators.md` §5 示例改为用 `A.size()`，
   消除"硬编码 n"（§7 难点①）

---

## 7. 实施路线（评审通过后）

| 阶段 | 内容 |
|---|---|
| P4a | slice 类型：Sema + `new slice<T>(n)` + `s[i]` + `s.size()` + `s.data()` |
| P4b | 两级 arena：`myp_arena_mark/release` + `@region` 注解 + 逃逸分析 |
| P4c | `@op("+")` 集合二元示例（基于 slice），更新 `docs/operators.md` |
| P4d | 回归测试 `tests/slice/` + 正常/ASAN 套件 + grammar.md 增量 |
