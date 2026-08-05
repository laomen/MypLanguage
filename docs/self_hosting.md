# MYP 自举路线设计（Self-Hosting）

> 状态：**T1 已实施（2026-08-05）**——`tools/myp.myp` 包管理器；T2 待实施
> 关联：语言规格 v1.0（`docs/grammar.md`）、`docs/pkg_manager.md`（Tier 1 详细设计）、
> `docs/design.md` §11"自举"、`docs/next_improvements.md` §六-2。
> 本文档规划**用 MYP 语言逐步重写自身工具链**：从工具到编译器本体，每层都以
> 可机器验证的输出对拍为验收标准，同时作为语言稳定性最硬核的证明。

---

## 1. 背景与动机

### 1.1 现状

MYP 工具链全部为 C++（`src/`）：

| 工具 | 源 | 规模 | 侧重子系统 |
|------|----|------|-----------|
| `mypc` | `src/`（lexer/parser/sema/codegen） | 大 | 编译器本体 |
| `myp_fmt` | `src/fmt/fmt.cpp` + `main.cpp` | 389 + 89 行 | **lexer + 字符串** |
| `myp_lsp` | `src/lsp/` | 大 | JSON-RPC + 分析引擎 |
| `myp_viz` | `src/myp_viz.cpp` | 中 | AST dump / 图输出 |
| `myp_debug` | `src/dap/` | 中 | gdb 桥 + DAP |

另有一个 Python 版包管理器 `myp`（268 行，见 `docs/pkg_manager.md`）。

### 1.2 动机

1. **自举**：语言自举自己的工具链（`docs/design.md` §11 第一步：先工具后编译器）。
2. **稳定性证明**：每个自举工具都是真实程序，覆盖不同子系统；验收有硬指标（输出对拍），
   比任何单测更能暴露语言短板。
3. **消除依赖**：逐步去掉 Python 运行时、最终去掉 C++ 编译器。

---

## 2. 自举阶梯

| 层 | 工具 | 侧重子系统 | 规模 | 工作量 | 状态 |
|----|------|-----------|------|--------|------|
| **T1** | 包管理器 | 文件/进程/JSON/CLI | ~300 行 | ~1-2 天 | 设计已定（`pkg_manager.md`） |
| **T2** | `myp fmt` | **lexer/字符串/字符** | ~400-500 行 | ~2-4 天 | 本文档 §4 |
| **T3** | `myp_viz` | AST dump/文本图 | ~200-300 行 | ~1-2 天 | 待定 |
| **T4** | `myp_lsp` | JSON-RPC + 符号/诊断 | 大 | 远期 | 待定 |
| **T5** | `mypc` 本体 | 编译全链路 | 最大 | 远期 | roadmap |

> 原则：**每层独立可交付、可验证**；低层不依赖高层。T1 先落地积累经验，T2 紧随。

---

## 3. 公共前提（跨层 stdlib 补充）

各层所需的 stdlib 能力汇总（现有缺失项）：

| 能力 | 用途 | 补法 | 涉及层 |
|------|------|------|--------|
| `myp_fs_mkdir_p(path)` | 递归建目录 | runtime.c FFI + `fs.myp` 封装 | T1 |
| `myp_fs_remove_recursive(path)` | 递归删除 | runtime.c FFI + `fs.myp` 封装 | T1 |
| `__myp_ord(string) → int` | 字符→ASCII 码（lexer 基础） | codegen intrinsic + runtime | T2 |
| 字符分类 isDigit/isAlpha/isSpace | 词法判定 | `__myp_ord` + int 比较**纯 MYP 派生** | T2 |
| （可选）json 序列化 | 写 index.json / lockfile | 手拼字符串或补 FFI | T1 v2 |

> `__myp_chr` 已存在（ASCII→字符）；`StringBuilder`/`substring`/`split`/`repeat` 已存在。

---

## 4. T2：`myp fmt`（格式化器）

### 4.1 现状（C++ 版）

- `src/fmt/fmt.cpp`（389 行）：基于 `mylang/Lexer` **tokenize 后按规则重排**
  （缩进、空格、注释提取、字面量保留）。
- 入口：`mypc fmt` 子命令 + 独立 `myp_fmt` 二进制。

### 4.2 MYP 版设计

```
myp_fmt.myp（或 myp_pkg/fmt.myp）
  lexer.myp      — mini 词法器：逐字符扫描 → Token 列表（关键字/标识符/数字/字符串/
                   char/注释/标点）
  fmt.myp        — Token 重排：缩进层级、行内空格、注释位置、保留空行
  main.myp       — CLI（args.myp）+ 读文件（io.myp）+ 写输出（StringBuilder）
```

**mini 词法器**（MYP 实现）：
- 逐字符扫描：`substring(s, i, i+1)` 取单字符 + `__myp_ord` 转码
- 分类：`isDigit/isAlpha/isSpace` 用 `ord` + 整数比较纯 MYP 派生
- 字符串/char 字面量：处理转义（`\n`/`\t`/`\"`/`\\`）
- 注释：`//` 行注释 + `/* */` 块注释提取

### 4.3 需补的 stdlib

**仅 1 个**：`__myp_ord(string) → int`（intrinsic）。其余 `StringBuilder`/`substring`/
`split`/`repeat`/`args`/`io` 全部现有。

### 4.4 验收标准（硬指标）

对全部 `stdlib/*.myp`（31 文件）+ `examples/` + `tests/*/test.myp`：

1. **字节级对拍**：MYP 版输出 == C++ 版 `myp_fmt` 输出（`cmp` 逐字节）。
2. **幂等**：`fmt(fmt(x)) == fmt(x)`。
3. **语法保持**：格式化后 `mypc` 编译结果一致（sema 通过数相同）。

### 4.5 风险与对策

| 风险 | 对策 |
|------|------|
| mini 词法器与 C++ Lexer 行为有细微差异 | 以对拍失败用例为驱动，逐项对齐 token 规则 |
| 字符串转义/注释边界处理 | 专项测试覆盖（转义/嵌套注释/行尾反斜杠） |
| 性能（MYP 字符串不可变，逐字符 substring 有开销） | 词法器按需 `substring` 缓冲；先正确后优化 |

---

## 5. T3 展望：`myp_viz`

- 现状：`src/myp_viz.cpp`——AST 解析后输出树/图（文本或 Graphviz）。
- MYP 版：解析（复用 T2 lexer）+ AST 打印（StringBuilder）→ 文本树输出。
- 验收：对拍 C++ 版 AST 文本输出。
- stdlib 缺口：无新增（复用 T2 基础设施）。

---

## 6. 里程碑

| 阶段 | 交付 | 验收 |
|------|------|------|
| **M1** | T1 包管理器（MYP 重写） | 与 Python 版逐命令对拍 |
| **M2** | `__myp_ord` + T2 lexer + fmt 骨架 | mini lexer 能 tokenize 全 stdlib |
| **M3** | T2 fmt 全量格式规则 | 全 stdlib 字节级对拍 + 幂等 |
| **M4**（可选） | T3 viz | AST 文本对拍 |
| **远期** | T4 LSP、T5 mypc | — |

---

## 7. 自举的终极意义

- T1-T3 完成后，MYP 可**自举自己的工具链**（包管理 + 格式化 + 可视化），
  工具链的日常使用即持续的语言稳定性回归。
- T5（mypc 本体）是 roadmap 终极目标；T1-T3 为其积累：
  - lexer（T2）→ 编译器前端词法
  - 文本/字符串/字符处理 → 编译器核心
  - 进程/文件编排（T1）→ 编译驱动

---

## 8. 待评审决策点

- **D1**：T2 `myp_fmt` 是否与 T1 并行推进（T1 先落地 vs 同步）？
- **D2**：T2 采用单文件 `myp_fmt.myp` vs 模块化（lexer.myp / fmt.myp / main.myp）？
- **D3**：`__myp_ord` 命名与签名（`__myp_ord(string)` 返回 int，空串/多字符如何处理）？
- **D4**：T2 验收的对拍文件集范围（仅 stdlib vs 含 examples/tests）？
- **D5**：T3 `myp_viz` 是否纳入本轮里程碑（M4）？
