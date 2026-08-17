# MYP 全自举编译器——任务拆解（tools/selfhost）

> 状态：**设计定稿（2026-08-13）**｜对应 `design.md`｜范围：**只做全自举（非 GPU）**
> 拆解粒度：里程碑 → 任务 → 验收。每任务可独立合并/评审，按序推进。

---

## 总览

| 阶段 | 名称 | 侧重 | 预计工作量 | 状态 |
|------|------|------|-----------|------|
| F0 | Oracle 与骨架 | C++ dump 契约 + 项目脚手架 | ~1 天 | ✅ **已完成**（2026-08-13，67/67） |
| F1 | 完整词法器 | lexer / token | ~1-2 天 | ✅ **已完成**（2026-08-13，448/448 + 78/78） |
| F2 | AST + 声明/语句 parser | ast / parser | ~3-5 天 | ✅ **已完成**（2026-08-13，AST 448/448） |
| F3 | 表达式 parser | parser.myp（含表达式） | ~3-5 天 | ✅ **已完成**（2026-08-13，AST 448/448） |
| F4 | 语义分析 | sema.myp（含类型） | ~5-8 天 | ✅ **已完成**（2026-08-14，正语料全绿 548/565；剩余负例/GPU/遗留/自引用泛型顺序） |
| G1 | IR 文本发射框架 | ir_emit / codegen 骨架 | ~2-3 天 | ✅ **已完成**（2026-08-14，hello 级运行对拍） |
| G2 | 语句 + 表达式 codegen | codegen.myp | ~5-8 天 | ✅ **基本完成**（控制流/数组/字符串/短路/intrinsic/slice/定长数组/lambda(按值捕获)/元组/默认命名参数 已对拍） |
| G3 | 类/ARC/异常/泛型 codegen | codegen.myp | ~5-8 天 | 🔄 **进行中**（@static/实例/构造器/属性默认值/成员访问/泛型单态化/ARC(最简)/function 段/**异常全链路（string/typed/接口/finally/TryExpr）** 已对拍；mapping/@startup/协程待补） |
| G4 | 驱动 + 链接 | main / link | ~2-3 天 | 🔜 |
| H1 | 两级自举验证 | bootstrap + 全量回归 | ~2-3 天 | 🔜 |

> 原则（沿用 `docs/self_hosting.md`）：**每层独立可交付、可验证**；低层不依赖高层；
> 每层以对拍为验收。**GPU（5,462 行）与 C 运行时（runtime.c）不在任何阶段内。**

---

## F0：Oracle 与骨架（硬前置）✅ 已完成

> 目标：C++ `mypc` 落地确定化前端 dump 契约 + 项目骨架。**F1 之前必须完成**，否则无对拍基准。
> 实施（2026-08-13）：`mypc --frontend-dump {tokens,ast,sema}` 已落地（`src/frontend_dump.cpp`），
> 契约冻结于 `format.md`；MYP 骨架 `tools/selfhost/src/` + CMake `myp_self` + 测试脚本
> `tests/test_myp_self.sh`（**67/67 全绿**）。注：`build-asan` 树为过期配置（`codegen_gpu.cpp`
> LLVM API 预存在失败，与 F0 无关），重配置后即可回归。

### 任务

- [x] **F0-1** C++ `mypc` 新增 `--frontend-dump <mode>`（tokens/ast/sema），输出带版本头的
      确定化文本（`design.md` §5.1 + `format.md`）。
      - 文件：`src/main.cpp`（编排）+ `src/frontend_dump.cpp` + `include/mylang/FrontendDump.h`。
      - 约束：哈希容器 dump 前**排序**（T3 教训）；字符串/浮点转义确定化；**无损**
        （源位置/注解/token 原文，供 debug info 与 codegen）。
- [x] **F0-2** 冻结 dump 格式说明（`tools/selfhost/format.md`），作为 MYP 侧唯一契约。
- [x] **F0-3** 创建 `tools/selfhost/src/` 骨架 + 各模块空壳，入口 `int selfMain()`。
- [x] **F0-4** CMake custom target `myp_self`（ALL，模式同 `myp_fmt2`）：C++ `mypc` 编译
      `main.myp` → `build/myp_self`；清理 `main.myp.o`。
- [x] **F0-5** `tests/test_myp_self.sh` 骨架：tokens/ast/sema 三模式 oracle 驱动 + 确定性 +
      负例断言（**67/67 全绿**）。
- [x] **F0-6** 采样语料清单：stdlib/env.myp、examples/hello.myp、BNCTDoseEngine/physics.myp、
      tools/fmt/fmt.myp、examples/showcase.myp + 负例 tests/negative/call_non_function.myp
      ——每日对拍样本（固化于 test_myp_self.sh）。

### 验收

- `mypc --frontend-dump {tokens,ast,sema} <5 样本>` 稳定输出、带版本头、无损。
- `build/myp_self --help` 可运行；测试脚本骨架跑通空语料断言。

> **语料排除说明**：`examples/test_*.myp` 是遗留坏样例（含故意错误/过期语法），sema 模式
> 对它们报错是**正确行为**；全量对拍时此类文件按"预期报错"处理或排除（同 fmt/viz 排除
> `tests/negative` 的先例），不计入正语料。

---

## F1：完整词法器 ✅ 已完成

> 目标：`token.myp` + `lexer.myp` 复刻 `src/lexer/lexer.cpp`（520）+ `src/token.cpp`（136）。
> 起点：`tools/fmt/lexer.myp`。
> 实施（2026-08-13）：`token.myp`（Tok + TokKind 关键字表）、`diag.myp`（Diag + DiagEngine）、
> `lexer.myp`（逐字符复刻 C++：全算子/标点、数字 0x/0b/0o + 下划线 + 科学计数 + L/u/f 后缀、
> 转义全集、注释/空白/位置、错误路径）；`main.myp` 接入 `--frontend-dump tokens`。
> **验收：全语料 448/448 字节一致 + 测试 78/78 全绿**（含词法错误诊断对拍）。
> 过程踩坑：`match` 是 MYP 保留关键字，不能作方法名 → 改名 `matchCh`。
> **修复的 C++ 参考 bug**（照 T2/T3 先例，`src/token.cpp`）：`keywordString` 缺
> `Keyword_nonlocal`/`Keyword_bitfield`/`Type_bit`/`Type_bitvector`/`Tilde` 5 个 case
> → C++ 输出 `?`；已补全，MYP 侧输出原词/`~`。

### 任务

- [x] **F1-1** `token.myp`：TokenKind 全集 + `keywordString` 与 `src/token.cpp` 逐项对齐。
- [x] **F1-2** 数字扫描补齐：`0x`、`u`/`U`、`L`、科学计数、`.5`/`5.`、`f` 后缀。
- [x] **F1-3** 字符串/char：转义全集解码 + 原样值保留。
- [x] **F1-4** 注释/空白/位置：`//`、`/* */`、行尾续行；`begin`/`end` 与 C++ `SourceLocation` 一致。
- [x] **F1-5** 错误路径：非法字符/未闭合字符串/未闭合注释诊断对齐。

### 验收

- `myp_self tokens` vs C++ `--frontend-dump tokens`：全语料字节级对拍。
- 负例诊断一致。

---

## F2：AST + 声明/语句 parser

> 目标：`ast.myp` + `parser.myp` 复刻 `src/parser/parser.cpp`（1150）+ `parser_stmt.cpp`（1079）。

### 任务

- [ ] **F2-1** `ast.myp` 节点类（kind + 字段 + 无损 dump），对照 `include/mylang/AST.h`。
- [ ] **F2-2** 顶层解析：import/class（含泛型参数）/interface/struct/function/enum/type 别名/
      mapping/注解。
- [ ] **F2-3** class 三段式：action/event/property/function/static/struct/interface class +
      `@constructor`/`@startup`/`@thread`/`@gpu`/`@macro` 注解绑定。
- [ ] **F2-4** 语句：if/while/for（含 for-in/range）/return/break/continue/声明/表达式语句/
      mapping 块/match/try/throw/ffi。
- [ ] **F2-5** 类型产生式：基本/`T[]`/定长/切片/generic/函数/元组/`T?`。

### 验收

- `myp_self ast` vs C++：声明+语句子集字节级对拍（表达式差异先标记，F3 补齐）。

---

## F3：表达式 parser（已并入 parser.myp）

> 目标：`parser.myp` 内实现表达式解析（复刻 `src/parser/parser_expr.cpp`，1439），按
> `docs/grammar.md` §6。（原 `parser_expr.myp` 空壳占位已删除，实现合并进 parser.myp。）

### 任务

- [ ] **F3-1** 优先级框架：赋值→`|>`→三元→`||`→`&&`→等值→关系→加法→乘法→一元→后缀→primary。
- [ ] **F3-2** 后缀链：成员访问/下标/调用/`++`/`--`；`this` 处理。
- [ ] **F3-3** 前缀：`!`/`-`/`+`/`new`（含泛型 `new C<T>(args)`）/`await`。
- [ ] **F3-4** lambda：`(a,b)=>{}`、捕获（按值/`nonlocal`）、上下文类型推断。
- [ ] **F3-5** 泛型调用消歧：`foo<int>(x)` 诊断-free 扫描（不误伤 `E < energies[mid]`）。
- [ ] **F3-6** 字面量/构造/元组/三元/管道组合。
- [ ] **F3-7** 复合赋值 desugar 的 AST/dump 对齐。

### 验收

- `myp_self ast` vs C++：表达式语料字节级对拍（F2 差异标记清零）。
- 复杂用例：泛型消歧、lambda、三元嵌套、管道专项。

---

## F4：语义分析

> 目标：`sema.myp` + `diag.myp` 复刻 sema 全套（sema 2842 + sema_expr 3718 +
> symbol_table 41 + type 38）。**体量最大，按子域切分，每子域独立对拍。**
> （原 `type.myp` 空壳占位已删除，类型表示/提升并入 sema.myp。）

### F4a 基础设施

- [ ] **F4a-1** `diag.myp`：消息文本、`file:line:col`、`errorCount()`、排序输出。
- [ ] **F4a-2** 类型表示 + 比较/提升（数字提升链，并入 sema.myp）。
- [ ] **F4a-3** 作用域链（复刻 `symbol_table.cpp`）。

### F4b 声明语义

- [ ] **F4b-1** 类/接口/struct/function 顶层登记 + 泛型参数/接口约束。
- [ ] **F4b-2** 类成员解析 + `var_class_map_` 成员访问解析。
- [ ] **F4b-3** 构造器绑定：`@constructor`/同名、重载解析、默认参数克隆。
- [ ] **F4b-4** import 跨模块：递归加载、去重、符号可见性。

### F4c 表达式语义

- [ ] **F4c-1** 表达式类型推断（字面量/二元/调用/成员/下标/new/三元/管道/lambda/元组）。
- [ ] **F4c-2** 重载解析（位置+命名+默认+泛型实参推断）。
- [ ] **F4c-3** 泛型约束检查（`where`、关联类型 `T::Item`，仅登记不单态化）。
- [ ] **F4c-4** lambda 捕获分析、mapping 链类型一致性、事件→action 参数匹配。

### F4d 诊断对齐

- [ ] **F4d-1** 正语料 sema 符号表 dump 对拍。
- [ ] **F4d-2** 负语料 errorCount + 每条 `file:line:col: msg` 对拍。
- [ ] **F4d-3** 差异登记到 `docs/next_improvements.md`（C++ 参考 bug 一并修，照 T2/T3 先例）。

### 验收

- 正语料 sema 字节级一致；负语料错误计数一致、消息逐步逐字符一致。
- 泛型/接口/元组高级用例作 F4b 扩展，不阻塞 F4 核心收口。

---

## G1：IR 文本发射框架

> 目标：`ir_emit.myp` + `codegen.myp` 骨架——最小可编译闭环（hello world）。
> 核心：**发射 LLVM IR 文本（.ll）+ 外部 `llc` + `gcc` 链接**（`design.md` D4）。

### 任务

- [x] **G1-1** `ir_emit.myp`：LLVM IR 类型表（i8..i64/f32/f64/ptr/struct/array/function）打印。
- [x] **G1-2** 模块/全局/函数签名发射（对齐 `mypc --emit-llvm` 形态）。
- [x] **G1-3** 最小 codegen：`main()` 返回 + 字面量 + 简单 `Console.writeString/writeLine`
      调用（FFI 到 runtime `__myp_*`）。
- [x] **G1-4** `link.myp` 雏形：`llc file.ll -filetype=obj` + `gcc` 链接缓存编译的
      `runtime.o` + `coro_ctx.S` + `-lpthread -lm -ldl` + `--gc-sections`。
- [x] **G1-5** `main.myp` 编译管线编排：lexer→parser→sema→codegen→link（先 `-O0`）。

### 验收

- `myp_self hello.myp` 产出可执行文件，运行输出与 `mypc hello.myp` **字节一致**。
- `--emit-llvm` 产出 .ll 可被 `llc` 编译通过。

---

## G2：语句 + 表达式 codegen（并入 codegen.myp）

> 目标：`codegen.myp` 复刻 `codegen.cpp`（2860）+ `codegen_stmt.cpp`（2772）+
> `codegen_expr.cpp`（3689）的非 GPU 核心。（原 `codegen_stmt.myp`/`codegen_expr.myp`
> 空壳占位已删除，实现并入 codegen.myp。）

### 任务

- [x] **G2-1** 变量声明/赋值/作用域 + 基本类型运算（含数字提升、比较、短路 `&&`/`||`）。
- [x] **G2-2** 控制流：if/while/for/for-in/range/break/continue/return。
- [x] **G2-3** 数组/切片/字符串：`new T[n]`、下标、字符串拼接 ✅；`slice<T>` fat pointer 已对拍（`27ed30c`：new/下标/size/data/for-in，验收 47/10 均与 mypc 一致）。
- [x] **G2-4** 函数调用：顶层函数 ✅；默认/命名参数（sema normalizeCallArgs 归一化，codegen 零额外逻辑）、多值返回、元组（字面量/解构/字段访问 t.0）均已对拍（`565ef47`/`e00cef3`）。
- [ ] **G2-5** lambda/闭包：fat pointer + 捕获（按值）已对拍；`nonlocal` cell（按引用捕获）待补。
- [x] **G2-6** 内建/FFI：`__myp_*` → `myp_*`（去两个下划线）。
- [x] **G2-7** 复合赋值 desugar 的 IR 对齐（parser 已 desugar 为 Assign）。

### 验收

- 控制流/表达式/函数/闭包用例产物运行输出 vs `mypc` **字节一致**。
- 负例：sema 拦截的语义错误与 C++ 一致（不进入 codegen）。

---

## G3：类/ARC/异常/泛型 codegen + 运行时对接（并入 codegen.myp）

> 目标：`codegen.myp` 复刻 `codegen_class.cpp`（1960）+ `codegen_test.cpp`（295），
> 并对接 C runtime（ARC/事件/线程/协程/异常 FFI 契约不变）。
> （原 `codegen_class.myp` 空壳占位已删除，实现并入 codegen.myp。）

### 任务

- [x] **G3-1** 类实例分配/构造器/成员访问/方法调用（含 `var_class_map_` 解析）。
- [ ] **G3-2** ARC 插桩：对象头 `{rc,type_id}`、retain/release、`__myp_release_table`、
      作用域退出释放、`@weak`、跨线程原子 ARC。
- [ ] **G3-3** mapping/event：`__myp_inst_X` 全局、mapping 注册、事件 fire/dispatch。
- [x] **G3-4** 异常：throw/catch/finally 展开 + 错误消息拷贝（string/typed class/Error 接口/finally+return/TryExpr 全链路，`790512b`..`03bfdfb`）。
- [ ] **G3-5** 协程/`@threadpool`/`@async`/await：对接 runtime coro（@thread 已做，`2edea55`）。
- [x] **G3-6** 泛型单态化：`T` 替换、`foo_int_inst`、`T::Item` 关联类型。
- [ ] **G3-7** `@gpu` 源文件 CPU 顺序回退（语义保持，无 GPU 发射）。
- [ ] **G3-8** `@parallel for`、`@startup` 自动 main、`@test` 生成器（`codegen_test`）。

### 验收

- 完整语料（stdlib/examples/tools/tests/BNCTDoseEngine 可编译样本）产物运行输出 vs `mypc`
  **字节一致**；含 `@gpu` 样本 CPU 回退行为一致。
- ARC 泄漏：长跑后 `Memory.liveObjectCount` 归零（ASAN 下跑）。

---

## G4：驱动 + 链接

> 目标：`main.myp` + `link.myp` 复刻 `main.cpp`（1023）的驱动/链接/子命令。

### 任务

- [ ] **G4-1** 多文件编译 + import 依赖文件链。
- [ ] **G4-2** `run` 子命令（仿 go run，临时产物 + 透传 args + 退出码）。
- [ ] **G4-3** `fmt` 子命令（建议调 `myp_fmt2` 产物，避免重复实现，见 R2）。
- [ ] **G4-4** 链接完善：sdl/gpu bridge `nm -u` 按需探测、runtime .o 缓存、`-O*` 透传
      `llc`/`gcc`、`--stdlib`/`--package-path` 路径解析。
- [ ] **G4-5** `--emit-llvm`/`--frontend-dump` 出口 + 退出码语义对齐。

### 验收

- `myp_self` 全流程（编译/run/fmt/对拍出口）与 `mypc` 行为一致。
- 链接产物运行输出字节一致；`-O0`/`-O2` 均过。

---

## H1：两级自举验证 + 全量回归

> 目标：证明自举成立，收口文档与回归接入。
> **阶段性达成（2026-08-14，ec80997）**：两级自举编译链已跑通——`mypc → myp_self
> → myp_self3 → myp_self4` 全部编译成功，self3/self4 对 hello/lam2 行为一致
> （42/34）；为跑通需补齐的运行时语义（定长数组 `[N x T]` 布局、ARC 引用计数、
> Unary/Ternary/Null/This、function 段方法、泛型实例体类型替换）均已落地。
> 剩余：全量回归接入 + 性能基准 + 文档收口。

### 任务

- [x] **H1-1** stage1：`mypc` 编译 `tools/selfhost/src/*.myp` → `build/myp_self`。
- [x] **H1-2** stage2：`myp_self` 编译同样源码 → `build/myp_self2`。
- [x] **H1-3** stage3：`myp_self2` 编译同样源码 → `build/myp_self3`。
- [x] **H1-4** 自举判定：stage2/stage3 对同一语料（前端 dump + 产物运行）行为一致
      ——`tests/test_myp_bootstrap.sh`（15/15，2026-08-14）。
- [ ] **H1-5** 性能基准：`myp_self` vs `mypc` 编译耗时（记录基线，≤10x，目标 ≤3x）。
- [ ] **H1-6** 文档收口：`docs/self_hosting.md`（T5 完成状态）、`docs/CHANGELOG.md`（精简内联）、
      README 工具清单；`run_tests.sh` 接入；`-O0`/`-O2`/ASAN 三套回归。

### 验收

- stage1/2/3 产物行为一致 → **自举成立**。
- 全量验收标准（`design.md` §6 七条）通过。

---

## 依赖关系

```
F0 ──> F1 ──> F2 ──> F3
              └─────> F4a ──> F4b ──> F4c ──> F4d
F0 ──> G1 ──> G2 ──> G3（G 系依赖 F 系：F2/F3 无损 AST + F4 符号表/类型）
G3 ──> G4 ──> H1
```

- F1 依赖 F0（oracle）；F2/F3 依赖 F1；F4 依赖 F2（AST 完整）。
- **G 系依赖 F 系**：G1 需 F3 的无损 AST + F4 的符号表/类型信息（codegen 需要已解析类型）；
  但可先用"最小 AST 子集"并行起步 G1 骨架（R1 建议 G1 骨架可提前验证 IR 契约）。
- G4 依赖 G1–G3（链接需 codegen 产物）；H1 依赖 G4。
- **H1 是独立脚本/CI 步骤，不进默认 CMake 构建**（避免自依赖环）。

## 风险备忘

见 `design.md` §7。重点：**sema 与 codegen 各占约一半工作量**，务必按 F/G 子域切碎、
每段独立对拍；**IR 文本发射与 LLVM API 的语义差异**由"产物运行输出"主验收兜底；
**自举验证的自依赖环**通过独立脚本规避。

---

## 后续计划（H1 之后，2026-08-17 基于 soft2 全量测试 + C++/Go 基准结论立项）

> 背景：用 soft2（stage1 自编译 myp_self）跑全量测试（274/275）与 40 项 C++/Go 基准
> （verify 全一致）后，验证出以下后续项。**优先级按 ROI 排序；任何 codegen/link 改动
> 必须过 `tests/test_myp_bootstrap.sh`（不动点）+ 全量回归把关。**

### P1 运行时/字符串：字符串头加长度字段（先做，收益最广）
- **现状**：字符串无长度字段 → `myp_str_append`/`myp_strcat` 每次 `strlen(已累加串)`
  O(len)；`Str.len`/`myp_str_eq`/`myp_str_cmp` 同理。此前 LD_PRELOAD 实测 strlen 曾占
  运行 29%（`__strlen_evex` 最大单项）；`s = s + x` 裸累加仍 O(n²)（N 翻倍时间×4）。
- **方案**：字符串头 `{len, rc, type_id}`（len@data-16，rc@data-8，type_id@data-4 不变
  → 既有 retain/release 读取位置不动）。需同步：`runtime.c myp_alloc_str` 16 字节头、
  **C++ 与自举两个 codegen 的字面量发射**（`{i64, i32, i32, [N x i8]}` GEP 0,3）、
  `myp_str_len` 读头。风险中（改动 ABI 相关布局，务必 ASAN + 全量回归）。
- **收益**：全局（所有生成程序 + 编译自身）；消除字符串累加 O(n²) 与 ~29% strlen。

### P2 生成代码性能对齐：soft2 数值循环向量化缺口（专项跟进）
- **现状**：soft2 编译产物平均慢 mypc ~14%（几何平均 1.138），最坏**数值向量化循环**
  matmul 2.43x / matrix_int_mul 2.50x / fib_matrix 2.20x / floyd 1.62x / sha256 1.41x；
  内存/字符串/混合负载持平或更好（kmeans 0.83x、coro_switch 0.95x）。
- **根因（反汇编实证）**：mypc -O2 二进制 matmul 用 **SSE2 向量化（`mulpd`/`addpd`）**，
  soft2 二进制**纯标量（`mulsd`/`addsd`）**。两者原始 IR 经**同一外部 `opt-21 -O2` 都
  不向量化** → 差距在 **mypc 进程内 LLVM pass pipeline 比 soft2 的外部 `opt -O2` 更强**
  （mypc `--emit-llvm` dump 有 `i0` 类型瑕疵，与真实编译路径不一致，佐证 dump 不忠实）。
- **下一步侦察**：在 `mypc codegen.generate(ast, output, opt_level)` 的进程内 pipeline
  打点，确认多跑了哪些 pass（-O3？额外 vectorize？pass 顺序？），再决定"对齐 pipeline"
  还是"调整 soft2 IR 形状"。

### P3 完整自举三步（从"子集自举"到"完全自举"）
1. **功能补齐**：GPU/剩余语言特性在 myp_self 落地（当前非 GPU、部分特性子集；
   274/275 是在"它能编译的子集"上跑的）。
2. **去委托**：`myp_self run`/`fmt` 目前 `delegateToMypc`（shell 到 mypc）→ 改为自托管
   实现（`run` 复用 G4 链路；`fmt` 见 design.md R2 调 `myp_fmt2`）。
3. **甩掉 C++ 种子（终极验收）**：只用 myp_self（+ 一份已编译 myp_self 二进制）从源码
   重建整个工具链，全程不调用 mypc。
4. **性能对齐**：P2 落地后 soft2 产物性能与 mypc 持平。

### P4 已知遗留问题（登记，按需处理）
| 项 | 现象 | 处置 |
|----|------|------|
| mypc `--emit-llvm` dump `i0` 类型瑕疵 | `store i0 0` 使独立 `opt` 拒绝整文件 | 修 C++ dump（oracle 对齐，F4-O 流程） |
| `-O2` × 异常展开 | `@test/arc_throw` runtime 失败（mypc -O2 与 soft2 同样失败，既有） | 排查 opt 下异常 unwind 语义，另立任务 |
| 基准源 `nqueens`/`alphabeta` | 违反属性私有规则，mypc 与 soft2 **都**编译失败（非自举缺口） | 修两个基准源文件（bench/myp/） |

### 未完成项（roadmap 原清单遗留）
- **H1-5 性能基准**：`myp_self` vs `mypc` 编译耗时基线（≤10x，目标 ≤3x）。本轮已实测
  整链编译：soft2 编译 10 文件链 ~20s、单文件 ~0.7-3s（opt +24% 开销）；正式基线待补录。
- **H1-6 文档收口**：`docs/self_hosting.md`（T5 完成状态）、README 工具清单、
  `-O0`/`-O2`/ASAN 三套回归接入（`run_tests_O2.sh` 的 `arc_throw` 失败即 P4-2）。
