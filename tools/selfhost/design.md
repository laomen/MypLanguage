# MYP 全自举编译器——详细设计（tools/selfhost）

> 状态：**H1 自举编译链跑通（2026-08-14）**｜版本：0.6（范围：只做全自举，非 GPU）
> 目标：用 MYP 完整重写 `mypc`（前端 + 非 GPU codegen + 驱动），交付 `myp_self`，
> 完成两级自举验证。**不做**混合编译（C++ 代做 codegen）、**不做** GPU。
> 前置阅读：`docs/self_hosting.md`、`docs/grammar.md`（规格 v1.0）、`src/lexer/`、`src/parser/`、
> `src/sema/`、`src/codegen/`、`tools/fmt/lexer.myp`。

---

## 1. 目标与范围

### 1.1 目标

1. 用 MYP 完整复刻编译器三阶段 + 后端：**lexer → parser → sema → 非 GPU codegen → 链接**。
2. 交付自举编译器 `build/myp_self`，行为与 `mypc` 一致（诊断、IR、产物运行输出）。
3. 完成经典**两级自举验证**：`mypc` 编译自举编译器（stage1）→ 自举编译器自编译（stage2）
   → 两代产物行为一致，证明"编译器能用自己编译自己"。

### 1.2 范围边界（用户已拍板）

| 在范围内 | 排除 |
|----------|------|
| lexer / parser / sema（前端） | **GPU 实现**（`@gpu for` / NVPTX / runtime_gpu / CUDA，5,462 行）——永久保留 C++ 后端；含 `@gpu` 源文件走既有 CPU 顺序回退语义（`-DMYP_ENABLE_GPU=OFF`） |
| **非 GPU codegen**（IR 文本发射，等价 C++ 约 1.17 万行） | **C 运行时 `runtime.c`**（约 6,000+ 行）——视作生成程序的"libc"，保留 C；自举只重写编译器本体，FFI/链接契约不变 |
| CLI 驱动（编译 / `run` / `fmt` 子命令、`--stdlib`、`-o`、链接） | `@macro` / `@eval` 编译期求值（依赖宏引擎，后续立项） |
| 两级自举验证（H1） | 完整 `myp_lsp` / DAP（远期，不在本项目） |

> **前端不受 GPU 排除影响**：`@gpu` 只是注解，前端无损解析照常覆盖；GPU 处理全在后端。

### 1.3 参考实现规模（wc -l，2026-08-13）

| 部分 | 文件 | 行数 | 合计 |
|------|------|------|------|
| 前端 | lexer 520 / token 136 / parser 1150+1439+1079 / sema 2842+3718 / symbol_table 41 / type 38 | — | **≈ 10,963** |
| 后端（非 GPU） | codegen 2860 / codegen_class 1960 / codegen_expr 3689 / codegen_stmt 2772 / codegen_test 295 / myp_passes 141 | — | **≈ 11,717** |
| 驱动 | main.cpp（编译管线 + 子命令 + 链接） | 1,023 | ≈ 1,023 |
| **非 GPU 合计** | | | **≈ 2.37 万** |

> GPU 5462 行明确剔除。**sema 与 codegen 各占约一半**——任务拆解必须切碎、逐段对拍。

### 1.4 与既有自举工具的关系

- 复用：`tools/fmt/lexer.myp`（mini 词法器起点）、模块化约定（`main.myp` 入口 `int xxxMain()`）、
  CMake target 模式、测试脚本模式（`tests/test_myp_*.sh` 并入 `run_tests.sh`）。
- 复用 stdlib：`text`（StringBuilder 动态）、`io`/`fs`/`args`/`env`（文件/路径/CLI）、
  `collections`（ArrayList/HashMap/Set，符号表）、`process`（调 `llc`/`gcc`）、`json`。

---

## 2. 参考管线

```
SourceManager.loadFile → Lexer.tokenize → Parser.parse → [imports: loadModule 递归加载]
→ Sema.analyze → evaluateCompileTimeConstants → CodeGen.generate（LLVM C++ API 进程内）
→ linkObjects（gcc + runtime.o + coro_ctx.S + 可选 bridge + --gc-sections）
```

`myp_self` 复刻到 codegen+link：**CodeGen 改为发射 LLVM IR 文本（.ll）**，link 改为
调 `llc` 编译 .ll → 目标文件，再 `gcc` 链接 C runtime（逻辑同 `linkObjects`）。

---

## 3. 关键设计决策

### D1 交付形态：独立二进制 `myp_self`（不替换 mypc）

- 模块化 `tools/selfhost/src/*.myp` → CMake custom target `myp_self`（ALL，模式同 `myp_fmt2`）。
- CLI（对齐 `mypc`）：
  - `myp_self <file.myp> [-o out] [--stdlib <path>] [-O*] [--trace] [--emit-llvm]`
  - `myp_self run <file.myp> [args...]`（仿 go run，含单类 `@startup` 自动 main）
  - `myp_self fmt [--check] <file.myp> ...`（复用 `tools/fmt` 逻辑或内联）
  - `myp_self --frontend-dump {tokens,ast,sema} <file.myp>`（对拍 oracle 出口）
- 入口约定：`int selfMain() { return Cli.run(); }`。

### D2 验收 oracle（双轨）

1. **前端**：C++ `mypc --frontend-dump {tokens,ast,sema}`（F0 落地，带版本头的确定化文本）
   → `myp_self --frontend-dump` 字节级对拍。
2. **后端**：
   - 主：`myp_self` 编译产物运行输出 vs `mypc` 直编产物运行输出**字节一致**。
   - 增强：`--emit-llvm` 的 .ll 文本对拍（允许寄存器命名/顺序差异，先规范化再比；不强求逐字节）。
3. **诊断**：负语料 errorCount + `file:line:col: msg` 对拍。

> 原则：dump 格式先在 C++ 侧**冻结**，MYP 侧按契约实现，禁止反向迁就（T3 教训：哈希容器
> dump 前必须排序）。

### D3 AST 表示：MYP class + kind 枚举（无继承）

同前版设计：`Node` 类 + `kind` 枚举 + 可选字段 + `dump(StringBuilder,int)`。比 C++ 多态节点
扁平，但 dump 契约保证对拍一致。**无损**：dump 必须携带源位置、注解、token 原文（供 debug
info 与 codegen 精度），F0 起就按无损冻结。

### D4 codegen 策略：发射 LLVM IR 文本（.ll）+ 外部 `llc`/`gcc`（核心决策）

MYP 程序**无法在进程内复用 LLVM C++ API**（无 C++ 类 FFI）。可选路径：

| 路径 | 做法 | 优劣 |
|------|------|------|
| **P1（推荐）** | 发射 **LLVM IR 文本（.ll）** → 外部 `llc` 编译为目标文件 → `gcc` 链接 C runtime | ✅ 与 `mypc --emit-llvm` 天然可对拍；✅ LLVM 后端承担指令选择/优化，MYP 侧只管"语义→IR"；✅ 确定性、可调试 |
| P2 | 发射 C 源码 → gcc | 更简单但 IR 对拍不可用、性能/语义保真度低；不选 |
| P3 | FFI 到 LLVM C API（libLLVM.so） | 需大量 C API 绑定（long/指针承载），工程量大且繁琐；不选 |

- `ir_emit.myp` 输出 LLVM IR 文本：类型表（i8..i64/f32/f64/ptr/struct/array/function）、
  全局、函数签名、指令。与 `mypc --emit-llvm` 的 IR 形态一致（LLVM 版本语义）。
- **链接**：`link.myp` 复刻 `linkObjects`——`llc file.ll -filetype=obj` + `gcc` + 缓存编译的
  `runtime.o` + `coro_ctx.S` + 可选 bridge（sdl/gpu 按需）+ `-ffunction-sections`/
  `-Wl,--gc-sections`。Sdl/gpu bridge 仅当产物引用对应符号时链接（照抄 C++ `nm -u` 探测）。
- **llc 探测（实测 2026-08-13）**：本机 `llc` 不在 PATH，二进制为 `/usr/bin/llc-21` 与
  `/usr/lib/llvm-21/bin/llc`。`link.myp` 须按 `llc` → `llc-21`/`llc-20` →
  `/usr/lib/llvm-*/bin/llc` 顺序探测并缓存，不得硬编码 `llc`。
- **优化等级**：`-O*` 传给 `llc`/`gcc`（C++ 侧是 LLVM 优化 pass 交给 opt；MYP 侧用
  `llc -O*` 对齐行为）。

### D5 运行时边界（runtime.c 保留 C）

`runtime.c`（约 6,000+ 行：事件/线程/协程/ARC/字符串/数组/FFI 基元）是生成程序的运行时库，
**视作 libc，不在自举范围**。`myp_self` 生成的程序与 `mypc` 生成程序共用同一 `runtime.c` +
同一 FFI ABI（`__myp_*` 签名冻结）。这是自举可行性的关键简化：**只需自举编译器，不必自举
运行时**。

### D6 import 解析：复刻 C++ `loadModule`

搜索顺序：`--stdlib` → exe 目录 `../stdlib/` → 源文件目录 `stdlib/` → `--package-path`；
路径去重；跨模块符号合并。用 `fs.myp` + `io.myp`。

### D7 lexer 起点：扩展 `tools/fmt/lexer.myp`

补齐：`UIntLiteral`（`u`/`U` 后缀）、`LongLiteral`（`L`）、`FloatLiteral32`（`f`）、科学计数
`1e3`/`1.5E-2`、`0x`、`isKeyword` 缺失词、位置 `begin`/`end` 与 C++ `SourceLocation` 一致。

### D8 Bootstrap：两级自举验证（H1）

```
stage0: C++ mypc 编译 tools/selfhost/src/*.myp        → build/myp_self   （第一代）
stage1: myp_self 编译 同样的 src/*.myp                → build/myp_self2  （自编译）
stage2: myp_self2 编译 同样的 src/*.myp               → build/myp_self3
自举成立 ⟺ myp_self2 与 myp_self3 对同一语料行为一致（编译结果 + 产物运行输出）
```

- 经典 self-hosting 验证：stage1/stage2 产物输出一致 → 编译器已能自我复制。
- 每代都过全量回归（`tests/test_myp_self.sh`：`--frontend-dump` 对拍 + 产物运行对拍）。
- CMake：`myp_self` 用 C++ `mypc` 编译（MYP_CC 子进程模式，同 `tools/pm`）；自举验证作为
  独立脚本/CI 步骤，不进入默认构建（避免自依赖环）。

### D9 泛型单态化：规范实例与 dump 事件分离

当前 F4 的主要阻塞不是单个类型替换分支，而是泛型类、泛型函数和泛型静态方法分别维护
实例化逻辑。C++ 参考前端还会因作用域局部查找而把同一泛型类实例多次追加到 AST；这些重复项
属于冻结 dump 的可观察行为，却不应成为后端重复发射的实体。

采用两层模型：

1. **规范实例（canonical instance）**：同一模板和同一组具体类型参数全局只物化一次，供
   sema 查询和 codegen 发射。
2. **dump 事件（dump event）**：记录参考前端在何时、何作用域把实例追加到可见 AST；
   `--frontend-dump sema` 按事件顺序重放，允许同一规范实例出现多次。

> **实测验证（2026-08-13）**：全语料重算分类仍为 67/22/10/83，基线可靠。参考 C++ 对
> **泛型类实例确实按触发点重复追加**——19 个文件的 sema dump 含同名 `_inst` 类声明
> （`tools/selfhost/src/ast.myp` 中 `ArrayList_AstAction_inst` 声明 3 次，行 14785/
> 15721/23053），且**每份克隆字节完全相同**（156 行块两两 diff IDENTICAL）→ “克隆规范
> 实例 + 重放 dump 事件”可精确复现，D9 前提成立。泛型静态方法 `__gs_` 参考端**全局去重**
> （`generic_static.myp`：5 个调用点 `resolved=` 注解 + 5 个唯一顶层 `(Function` 声明，
> 无重复）→ 该类别只需规范实例，**不需要 dump 事件层**（比设计原文更简单）。

统一实例键：

```
GenericKey = kind + templateIdentity + canonical(typeArgs)
kind       = class | function | static-method

class:         class|ArrayList|AstAction
function:      function|identity|int
static method: static-method|Math.sin|double
```

`templateIdentity` 使用声明身份而非仅用短名称；跨模块后至少包含模块/翻译单元和所属类，避免
同名模板碰撞。`canonical(typeArgs)` 递归编码基本类型、数组、切片、元组、函数类型和嵌套泛型，
不得直接依赖 dump 字符串。

实例状态机：

```
Declared -> Materializing -> Ready
                        \-> Failed
```

- 先登记 `Materializing` 再递归替换，阻止 `Node<Node<T>>` 等路径无限重入。
- `Ready` 实例可被后续调用复用；`Failed` 缓存首个确定诊断，避免重复报错。
- codegen 只遍历 `Ready` 的规范实例，绝不遍历 dump 事件。

所有泛型入口共用以下管线：

```
收集实参类型
  -> 解析显式类型参数或结构化推导
  -> 规范化类型参数
  -> 校验数量和 where/接口约束
  -> registry.getOrCreate(key)
  -> 递归 substitute(type, bindings)
  -> 设置 resolved_call_name / concrete class name
  -> 记录 dump event
```

`substitute` 必须覆盖 `T`、`T[]`、定长数组、slice、`Box<T>`、嵌套泛型、元组和函数类型，
并用于返回类型、参数、属性、基类和关联类型。实例命名保持现有 ABI：类/函数使用
`Name_<types>_inst`，泛型静态方法使用 `__gs_Class_method_<types>_inst`。

在 MYP 实现中先用并行 `ArrayList` 表示 registry（键、状态、实例 AST、类型实参），待前端完全
对拍后再考虑 HashMap；现阶段优先保证确定顺序。现有 `instDone_`、`instNames_`、`instTAs_`
迁移为 registry 的查询接口，`instantiateClass` 保留为薄包装，避免一次改动所有调用点。

---

## 4. 当前基线与执行方案（2026-08-13）

### 4.1 已完成基线

全语料口径为 `stdlib/`、`examples/`、`tools/`、`tests/`、`BNCTDoseEngine/` 下 448 个
`.myp` 文件，参考输出由同一构建树中的 `mypc --frontend-dump` 产生。

| 能力 | 当前结果 | 结论 |
|------|----------|------|
| tokens | **448/448 字节一致** | F1 完成 |
| AST | **448/448 字节一致** | F2/F3 完成 |
| sema | **548/565 字节一致** | F4 完成（正语料全绿；剩余负例/GPU/遗留/自引用泛型顺序） |
| codegen | G1 完成（hello 级运行对拍）；G2 核心完成（控制流/数组/字符串/短路/intrinsic/slice）；G3-1/G3-6a 完成（类/实例方法/构造器/属性默认值/成员访问/泛型单态化） | G2-5 lambda 闭包进行中 |

> 更新（F4-G1 后，2026-08-13）：**泛型静态方法已落地**——sema 266→303（+37 文件），
> GS 簇 67→6。`__gs_` 实例函数（含函数类型参数替换 `(T)->R`→`(int)->string`）与调用点
> `resolved=` 已与 C++ 一致；共享 Call 尾部加 `argsVisited_` 防实参双重访问。剩余 6 个 GS
> 文件均为边界情况：3 个 codegen 测试（C++ 把结构体字段 `g.g0` 解析成 `void` → 推断 int，
> MYP 更正确 → 属 C++ oracle 对齐，F4-O）；3 个 BNCT（import 合并顺序 → `__gs_` 实例
> 顺序差，集合一致，属已知对齐难点）。

剩余 sema 差异按首要特征分类：

| 类别 | 文件数 | 处理顺序 |
|------|--------|----------|
| 泛型静态方法 `__gs_` | 6（原 67） | ✅ F4-G1 主簇已清，剩 6 个边界文件 |
| 泛型类实例 `_inst` | 22 | 进行中：占位符延迟实例化完成（清幽灵实例）；作用域感知去重+级联部分落地 |

> **F4-G2 作用域模型验证（2026-08-14）**：实测确认 C++ 对泛型类实例按**作用域**重复追加
> 字节相同的克隆（gen_orm 参考 `ArrayList_FfiParam_inst`×3 等，MYP 曾×1）。正确模型 =
> C++ 的 Pass 1（`visitClassDecl` 在**类作用域**解析属性+action/函数参数+返回类型，构造器
> `continue` 跳过参数）→ 弹类作用域 → Pass 2（每个 body 独立作用域**再次**解析参数+body）。
> 按此模型重构后 2× 实例计数**精确对齐**（MYP 1→2 与参考一致）。但弹出类作用域使 action 体
> 的裸属性名/下标/成员访问失效 → 需实现 **`buildCurrentClassMemberTypes` 式成员类型回退**
> （currentClass_ + 属性表，覆盖 Identifier/Subscript/Member/New 全表达式）才能无回归。
> 该回退是独立机制，需专项实施；当前已回退到 304/448 基线（避免回归）。
| lambda 合成类 `__lambda` | 13 | 第三优先，独立于泛型 registry 收口 |
| 其他语义/诊断 | 103 | 按最小差异簇逐项清零（含 C++ oracle 缺口） |

上述分类用于排优先级，不代表各集合互斥；每轮修改后必须重新跑全语料并重新分类。

### 4.2 F4 收口步骤

#### F4-G1：泛型静态方法

- 登记类静态 action/function 模板及其类型参数、约束和声明身份。
- 支持显式类型参数与从直接参数、数组元素类型推导。
- 复用统一约束检查和递归类型替换。
- 物化顶层 `AstFunction`，命名为 `__gs_Class_method_<types>_inst`，写回调用节点的
  `resolved` 字段。
- 参考端对 `__gs_` **全局去重**（实测：5 调用点注解 + 5 唯一声明）→ 本阶段只需规范
  实例，不做 dump 事件层；未去重反而会对拍失败。
- 先用 `Math`/集合静态泛型方法专项对拍，再跑 448 文件；目标是清零 `__gs_` 主差异簇。

#### F4-G2：泛型类规范实例与 dump 兼容

- 将 `instantiateClass` 接入 registry，递归处理嵌套类型实参。
- 规范实例全局去重；每个参考追加点单独记录 dump 事件。
- 实测参考端重复克隆**字节完全相同**（`ast.myp` 三份 156 行块 diff IDENTICAL）→ dump
  事件 = 在同一触发点克隆规范实例即可；与 C++ 触发点顺序一致是唯一对齐难点。
- 先对齐实例名称、成员替换和方法返回类型，再对齐重复次数及声明顺序。
- 用 `ArrayList<AstAction>` 等高频实例验证：dump 可重复，规范实例计数必须为 1。

#### F4-L：lambda 合成类

- 按参考前端顺序生成 `__lambda*` 隐藏类，固定捕获槽、nonlocal cell 和调用签名。
- lambda 名称分配器按翻译单元重置，不使用哈希迭代顺序。
- lambda 不进入泛型 registry；若捕获类型含泛型实例，只引用已规范化的具体类型。

#### F4-O：其他差异

- 每次选取同一根因的最小文件簇，先建立单文件可复现 diff，再修改 owning path。
- 诊断比较包括位置、文本、数量和顺序；parser 诊断在进入 sema 前清空，避免跨阶段泄漏。
- 不通过放宽 dump 契约掩盖差异；发现参考实现非确定或错误时，先修 C++ oracle，再同步
  `format.md` 和 MYP 实现。

> **已确认 C++ oracle 缺口（2026-08-14 排查，暂缓，先记后修）**
> - **接口默认方法体不类型检查**：`tests/@test/interface_default.myp` 的 `IShape`
>   默认 `describe()` 内，C++ 把所有表达式标成默认 `: int`——字符串字面量 `"area="`
>   、`area()`（返回 double）、`perimeter()`、整个 `+` 拼接全是 `: int`。根因：C++
>   不 sema 接口默认方法体（表达式保持默认 resolved_kind=Int）。
> - 按 F4-O 流程应**先修 C++ oracle**（让 mypc 类型检查接口默认方法体，输出正确的
>   string/double），再同步 MYP（当前 MYP 未类型检查 → 无后缀，与 C++ 的 `: int`
>   对不上）。影响面：含接口默认方法的少数文件。
> - 排查方法论备忘：对拍差异里 "Float/String 字面量被标 int" 要先看是否
>   两端 rc=1（引用未定义符号的坏文件，错误路径级联差异），不要误判为干净 C++ bug。
>   `test_autodiff`/`test_ffi`（未定义 f1/myp_math_sqrt）、`transport.myp`
>   （未 import data_manager）均属此类。

### 4.3 从前端到完整自举

| 门禁 | 必须满足后才能进入下一阶段 |
|------|----------------------------|
| F4 -> G1 | 448/448 sema 字节一致；连续两次结果相同；自举编译器自身源码包含在语料内 |
| G1 -> G2 | hello/基本函数可生成合法 `.ll`，经 `llc`/`gcc` 链接并与 `mypc` 运行输出一致 |
| G2 -> G3 | 基本类型、控制流、数组、字符串、函数、tuple、lambda 的专项运行对拍通过 |
| G3 -> G4 | 类、ARC、异常、协程、mapping、泛型规范实例通过；同一实例只发射一次 |
| G4 -> H1 | 全语料可编译子集运行对拍通过，CLI/链接/退出码一致 |
| H1 完成 | stage1、stage2、stage3 对固定语料的前端 dump 和生成程序行为一致 |

后端实现按依赖闭环推进，不按 C++ 文件逐行翻译：

1. `ir_emit.myp` 先提供 SSA 名、基本块、声明、函数、全局和核心指令的确定化发射。
2. `codegen.myp` 打通基本类型、函数调用和返回，形成第一个可链接程序。
3. `codegen_expr.myp` / `codegen_stmt.myp` 扩展表达式与控制流，每增加一簇立即运行对拍。
4. `codegen_class.myp` 最后消费 F4 产出的规范实例，接入布局、ARC、异常、协程和 mapping。
5. `link.myp` 完成 `llc`、`gcc`、runtime 和 CLI 后进入 stage2/stage3。

> **自举源码特性面（实测 2026-08-13，H1 关键输入）**：`tools/selfhost/src/*.myp`（7,568
> 行）真实使用的语言特性仅为：类（`@static`/`@constructor` 共 46 处）、接口（数据载体）、
> enum、泛型（`ArrayList<AstAction>` 等）、数组/切片/字符串、lambda/闭包、元组、struct、
> FFI（`__myp_*`）。**不使用** `@coro`/`await`/`@thread`/`mapping()`/`@gpu`/`@macro`/
> `throw`/异常（匹配均为语言诊断字符串或 AST 数据结构字段）。
> → **H1 自编译只需 G2+G3 核心子集**（类/ARC/泛型/接口/enum/闭包/元组/字符串/FFI）；
> 协程/线程/mapping/异常后端仅当“全语料含 BNCT 运行对拍”（G3 全量验收）需要。建议把
> “H1 自编译最低要求”与“G3 全语料运行对拍”分为两个档位，H1 不必等全后端。
> 另有 45 个语料文件含 `@gpu`——前端无影响，后端走 CPU 回退（G3-7）。

### 4.4 固定验证命令与产物

每个 F4 修改至少执行：

```bash
cmake --build build
bash tests/test_myp_self.sh
```

并生成全语料汇总：tokens/AST 必须保持 448/448，sema 匹配数不得下降；若下降，当前改动不得
进入下一差异簇。泛型阶段额外输出规范实例计数与 dump 事件计数，验证“实体唯一、输出兼容”。

H1 固定产物：

```
build/myp_self    # stage1，由 C++ mypc 编译
build/myp_self2   # stage2，由 myp_self 编译
build/myp_self3   # stage3，由 myp_self2 编译
```

不以二进制文件逐字节相等作为自举成立条件，因为链接器时间戳、符号和布局可能不同；判据是固定
输入上的前端 dump、诊断、生成 IR 语义、程序输出和退出码一致。

---

## 5. 模块设计（`tools/selfhost/src/`）

### 前端（F 系）

| 模块 | 职责 | 依赖 |
|------|------|------|
| `token.myp` | TokenKind + keywordString + Tok | — |
| `lexer.myp` | 完整词法器 | token, text |
| `ast.myp` | AST 节点 + 无损 dump | text |
| `parser.myp` | 顶层/类/语句 | token, lexer, ast |
| `parser_expr.myp` | 表达式 | 同上 |
| `type.myp` | 类型表示 + 比较/提升 | token |
| `diag.myp` | 诊断引擎 | — |
| `sema.myp` | 符号表/类型检查/成员解析/导入 | ast, type, diag, fs, io |

### 后端（G 系）

| 模块 | 职责 | 对应 C++ |
|------|------|----------|
| `ir_emit.myp` | LLVM IR 文本发射器（类型/模块/全局/函数骨架/指令打印） | LLVM C++ API 封装 |
| `codegen.myp` | 顶层/全局/函数 codegen 入口（对齐 `codegen.cpp`） | codegen.cpp（2860） |
| `codegen_expr.myp` | 表达式 → IR（含短路、泛型调用、lambda/闭包、元组） | codegen_expr.cpp（3689） |
| `codegen_stmt.myp` | 语句 → IR（控制流/声明/for-in/parallel/try/coro） | codegen_stmt.cpp（2772） |
| `codegen_class.myp` | 类/构造器/ARC/异常/泛型单态化/mapping/@thread/@startup/FFI → IR | codegen_class.cpp（1960）+ codegen_test.cpp |
| `link.myp` | llc + gcc 链接 runtime / run / fmt 子命令 | main.cpp linkObjects + runFile |

### 驱动

| 模块 | 职责 | 对应 C++ |
|------|------|----------|
| `main.myp` | CLI 解析、子命令分发、编译管线编排、退出码 | main.cpp（1023） |

```
main.myp ─┬─> 前端: lexer → parser → sema（对拍/编译两用）
          └─> 后端: codegen → ir_emit → link.myp（llc/gcc）
```

---

## 6. 输出契约

### 6.1 前端 dump（F0 冻结，带版本头）

- `tokens`：每行 `begin:end kind "value"` + EOF。
- `ast`：缩进树 `(kind)` + 字段；**无损**（源位置/注解/token 原文）。
- `sema`：符号表（排序）+ 诊断（按 file/line/col 排序）。
- 哈希容器 dump 前排序；字符串/浮点转义确定化。

### 6.2 IR 文本（G 系对拍）

- `myp_self --emit-llvm x.myp` 产出 `x.ll`，与 `mypc --emit-llvm` 语义一致。
- 主验收 = 产物运行输出字节一致；.ll 文本对拍为增强（先规范化寄存器名/顺序）。

---

## 7. 验收标准（硬指标）

1. **前端三模式对拍**：全语料（stdlib/examples/tools/tests/BNCTDoseEngine）`tokens/ast/sema`
   vs C++ `--frontend-dump` 字节级一致。
2. **产物运行对拍**：`myp_self compile` 产物 vs `mypc` 直编产物，可编译样本运行输出字节一致
   （含含 `@gpu` 样本走 CPU 回退）。
3. **诊断对拍**：负语料 errorCount + 每条 `file:line:col: msg` 一致。
4. **IR 对拍（增强）**：`--emit-llvm` .ll 规范化后一致。
5. **两级自举（H1）**：stage1/stage2/stage3 对同一语料行为一致。
6. **确定性/性能**：同输入输出确定；`myp_self` 编译耗时 ≤ C++ mypc 10x（目标 ≤3x）。
7. **回归接入**：`tests/test_myp_self.sh` 并入 `run_tests.sh`；`-O0`/`-O2`/ASAN 可跑；
   MYP 侧 ARC 泄漏检查（`Memory.liveObjectCount` 长跑后归零）。

---

## 8. 风险与对策

| 风险 | 对策 |
|------|------|
| IR 文本发射与 C++ LLVM API 语义有细微差异 | 主验收用"产物运行输出"兜底；.ll 对拍先规范化 |
| 总规模 2.37 万行，一次性重写失控 | F/G 各阶段独立对拍闭环；sema/codegen 再按子域切碎 |
| 哈希容器顺序不可移植（T3 教训） | dump/IR 生成前强制排序 |
| 字符串不可变 → lexer/codegen 性能 | T2 已验证可行；先正确后优化；必要时 StringBuilder |
| 递归下降深层嵌套栈深 | 与 C++ 同构先复刻；出问题再迭代/优化 |
| MYP 无继承 → AST/IR 表达繁琐 | kind 枚举 + 扁平化；契约保证对拍 |
| 参考 sema 重复追加同一泛型实例 | 规范实例与 dump 事件分离；codegen 只消费规范实例 |
| 泛型递归实例化或嵌套替换不完整 | registry 状态机阻止重入；统一递归 `substitute` |
| 自举验证引入自依赖环 | stage 验证独立脚本，不进默认 CMake 构建 |
| 链接细节（gc-sections/bridge 探测/缓存 .o） | link.myp 逐项复刻 C++ linkObjects；先直链后优化缓存 |

---

## 9. 里程碑（详见 `roadmap.md`）

| 阶段 | 交付 | 验收 |
|------|------|------|
| **F0** | C++ `--frontend-dump` oracle + 项目骨架 + CMake `myp_self` + 测试脚本骨架 | ✅ 完成 |
| **F1** | 完整词法器 | ✅ tokens 448/448 |
| **F2** | AST + 声明/语句 parser | ✅ AST 448/448 |
| **F3** | 表达式 parser | ✅ AST 448/448 |
| **F4** | sema | ✅ 完成：548/565（正语料全绿） |
| **G1** | IR 文本发射框架 + 最小程序 codegen | ✅ 完成：hello 级产物运行对拍 + .ll 可编译 |
| **G2** | 语句 + 表达式 codegen | 🔄 进行中：控制流/数组/字符串/短路/intrinsic 已对拍 |
| **G3** | 类/ARC/异常/泛型/mapping codegen + 运行时对接 | 完整语料（含 BNCT 子集）产物运行对拍 |
| **G4** | 驱动 + 链接（run/fmt/--stdlib/llc/gcc） | myp_self 全流程可用 |
| **H1** | 两级自举验证 + 全量回归 + 文档 | stage1/2/3 行为一致 |

---

## 10. 待评审决策点

- **R1**：`--emit-llvm` .ll 文本对拍是否要求逐字节（建议：主用运行对拍，.ll 规范化后作增强）。
- **R2**：`fmt` 子命令在 `myp_self` 内联 vs 直接调用 `tools/fmt` 产物（建议：调 `myp_fmt2`，
  避免重复实现）。
- **R3**：`@macro`/`@eval` 是否在 H1 后追加（建议：作为 H1 后续独立里程碑，不阻塞自举闭环）。
  **已实测确认**：自举源码不使用宏（匹配均为注释/语言实现代码），排除不影响 H1。
- **R4**：sema 负语料诊断是否逐字符一致（建议：先 errorCount+行号，再逐条文本）。
