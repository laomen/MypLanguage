# tools/selfhost —— MYP 全自举编译器项目（非 GPU）

> 状态：**F0 已完成（2026-08-13）**——C++ `mypc --frontend-dump {tokens,ast,sema}` oracle 落地
> （`src/frontend_dump.cpp`）、契约冻结（`format.md`）、MYP 骨架（`tools/selfhost/src/`）、
> CMake `myp_self`、`tests/test_myp_self.sh`（67/67 全绿）。F1（词法器）为下一步。
> 关联：`docs/self_hosting.md` §2 T5、`docs/grammar.md`（语言规格 v1.0，冻结）、
> `docs/design.md` §11、`docs/next_improvements.md` §七。
> 参考实现：`src/lexer/`、`src/parser/`、`src/sema/`、`src/codegen/`（C++，非 GPU 约 2.27 万行）。

## 这是什么

自举路线终极目标：**用 MYP 语言完整重写 `mypc` 编译器本体**——前端（lexer/parser/sema）+
**非 GPU codegen** + CLI 驱动，交付自举编译器 `myp_self`，并完成经典两级自举验证
（stage1 由 C++ 编译、stage2 由自身编译自身）。

## 范围

| 在范围内 | 排除（明确不做） |
|----------|------------------|
| lexer + parser + sema（前端） | **GPU 实现**：`@gpu for` / NVPTX / runtime_gpu / CUDA（5,462 行）——永久保留 C++ 后端；含 `@gpu` 源文件走 CPU 顺序回退语义 |
| **非 GPU codegen**（LLVM IR 发射，约 1.17 万行等价物） | **C 运行时 `runtime.c`**（约 6,000+ 行）——视作生成程序的"libc"，保留 C，自举只重写编译器本体 |
| CLI 驱动（编译 / `run` / `fmt` 子命令、`--stdlib`、链接） | `@macro` / `@eval` 编译期求值（依赖宏引擎，留待后续） |

## 核心设计要点（详见 `design.md`）

- **codegen 策略**：MYP 无法在进程内复用 LLVM C++ API → **发射 LLVM IR 文本（.ll）**，
  再调外部 `llc`（LLVM 后端）+ `gcc` 链接 C runtime。天然可与 `mypc --emit-llvm` 对拍。
- **前端 oracle**：C++ `mypc --frontend-dump {tokens,ast,sema}`（F0 落地）→ MYP 版字节对拍。
- **codegen oracle**：`--emit-llvm` IR 文本对拍 + **产物运行输出对拍**（主验收）。
- **自举验证（H1）**：stage1 `mypc` 编 `myp_self` → stage2 `myp_self` 自编 → stage3 再自编，
  两代产物行为一致即自举成立。

## 文档

| 文档 | 内容 |
|------|------|
| [`design.md`](design.md) | 详细设计：范围、管线、模块、codegen 策略、输出契约、验收、bootstrap |
| [`roadmap.md`](roadmap.md) | 任务拆解：F0–F4（前端）+ G1–G4（后端）+ H1（自举验证） |

## 目录规划（设计，待编码创建）

```
tools/selfhost/
├── README.md            # 本文件
├── design.md            # 详细设计
├── roadmap.md           # 任务拆解
└── src/                 # （规划）MYP 源码——自举编译器
    ├── main.myp         # CLI 驱动（编译/run/fmt/--stdlib/-o；int selfMain()）
    ├── token.myp        # TokenKind 表示 + keywordString + Token
    ├── lexer.myp        # 完整词法器（由 tools/fmt/lexer.myp 扩展）
    ├── ast.myp          # AST 节点类 + 确定化 dump
    ├── parser.myp       # 顶层声明 / 类 / 语句
    ├── parser_expr.myp  # 表达式（grammar §6）
    ├── type.myp         # 类型表示
    ├── diag.myp         # 诊断引擎（消息 + 位置）
    ├── sema.myp         # 符号表 + 类型检查 + 成员解析 + 导入
    ├── ir_emit.myp      # LLVM IR 文本发射器（类型/模块/函数骨架）
    ├── codegen.myp      # 顶层/全局/函数 codegen 入口
    ├── codegen_expr.myp # 表达式 → IR
    ├── codegen_stmt.myp # 语句 → IR
    ├── codegen_class.myp# 类/构造器/ARC/异常/泛型/mapping → IR
    └── link.myp         # 调 llc + gcc 链接 C runtime（同 C++ linkObjects 逻辑）
```
