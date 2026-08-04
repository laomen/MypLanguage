# MYP 切片类型设计（Slice）

> 状态：**设计提案 v0.1**（待评审后实施）
> 关联：语言规格 v1.0（`docs/grammar.md`）、变更策略（`docs/CHANGELOG.md`）、
> 算子系统（`docs/operators.md` P4：元素级提升 + 集合二元）
> 本文档提出带运行时长度的集合类型——**切片（slice）**，作为 P4 的地基。实施前请先评审。

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

## 3. 设计取舍（待评审拍板）

### 3.1 `new double[n]` 的返回类型

- **选项 1（推荐，非破坏）**：`new double[n]` 保持返回裸指针；
  slice 用显式 `new slice<double>(n)` 创建 → 新旧完全隔离，零破坏
- **选项 2（Go 风格，破坏性）**：`new double[n]` 直接返回 slice；
  `double[] A = new double[n]` 需 slice→裸指针隐式降级 → 更统一，
  但触碰所有现有赋值点，需升语言规格 v2.0

### 3.2 语法命名

- **`slice<T>`（推荐）**：与现有泛型 `ArrayList<T>` / `HashMap<K,V>` 风格一致
- `&[T]`（Rust）：MYP 无借用概念，引入会误导
- `[]T`（Go）：与 MYP 现有后置 `T[]` 语法冲突

### 3.3 下标边界检查

- 安全（bounds check → 越界报错）vs 性能（直接 GEP）
- 建议：默认不做（与现有 `T[]` 一致），作为后续可选开关

---

## 4. 实现面评估

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

## 5. 待决问题（评审清单）

1. **`new double[n]` 返回裸指针还是 slice？**（§3.1 选项 1 vs 2）
2. **语法命名**：`slice<T>` 还是其他？（§3.2）
3. **下标是否做边界检查？**（§3.3）
4. **是否提供 `T[]` → `slice<T>` 转换？**（裸数组无长度，转换语义需谨慎）
5. **与 P4 的衔接**：slice 落地后，`docs/operators.md` §5 示例改为用 `A.size()`，
   消除"硬编码 n"（§7 难点①）

---

## 6. 实施路线（评审通过后）

| 阶段 | 内容 |
|---|---|
| P4a | slice 类型：Sema + `new slice<T>(n)` + `s[i]` + `s.size()` + `s.data()` |
| P4b | `@op("+")` 集合二元示例（基于 slice），更新 `docs/operators.md` |
| P4c | 回归测试 `tests/slice/` + 正常/ASAN 套件 + grammar.md 增量 |
