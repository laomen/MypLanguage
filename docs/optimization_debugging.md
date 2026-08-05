# MYP 编译后端能力设计：优化管线 + 调试信息

> 状态：**设计草案**（v0.1）—— 尚未实施。
> 目标：让 `-O1/-O2/-O3` 真正生效（IR 级优化）+ 提供 `-g` 调试信息（DWARF，gdb 可用）。
> 两者关联设计：优化会改变代码形态（影响调试），调试通常配合 `-O0`，需统一规划命令行与后端架构。

---

## 1. 现状（问题陈述）

### 1.1 优化级别：名存实亡

- **已实现**：`-O[0123]` 命令行解析（默认 -O0）；传给后端 `TargetMachine::setOptLevel`
  （-O1=Default，-O2+=Aggressive），只影响**后端指令选择**。
- **缺失**：`generate()` 生成 LLVM IR 后**直接** verify → 写对象文件，
  **未运行 IR 优化 pass**（无 mem2reg、instcombine、GVN、内联、循环优化）。

实测 `-O2` IR（`compute` 函数）：
```llvm
define i32 @compute(i32 %n) {
entry:
  %i = alloca i32          ; 变量全是 alloca（无 mem2reg promote）
  %sum = alloca i32
  store i32 0, ptr %sum    ; 冗余重复 store
  store i32 0, ptr %sum
  ...
}
```
→ `-O2` 对性能提升很有限（只靠后端指令选择）。

### 1.2 调试信息：完全缺失

- 无 `-g` 标志；不生成 DWARF（.debug_info/.debug_line）。
- gdb 只能看到函数级断点（符号表），无源码行号、无局部变量。

---

## 2. 命令行设计

```
mypc [-O0|-O1|-O2|-O3] [-g] [-O0] <file.myp> [-o out]
```

| 标志 | 含义 | 默认 |
|---|---|---|
| `-O0` / `-O1` / `-O2` / `-O3` | 优化级别 | `-O0` |
| `-g` | 生成 DWARF 调试信息 | 关 |
| `-g -O0` | 调试构建（推荐组合）| — |

**约定**：
- `-g` 与优化兼容：`-g -O2` 允许（LLVM 支持优化 + 调试），但变量可读性下降。
- 调试构建推荐 `-g -O0`。
- `--emit-llvm` 输出：**优化后**的 IR（打印位置调整到优化管线之后）。

---

## 3. Part A：优化管线（`-O` IR 级优化）

### 3.1 架构

```
parse → sema → codegen(生成 IR) → verifyModule
                                   ↓
                          [优化管线：PassBuilder]
                                   ↓
                              writeObjectFile
```

### 3.2 实现（`writeObjectFile` 或 generate 尾部）

用 LLVM **New Pass Manager** + `PassBuilder`：

```cpp
if (opt_level > 0) {
    llvm::LoopAnalysisManager LAM;
    llvm::FunctionAnalysisManager FAM;
    llvm::CGSCCAnalysisManager CGAM;
    llvm::ModuleAnalysisManager MAM;
    llvm::PassBuilder PB;
    PB.registerModuleAnalyses(MAM);
    PB.registerCGSCCAnalyses(CGAM);
    PB.registerFunctionAnalyses(FAM);
    PB.registerLoopAnalyses(LAM);
    PB.crossRegisterProxies(LAM, FAM, CGAM, MAM);

    llvm::ModulePassManager MPM;
    llvm::OptimizationLevel OL =
        opt_level >= 3 ? llvm::OptimizationLevel::O3 :
        opt_level == 2 ? llvm::OptimizationLevel::O2 :
                         llvm::OptimizationLevel::O1;
    PB.buildPerModuleDefaultPipeline(MPM, OL);
    MPM.run(*module_, MAM);
}
```

各级别管线（`buildPerModuleDefaultPipeline`）：
- **O1**：mem2reg、instcombine、GVN、DCE、内联（基础）、简单循环优化
- **O2**：O1 + 更积极内联、循环展开/向量化、SROA、全局优化
- **O3**：O2 + 更激进内联/向量化（可能增加代码体积）

### 3.3 风险与对策（重点）

MYP 运行时机制与标准优化 pass 的交互，必须逐一验证：

| 风险 | 说明 | 对策 |
|---|---|---|
| **setjmp/longjmp 异常** | LLVM 把 `longjmp` 视为 noreturn；优化 pass 可能移动/合并 try 块控制流 | 协程 trampoline 与异常 handler 的 IR 需专项回归（`tests/exception*`、`coro_throw`）|
| **ucontext 协程** | `swapcontext` 是外部函数，优化 pass 不能跨它移动有副作用代码（LLVM 默认保守）| 回归全部 `tests/coro*` |
| **arena 分配** | `myp_region_alloc` 是外部调用，优化 pass 不会误重排（函数调用边界）| 回归 `@region` 相关测试 |
| **事件/回调** | 间接调用，优化安全 | 回归 `tests/coro_event` 等 |

**通用对策**：全套 109/109 在 **-O0（默认）与 -O2 双级别**跑通；任何语义破坏的 pass 定向禁用或调整管线顺序。

### 3.4 验证

- 全套 `-O0` + `-O2` 双跑（新增 `tests/run_tests_O2.sh` 或给 `run_tests.sh` 加 `-O2` 变体）
- 性能基准：`examples/coro_bench.myp` 在 -O0/-O2 下对比（优化应显著加速）
- IR 检查：`-O2 --emit-llvm` 确认 mem2reg 已 promote（无 alloca 循环变量）

### 3.5 自定义 LLVM pass（MYP 专属优化/变换）

LLVM pass 是开放 API，MYP 可写**自定义 pass** 做 MYP 特定优化或语义变换。
`writeObjectFile` 已有 PassBuilder + NewPM 基础设施（TSan），注册自定义 pass 很自然。

**接口**（New Pass Manager）：

```cpp
// 自定义 FunctionPass（NewPM 标准写法）
struct MyMypPass : public llvm::PassInfoMixin<MyMypPass> {
    llvm::PreservedAnalyses run(llvm::Function& F,
                                llvm::FunctionAnalysisManager& AM) {
        // ...遍历 F 的指令做变换...
        return llvm::PreservedAnalyses::all();   // 未修改 IR
        // return llvm::PreservedAnalyses::none(); // 修改了 IR
    }
};
// 也可写 ModulePass：run(Module&, ModuleAnalysisManager&) 做跨函数/模块级变换
```

**注册进 `-O` 管线**（两种方式）：

```cpp
// 方式 A：直接追加到默认管线之后
PB.buildPerModuleDefaultPipeline(MPM, OL);
MPM.addPass(llvm::createModuleToFunctionPassAdaptor(MyMypPass()));

// 方式 B：注册命令行可调用 pass（-passes="myp-pass"）
PB.registerPipelineParsingCallback(
    [](llvm::StringRef Name, llvm::ModulePassManager& MPM,
       llvm::ArrayRef<llvm::PassBuilder::PipelineElement>) {
        if (Name == "myp-pass") {
            MPM.addPass(llvm::createModuleToFunctionPassAdaptor(MyMypPass()));
            return true;
        }
        return false;
    });
```

**MYP 实用场景**：

| 场景 | 用途 |
|---|---|
| **intrinsic 降级/展开** | 把 `__myp_*` 内部调用按 MYP 语义内联/降级（如高频 intrinsic 优化）|
| **冗余消除** | 清理编译器生成的冗余模式（如重复 `store i32 0, ptr %sum`）|
| **协程/事件语义优化** | 通用 LLVM pass 不识别 ucontext/事件模式，MYP 特定 pass 可优化 |
| **代码形态转换** | 为 GPU/并行做 MYP 特定 IR 变换 |

**注意事项**：
- 遵守 `PreservedAnalyses` 契约（改了 IR 就返回 `none()`，否则后续 pass 用错缓存分析）。
- 不误删/内联 MYP intrinsic（`__myp_*` 是运行时外部函数，除非明确降级）。
- LLVM 21 API（`PassInfoMixin`/`createModuleToFunctionPassAdaptor`/`registerPipelineParsingCallback`）与现有 TSan 用法一致。
- 自定义 pass 改动需双级别（-O0/-O2）全套回归。

---

## 4. Part B：调试信息（`-g` DWARF）

### 4.1 架构

```
codegen 中用 llvm::DIBuilder 生成调试元数据（随 IR）
  → DWARF 段（.debug_info/.debug_line/.debug_loc）
  → gdb：break file.myp:N / print x / next/step
```

### 4.2 DIBuilder 集成点（codegen.cpp）

| 元素 | DIBuilder API | 说明 |
|---|---|---|
| 编译单元 | `createCompileUnit(DW_LANG_C, file, producer, ...)` | 每 TU 一个 |
| 源文件 | `createFile("foo.myp", dir)` | 用 SourceManager 路径 |
| 函数/方法 | `createFunction(...)` → DISubprogram | 关联到 LLVM Function（`setSubprogram`）|
| 局部变量 | `createAutoVariable` + `insertDeclare(alloca, ...)` | dbg.declare 关联 alloca |
| 参数 | `createParameterVariable` + `insertDbgValueIntrinsic` | 入口参数（arg 0/1...）|
| 行号 | `builder_.SetCurrentDebugLocation(line, col, scope)` | 每条语句生成前，用 `stmt.range` 的 SourceLocation |
| 作用域 | 函数级（DISubprogram）| V1 简化，不做嵌套 DILexicalBlock |

### 4.3 类型映射（DIType）

| MYP | DWARF |
|---|---|
| `int` | DW_ATE_signed（32）|
| `long` | DW_ATE_signed（64）|
| `double` / `float` | DW_ATE_float |
| `bool` | DW_ATE_boolean |
| `char` | DW_ATE_unsigned_char |
| `string` | 指针 → i8 |
| `class` / `struct` | DICompositeType（成员 DIDerivedType）|
| 数组 / slice | DICompositeType（子范围）|

### 4.4 协程调试（已知限制）

- 协程用 `ucontext swapcontext`（自定义栈 + 长跳转）——**gdb 单步跨 `await` 可能无法正确
  跟踪协程栈**（调试器不识别 ucontext 栈切换）。
- **V1 范围**：只保证普通函数/方法调试；协程内调试列为已知限制（文档说明，用日志辅助）。
- 远期（若需）：运行时注册协程栈为调试器可见栈（复杂）。

### 4.5 验证

- gdb **batch 模式**自动化：`gdb -batch -ex 'break foo.myp:N' -ex run -ex 'print x' ./out`
- 新增 `tests/debug/`（编译带 `-g` 的程序 + gdb 脚本断言断点/变量）
- 若 CI 无 gdb：退化为 `readelf --debug-dump` 检查 DWARF 段存在 + 行号表

---

## 5. 里程碑规划

| 里程碑 | 内容 | 验收 |
|---|---|---|
| **M1** | `-O1/-O2/-O3` PassBuilder 管线 | ✅ 已完成（`writeObjectFile` 加 `buildPerModuleDefaultPipeline`；修复 setjmp `returns_twice` + `myp_throw` 误标 noreturn 两个优化暴露的 bug；`tests/run_tests_O2.sh` -O2 全套 109/109；`MYPC_DUMP_OPT_IR=1` 调试开关）|
| **M2** | 优化 × 异常/协程兼容专项回归 + 修复 | ✅ 已完成（`tests/exception*`、`tests/coro*` 在 -O2 专项回归全部通过；性能基准：`-O2` 比 `-O0` 至少 45×（45ms→<1ms），结果一致；`-O0`/`-O2` 全套 109/109）|
| **M3** | `-g` DIBuilder：编译单元/文件/函数/行号 | ✅ 已完成（`main.cpp` 加 `-g/--debug` 全链路传参；`CodeGen` 加 `debug_mode_` + DIBuilder；`generateFuncDecl`/`generateClassAction`/`generateStaticAction` 建 DISubprogram + `setSubprogram`；`generateBlock` 逐语句 `SetCurrentDebugLocation`；gdb `break foo.myp:N` 命中验证）|
| **M4** | `-g` 局部变量 + 参数（dbg.declare）| ✅ 已完成（参数用 `createParameterVariable` + `insertDeclare`；局部变量在 `popScope` 集中 `createAutoVariable` + `insertDeclare`；`debug_declared_` 去重防参数/局部重复；gdb `print a/b/sum/x/y` 正确验证）|
| **M5** | `-g` 类型细化（class/struct/数组）| ✅ 已完成（`getDebugType(LLVM Type→DIType)`：int/long/double/float/bool/char→DIBasicType；string/类实例→DIDerivedType 指针；struct→DICompositeType + 成员（DataLayout 偏移）；数组→DICompositeType + DISubrange；gdb `print c`（类实例）验证）|
| **M6** | 自定义 MYP pass（冗余消除 / intrinsic 优化）| `-passes="myp-pass"` 可调用；双级别回归通过 |

每阶段独立可验证：构建（正常 + ASAN）+ 全套测试 + no-crash 回归。

---

## 6. 关键决策

1. **优化默认级别**：保持默认 `-O0`（正确性优先、调试友好）；用户显式 `-O2` 获得性能。
2. **优化管线**：用 New Pass Manager（`buildPerModuleDefaultPipeline`），与 TSan 的
   PassBuilder 用法一致（`writeObjectFile` 已有先例）。
3. **调试与优化**：`-g` 独立于 `-O`；调试构建推荐 `-g -O0`，`-g -O2` 也支持（变量可读性降级）。
4. **协程调试**：V1 明确为已知限制，不投入调试器栈注册（复杂度高、收益低）。

---

## 8. 后续扩展（还缺少的部分）

### 8.1 DAP 调试协议（VS Code 调试，最高价值）

`-g` 只让 gdb/lldb 可用。MYP 已有 LSP + VS Code 扩展，但**无 DAP**（Debug Adapter Protocol），
IDE 内无法设断点/单步。规划：

- 新增 DAP 代理（对接 gdb/lldb），实现 VS Code 断点、单步、变量查看。
- 复用 `-g` 生成的 DWARF；`sourceFile` 映射到 `.myp`。
- 里程碑：M7（DAP 基础：launch/断点/continue/next）。

### 8.2 质量保障

| 项 | 说明 |
|---|---|
| `tests/run_tests_O2.sh` | `-O2` 全套回归脚本（优化管线质量保障）|
| 性能基准套件 | 编译时间 + 运行性能矩阵（-O0 vs -O2），不止 `coro_bench` 单例 |
| 自定义 pass 测试框架 | IR 级单元测试（`opt` 风格输入/输出断言）|

### 8.3 进阶优化（低频）

- **PGO**（profile-guided optimization）：profile 反馈优化，MYP 无 profile 采集机制，低频。
- **编译时间预算**：`-O2` 编译耗时基准，防止优化导致编译显著变慢。

### 8.4 工程

- **manual 用户文档**：`-O`/`-g` 使用说明（`manual.md` 已列 `-O2`，补 `-g` 与调试章节）。
- **多文件优化说明**：MYP 多文件本就合并成**单模块**编译，天然享受跨文件优化
  （**无需 LTO**，这是架构优势，文档注明）。

---

## 9. 参考

- LLVM PassBuilder / OptimizationLevel（LLVM 21）
- LLVM DIBuilder（`llvm/IR/DebugInfo.h`）
- 现有 TSan PassBuilder 用法：`src/codegen/codegen.cpp` `writeObjectFile`
- 版本策略：`docs/CHANGELOG.md`（`-O`/`-g` 为编译器特性，不影响语言规格）
