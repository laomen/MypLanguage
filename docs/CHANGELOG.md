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

### v2.4.2（当前）
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
