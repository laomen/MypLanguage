# MYP 下一步完善清单（审评稿）

> 状态：**审评中**（v3.9.0 期间归档，2026-08-05）
> 目的：归档本会话识别的"需完善"项，供评审拍板后实施。
> 变更策略：语言规格 v1.0 冻结——凡新增语法 / stdlib 均为 **additive**（非破坏），
> 可在当前规格下陆续引入，无需等待 2.0。

---

## 一、发布收尾（需拍板）

| # | 事项 | 状态 | 待决定 |
|---|------|------|--------|
| 1 | 创建 git tag `v3.9.0` | 暂缓 | 是否创建（v3.8.0 亦无 tag） |
| 2 | 推送 14 个提交到 gitee / GitHub | ✅ 已完成 | 已多次推送（自举/短路/包管理修复等），两远端已同步 |

> 注：稳定性已过 `-O0`/`-O2`/ASAN/TSan/fuzz/多线程 10 次重复/整库 3 连跑（123/123）。
> 若想更充分再打 tag，可先补 §二-1、§二-2。

## 二、测试 / 稳定性强化

| # | 事项 | 说明 | 工作量 | 优先级 |
|---|------|------|--------|--------|
| 1 | **字节级精确 expected** | `run_tests.sh` 用 `$(...)` 捕获会剥尾部换行，导致 `parallel_for`/`coro_thread` 的程序原始输出（末尾多一个空行）与 expected 不精确一致。清掉测试里 `writeFloat`/`writeLong` 后冗余的 `Console.writeString("\n")`（二者自带换行），使 expected 与原始字节精确一致 | 小 | P1 |
| 2 | **并行测试长 soak** | 线程/池类测试（`pool`/`parallel_for`/`barrier`/`coro_thread`/`future`）跑 100 次重复，确认零抖动 | 小 | P2 |
| 3 | **多点模糊测试** | 多 seed 多轮 `fuzz_test.py`，扩大畸形输入覆盖（当前 100 迭代 0 崩溃） | 小 | P2 |

## 三、语言机制增强——类型系统（最高优先）

| # | 事项 | 现状 | 价值 / 影响 | 工作量 | 优先级 |
|---|------|------|-------------|--------|--------|
| 1 | ~~**`Option<T>` / 空安全**~~ | ✅ **已实施（additive）**：stdlib `Option<T>`（`option.myp`，构造器重载 none/some + `isSome/isNone/get/getOr/set/clear`）+ `T?` 语法糖（类型位置 ≡ `Option<T>`；`new` 用显式形式）。显式可空包装消除裸 `null` 解引用；完整"null 解引用编译期报错"为非破坏所限未做。`tests/option` | 消除最大一类运行时错误；系统性最强 | 大 | **P0** |
| 2 | ~~**元组 + 解构**~~ | ✅ **已实施（additive）**：元组类型 `(int, string)` + 字面量 `(1, "x")` + 多值返回 `(A,B) f()` + 声明式解构 `(A a, B b) = f()` / 赋值式解构 `(a, b) = f()` / 嵌套解构 + 字段访问 `t.N`。括号消歧（函数类型 `(A,B)->R` 有 `->`；元组顶层逗号 + `)` 后非 `->`；`(a,b)=>{}` 为 lambda）。`tests/tuple` + 负测试。**待办**：`_` 忽略符、元组方法（v2） | 表达力质变（多值返回/模式解构） | 中 | P1 |
| 3 | **一等函数 / 闭包** | 设计见 `docs/function.md`。**M-FN-1/M-FN-2/M-FN-3 已实施**：函数类型 `(A,B)->R` + lambda 一等函数值（fat pointer `{closure, call_fn}` + tramp）+ 存/传/返/调用 + **按值捕获**（标量/字符串深拷贝、class 引用浅拷贝、嵌套、上下文类型推断）+ **泛型高阶函数**（`mapOpt`/`foldInt`：泛型参数 + 一等函数实参，泛型函数模板与类占位符延迟单态化）+ **`Option.map`** + **泛型 `@static` 类方法**（`List.map<T,R>` stdlib 落位）。`tests/function` + `tests/generic_static` | 高阶函数（map/filter/reduce 可落地） | 中 | P1 |
| 4 | ~~**类型别名 `type X = ...`**~~ | ✅ **已实施**（上下文关键字，仅顶层 `type <Id> = <Type>;` 形态识别，`type` 仍可作标识符；parser 解析时替换 + sema 兜底递归检测；`tests/typealias` 正 + 负） | 可读性 | 小 | P2 |
| 5 | ~~**trait 默认实现 / 关联类型**~~ | ✅ **已实施（additive）**：**默认实现**——接口方法**带默认体** → 实现类可省略，虚表回退默认函数（按类特化 `__ifdef_<Iface>_<method>_<Class>`，`this` 绑定具体类，默认体内 `this.method()`/裸方法调用静态解析到类方法，默认调默认也支持）；类覆盖则用覆盖；纯签名方法仍强制实现（负测试）。**关联类型**——接口 `type Item;` 声明抽象关联类型 + 实现类 `type Item = int;` 绑定（必须绑定，负测试 `assoc_unbound`）；绑定经 `X::Item` 直接引用（局部变量/参数/返回）；泛型 `where T:I` 内 `T::Item` 实例化后单态化为具体绑定（sema：约束类型参数注册为接口类型、`T::Item` 替换、Assoc 通配；codegen：`::` 拦截 + `resolveAssocType` + 类参数 `var_class_map_` 注册使 `c.method()` 精确解析）。`tests/interface_default` + `tests/assoc_types` + 负测试 | 泛型能力 | 中 | P2 |
| 6 | ~~**泛型方法推断**~~ | ✅ **已实施（additive）**：泛型**顶层函数** `T foo<T>(...)`——显式类型实参 `foo<int>(x)` + 实参推断 `foo(x)`（含 `T[]` 元素推断）；按实参单态化（`foo_int_inst`）与泛型类同构；调用点 `<Type,..>(` 用**诊断-free 令牌扫描**消歧（不误伤 `E < energies[mid]` 比较）。泛型方法/静态方法暂缓。`tests/generic_func` | 泛型函数 | 中 | P2 |

## 四、语法 / 表达层

| # | 事项 | 说明 | 优先级 |
|---|------|------|--------|
| 1 | **默认参数 / 命名参数** | `Param ::= Type [Identifier]` 无默认值 | P1 |
| 2 | **for-in / 迭代器协议** | 仅索引式 `for (i=0;...)`，集合遍历靠手写索引 | P1 |
| 3 | **扩展方法** | 无；内建类型只能靠静态类工具函数 | P2 |
| 4 | **多行 / raw 字符串** | 无 `"""..."""` / `r"..."` | P2 |

## 五、机制 / 运行时

| # | 事项 | 说明 | 优先级 |
|---|------|------|--------|
| 1 | ~~**中寿命对象回收：class 实例 ARC**~~ | ✅ **M-ARC-1 已实施（additive，无新语法）**：class 实例自动引用计数——对象头 `{rc:u32, type_id:u32}`（数据指针前 8 字节），`myp_alloc_object/retain/release/free_object` + 每类销毁桩 `__myp_destroy_<Class>`（级联释放类引用字段）+ 按 type_id 分派的 `__myp_release_table`。插桩：作用域退出释放局部类/接口槽（参数/`this` 借用不释放）、函数返回 retain-at-return（借用/新对象均覆盖）、赋值 retain-new/release-old（自赋值安全）、属性存储/静态属性/映射全局 retain、`slice<T>` 类元素 retain/release、`var x = new` 归槽。诊断：`Memory.liveObjectCount()`。`tests/arc`（生命周期/级联/自赋值/借用返回/循环不累积）。**M-ARC-2 待办**：异常/throw-catch 展开释放、`T[]` 数组元素、`@thread`/协程帧释放、与 `@region` 逃逸简化整合。设计见 `docs/arc.md` | **P0** |
| 2 | **同步原语 stdlib** | 只有 `Atomic`/`Barrier`；缺 `Mutex`/`RWLock`/`CondVar`/`Semaphore`/`Once`（pthread 底层已有，低成本） | P1 |
| 3 | **错误类型分层 / `Result<T,E>`** | 只有 `try/catch`，无自定义错误类型体系 / 值式错误传播 | P1 |
| 4 | **反射 / RTTI** | 无运行时类型查询 | P2（远期） |
| 5 | **异步 IO 统一抽象** | `await` 仅限事件，未覆盖文件/网络/睡眠 | P2（远期） |

## 六、平台 / 生态（远期）

| # | 事项 | 说明 | 优先级 |
|---|------|------|--------|
| 1 | **Windows 移植** | 前端已可移植（纯 C++17+LLVM）；难点：`runtime.c` POSIX 子系统（pthread/TLS/termios/socket/regex/process）+ `linkObjects` gcc 硬编码 + DAP 对 gdb/fork/pipe 的依赖 | P2（**用户已明确推迟**） |
| 2 | **包管理器** | ✅ 已完成（T1 v1/v2 + registry/Gitee，见 `docs/pkg_manager.md`） | 已交付 |
| 3 | **文档生成** | 无 doc-comment → API 文档 | P2 |
| 4 | **stdlib 缺口** | crypto/hash、HTTP 客户端、SQL、时区、压缩、`sprintf` 格式化（仅插值）、随机分布 | P2 |
| 5 | ~~**`mypc run`（仿 `go run`）+ 单类文件自动 `main`**~~ | ✅ **已实施（additive）**：①`mypc run file.myp [args]`——编译到临时产物 → 链接 → 直接运行 → 清理，退出码=程序退出码，args 透传。②**单类文件无 `main` 也可 run**：**约束：类必须带 `@startup` 注解**——编译器在 sema Pass 1 后注入合成 `main()`（实例化该类并触发其 `@startup` 入口；`is_auto_main` 标志豁免 main() 直接调用限制）；无 `@startup` / 多 `@startup` 类报错提示。正常编译（非 run）仍要求显式 main（行为不变）。`tests/test_myp_run.sh` 8 断言 | P1 |

## 七、官方 roadmap（design.md §11，已有规划）

- 🔜 Event-driven Pool（方案 B）
- 🔜 自举（用 MYP 实现编译器前端）
- 🔜 JIT
- 🔜 神经形态后端

## 八、本会话已完成的完善项（归档备查）

1. ✅ **线程池完整 API**：`Parallel` 扩展为 `threadCount`/`workerCount`/`workerId`/`isActive`/`setThreads` + 运行时 3 个新函数 + `tests/pool/`（`0465d71`）
2. ✅ **`myp_fmt` 版本号同步** `2.0.0 → 3.9.0`（`96b3061`）
3. ✅ **过时 docs 状态头修订**：slice / operators / metaprogramming / optimization_debugging / exceptions + CHANGELOG v3.8.0 补 P4 条目（`b5235ae`, `8584bfd`）
4. ✅ **稳定性验证矩阵全绿**：-O0/-O2/ASAN 123/123、TSan 12/12 无竞态、fuzz 0 崩溃、pass 6/6、debug 6/6、DAP 15/15、整库 3 连跑、线程测试 10 次稳定
5. ✅ **包管理器 v1（自举）**：`tools/myp.myp` 重写 Python 版（init/build/install/run/legacy）+ runtime 补 `myp_fs_mkdir_p`/`myp_fs_remove_recursive` + `myp_process_run` 改真实退出码（WEXITSTATUS）+ `tests/test_myp_pm.sh`（9 断言）+ 集成进 run_tests.sh（124/124）
6. ✅ **包管理器 v2 形态**：模块化 `tools/pm/*.myp`（main/meta/build/install/util）+ CMake 自定义 target `myp_pm`（build/myp）
7. ✅ **包管理器 v2 功能**：registry（纯 git 子目录）+ `myp.lock` + `add`/`remove`/`update`/`list` + `myp build` 缺失依赖自动安装；`tests/test_myp_pm.sh` 14/14；-O0/ASAN 124/124
8. ✅ **修复运行时 bug**：`myp_io_read_line` 共享缓冲（`static char buf[4096]`）→ 每次返回新分配字符串，EOF 返回空串；补 `Str.toInt`（`myp_str_to_int`）

## 九、自举发现的语言 bug（需修复）

| # | Bug | 现象 | 影响 | 状态 |
|---|-----|------|------|------|
| 1 | ~~**函数返回定长数组共享存储**~~ | 函数返回 `string[N]`（如 `Fs.listDir`/`Str.split`）后，嵌套调用再返回数组会**覆写外层数组内容**（同层连续调用不冲突，跨嵌套边界才触发） | 所有"先取数组再递归"的模式（含 `copyTree`）出错；自举工具已踩中 | ✅ **已修复**（codegen 对返回固定数组栈变量做堆拷贝；`Fs.listDir`/`Str.split` 改动态 `new string[n]` 分配并移除 1024 上限；新增 `tests/array_ret`） |
| 2 | ~~**io 单一全局文件句柄**~~ | `__myp_io_*` 用 `static FILE* myp_io_fp`，不能同时开两个 File | 同时读写两文件互相覆盖（`copyFile` 曾踩中） | ✅ **已修复**（句柄表 + 每 File 持句柄，操作前 `__myp_io_select` 切换；新增 `tests/io_multi`） |
| 3 | ~~**`myp_io_read_line` 共享缓冲**~~ | 原 `static char buf[4096]`，多次 readLine 结果存数组全指向最后一行 | 存多行数组出错（lockfile/registry 踩中）；`tests/io` expected 曾编码旧 bug | ✅ **已修复**（每次返回 `myp_strdup` 新分配，EOF 返回空串） |
| 4 | ~~**stdlib `StringBuilder` 定长越界**~~ | `parts_` 固定 `string[256]` 且 `append` **无边界检查**，超 256 次追加越界写坏堆（段错误） | 长文件/逐字符追加崩溃（T2 格式化器踩中，两个实锤：extractComments 按字符追加、readFile 按行 ×2 追加） | ✅ **已修复**（改 `string[]` 动态扩容，参考 `ArrayList`；`tests/text` 加 1000 片段用例；T2 格式化器已改回用 StringBuilder 并通过全量对拍） |
| 5 | ~~**`@thread` 协程 stdout 缓冲 bug**~~ | `@startup @thread` 协程里 `Console.write(int)`（`printf("%d\n")`）的换行在**后续大分配/`sb.toString()`** 后丢失或行为不稳（首个 write 的 `\n` 有时消失），且随对象布局扰动而变化 | 协程+输出代码输出不稳定；`tests/text` 原 @thread 版在 StringBuilder 扩容改动后暴露 | ✅ **已修复**（根因：`myp_print` 未 `fflush`，滞留缓冲与退出/stderr 竞态；补上 flush 后一致；`coro_throw` expected 更新为新逻辑序） |
| 6 | ~~**`\r`/`\'` 字符串转义缺失**~~ | 字符串字面量不支持 `\r`（需 `__myp_ord(c)==13` 绕）与 `\'` | 转义表不完整；格式化器需 `__myp_chr` 生成 CR | ✅ **已修复**（C++ lexer scanString/scanChar 补 `\r`/`\'`；MYP 自举 lexer 同步） |
| 7 | ~~**格式化器解码字符串转义**~~ | `tokenStr` 直接回放解码后的 value，`"\n"` 被写成真实换行，改变源码语义 | 格式化改变源码（多行串与转义不可分） | ✅ **已修复**（C++ 与 MYP `tokenStr` 统一 re-escape `\n \t \r \\ \" \' \e \0`；`\0` 用 0x01 哨兵绕过 MYP 串 NUL 截断；全语料对拍保持） |
| 8 | ~~**`&&`/`\|\|` 不短路**~~ | 逻辑与/或**不短路**：`a && f()` 在 a=false 时仍调用 f()；`j>=0 && arr[j]==x` 在 j=-1 时仍读 `arr[-1]`（越界） | 依赖短路的代码（越界保护、副作用抑制）出错/崩溃（T3 可视化器排序踩中：`nodes_[-1]` 崩溃） | ✅ **已修复**（codegen 新增 `generateShortCircuitLogic`：`CreateCondBr` + PHI 分支短路——`&&`/`\|\|` 结果已定时不求值右操作数；sema 保证操作数为 bool（i1），GPU kernel 路径保持位运算不变；`tests/short_circuit` 覆盖全部 5 条路径；T3 可视化器 sortNodes 已改回自然 `j>=0 && ...` 写法并通过全量对拍；全回归 -O0 + ASAN 129/129） |

---

## 待评审决策点

- **D1**：是否先做 §二-1（字节级精确）再考虑打 tag？
- **D2**：§三-1（`Option`/空安全）与 §五-1（class 实例 ARC，**已决议**，见 `docs/arc.md`）是否纳入下一里程碑？
- **D3**：§三/§四/§五 各 P1 项的实施顺序（建议：五-1[ARC] → 三-1[`Option`] → 三-3[一等函数] → 五-2[同步原语] → 四-2[for-in] → 三-2[元组]；ARC 与 `Option` 并行不冲突）？
- **D4**：Windows 移植是否正式立项（当前为"后续再说"）？
