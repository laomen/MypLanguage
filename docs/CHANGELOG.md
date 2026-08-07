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

### v3.9.0（当前）
- **反射 / RTTI（§五-4，additive）**：class 对象头 `{rc, type_id}` 自带运行时类型 id。
  新增 `stdlib/rtti.myp` 静态类 `Rtti`——`typeOf<T>(obj)`（运行时类名）、
  `typeId<T>(obj)`（运行时类型 id）、`sameType<T,U>(a,b)`（同类型判定；null → id 0 / 空名）。
  实现：codegen 并列生成 `__myp_type_name_table`（type_id → 类名，ExternalLinkage 常量表，
  无类 TU 退化为 `[null]`）；新内建 `__myp_type_id`/`__myp_type_name`（sema 注册 +
  `intrinsic_map_` 解析）→ runtime `myp_obj_type_id`/`myp_obj_type_name`。`tests/rtti`。
  - **§三 类型系统增强（additive）**：
  - **类型别名 `type X = ...`**：上下文关键字（仅顶层 `type <Id> = <Type> ;` 形态），
    别名可在任何类型位置使用、支持别名套别名、递归别名编译报错。`tests/typealias` + 负测试。
  - **`Option<T>` 可空容器 + `T?` 语法糖**：stdlib `option.myp`（`Option()`=none /
    `Option(T v)`=some + `isSome/isNone/get/getOr/set/clear`）；`Type?` ≡ `Option<Type>`
    （类型位置）。`tests/option`。
  - **泛型函数 `T foo<T>(T x)`**：显式类型实参或实参推断（含 `T[]` 推元素类型）；
    按类型实参单态化（`foo_int_inst`），模板不生成运行时代码。`tests/generic_func`。
  - **一等函数与闭包（M-FN-1/2/3）**：函数类型 `(A,B)->R`（胖指针 `{closure, call_fn}` +
    统一 tramp）；lambda 按值捕获（标量/字符串深拷贝、class 引用浅拷贝、嵌套、上下文类型
    推断）；命名 lambda `fn name(...) =>` 递归；泛型高阶函数 `mapOpt`/`foldInt` + `Option.map`。
    设计见 `docs/function.md`；`tests/function`。
  - **元组 + 解构（TUP-1/2）**：元组类型 `(int, string)` + 字面量 + **多值返回** +
    声明式/赋值式/嵌套解构 + 字段访问 `t.N`；与函数类型/lambda/元组变量声明四路消歧。
    设计见 `docs/tuple.md`；`tests/tuple` + 3 负测试。
  - **泛型 `@static` 类方法（M-FN-3 stdlib 落位）**：`static:` 段方法名后带类型参数
    （`List.map<T,R>`），跨模块可见；`resolveGenericStaticCall` 单态化到 `tu.functions`
    （`__gs_<Class>_<method>_<types>_inst`）。`tests/generic_static`。
  - 全库回归：141/141（`-O0` + ASAN）。
  - **trait 默认实现（§三-5，additive）**：
  - 接口方法**带默认体** → 实现类可省略该方法，虚表回退默认函数；类覆盖则用覆盖。
  - 实现：按类特化默认函数 `__ifdef_<Iface>_<method>_<Class>`（预声明 + generateClass 生成），
    `this` 绑定具体类 → 默认体内 `this.method()`/裸方法调用**静态解析到类方法**（含默认调默认）；
    sema `checkInterfaceImpl` 放行带默认体的接口方法，纯签名方法仍强制实现（负测试）。
  - `tests/interface_default`（默认/覆盖/this 分派/默认调默认）+ 负测试。
  - 全库回归：144/144（`-O0` + ASAN）。
  - **关联类型（§三-5，additive）**：接口 `type Item;` 声明抽象关联类型，实现类
    `type Item = int;` 绑定（**必须绑定**，负测试 `assoc_unbound`）；接口方法参数/返回
    引用关联类型；绑定经 `X::Item` 直接引用（局部变量/参数/返回类型）；泛型
    `where T:I` 内 `T::Item` 实例化后单态化为具体绑定（约束类型参数注册为接口类型、
    `T::Item` 替换、Assoc 通配；codegen `::` 拦截 + `resolveAssocType` + 类参数
    `var_class_map_` 注册使 `c.method()` 精确解析到具体实例类）。
  - `tests/assoc_types`（基本引用 + 泛型 T::Item + 关联类型参数/返回）+ 负测试。
  - 全库回归：146/146（`-O0` + ASAN）。
  - **class 实例 ARC（§五-1，M-ARC-1，additive 无新语法）**：自动引用计数回收中寿命对象。
    - 对象头 `{rc:u32, type_id:u32}`（数据指针前 8 字节）；`myp_alloc_object/retain/release/
      free_object`；每类销毁桩 `__myp_destroy_<Class>`（级联释放类/接口引用字段）+ 按
      type_id 分派的 `__myp_release_table`（ExternalLinkage，运行时按需读取）。
    - 插桩：作用域退出释放局部类/接口引用槽（参数/`this` 借用不释放）；函数返回
      **retain-at-return**（借用返回/新对象均覆盖，调用方转移接管）；赋值 retain-new
      （fresh new/call 转移）release-old（自赋值安全）；属性存储（`this.prop`/裸 `prop`/
      静态属性/映射全局）retain；`slice<T>` 类元素 retain/release；`var x = new` 归槽。
    - 修复潜在 bug：类结构体**两遍构建**（自/交叉类属性引用解析为 ptr，原为 i32 致
      指针字段损坏，ARC 测试暴露）；`myp_release` free 前缓存 rc（原 free 后读头 → UAF）。
    - 诊断：`Memory.liveObjectCount()`（当前线程存活实例数）。
    - `tests/arc`（生命周期/级联/自赋值/借用返回/循环不累积）；全库回归 147/147（-O0+ASAN）。
    - **M-ARC-2（2026-08-06）**：`T[]` 数组元素 retain/release（局部/this.arr/obj.arr，
      slice 同）；语句末临时释放（`new` 作实参/丢弃不累积，强槽 store 消费）；`return new T()`
      转移（跳过 retain-at-return，修 M-ARC-1 fresh-return 泄漏）；函数 epilogue release
      （修 return 结尾局部泄漏）；`@thread`/`@threadpool` 实例在 `myp_thread_destroy` 释放
      startup_arg（线程池改 `myp_alloc_object` 带头）。修复：lambda 闭包/`@thread` 实例
      临时消费（ASAN 捕获的过早释放）；emitFunctionReturn 顺序（retain 先于 release、
      main 的 release 先于 `myp_free_all`）。`tests/arc_m2`；全库回归 148/148（-O0+ASAN）。
    - **M-ARC-3（2026-08-06）**：**闭包释放**——函数值局部注册为 ARC 槽（fat pointer
      index 0 = 闭包），作用域退出释放；`LambdaExpr` 视为 fresh（闭包是新分配的 class
      实例）；别名赋值 retain 闭包；捕获的 class 引用在 `generateLambda` **retain**（闭包
      拥有自己的引用，销毁桩级联释放平衡——修外层局部释放后闭包持悬垂借用的 UAF）。
      `tests/arc_fn`；全库回归 149/149（-O0+ASAN）。
    - **剩余**：异常/throw-catch 展开释放（v1 泄漏安全，`myp_free_all` 兜底）、协程帧
      释放、`@region` 逃逸精修。设计见 `docs/arc.md`。
    - **同步原语 stdlib（§五-2，additive）**：`sync.myp`——`Mutex`（普通+可重入
      `PTHREAD_MUTEX_RECURSIVE`，tryLock）、`RWLock`（pthread_rwlock，try rd/wr）、
      `CondVar`（wait 关联 Mutex handle + signal/broadcast）、`Semaphore`（POSIX sem_t，
      tryWait）、`Once`（enter/done call-once 惯用法）。全部 handle 模式（同 `Barrier`，
      固定数组+分配表+槽位互斥，每类 64 槽）。`tests/sync`（4 `@thread` worker 用 Mutex
      保护 `@static` 共享计数 400 确定性 + CondVar 生产者/消费者 + tryLock/递归/RWLock/
      Semaphore/Once API 检查）；全库回归 150/150（-O0 + ASAN）。
  - **for-in / 集合迭代（§四-2，additive）**：`for (x in coll)` / `for (T x in coll)` /
    无括号 `for x in coll`。四种可迭代源：**固定数组** `T[N]`（编译期长度）、**slice**
    `slice<T>`（`.size()`）、**集合类**（需 `size()`+`get(int)`，de-facto 迭代器协议，
    如 `ArrayList<T>`）、**range** `for (i in a..b)`（右开 `i<b`，等价索引式 for）。
    循环变量每次迭代声明（作用域级），`break`/`continue`/嵌套支持；迭代源只求值一次。
    ARC：类迭代集合适用临时持有 + 循环末释放；类元素（数组/slice 下标借用→retain、
    类 get 结果 fresh 转移）循环变量作用域末释放，实测零泄漏。负例：动态数组 `T[]`/
    非可迭代类/非集合类型/集合元素为数组均编译期报错。`tests/for_in`；全库回归
    151/151（-O0 + ASAN）。
  - **默认参数 / 命名实参（§四-1，additive）**：`Param ::= Type Identifier ('=' Expression)?`
    默认参数——顶层函数/类方法（action:/function:）/静态方法/构造器/struct 构造；事件与
    枚举数据字段不允许默认值。调用点 `f(name = value)` 按赋值表达式解析，sema 按「目标
    标识符匹配形参名」重解释为命名实参（乱序可用；与宏的赋值实参 `$n/$body` 无歧义——
    宏参数名永不匹配普通标识符）。位置实参按序填前 N 形参、命名按名填入、缺失且有默认值
    者克隆默认表达式在调用点求值；声明期校验默认值类型兼容。泛型函数/泛型静态方法亦支持
    （实例参数克隆保留默认值 + 实参规范化）。负例：未知/重复命名、位置+命名重叠、必填
    缺失、实参过多、默认值类型不匹配。`tests/defarg` + 6 负测试；全库回归 158/158（-O0+ASAN）。
  - **值式错误传播 Result<T,E>（§五-3，additive）**：`stdlib/result.myp`——`Result<T,E>`
    二态容器（`Result()`=err / `Result(T v)`=ok + `isOk/isErr/get/getErr/getOr/setOk/setErr`）；
    顶层泛型工厂 `resultOk/resultErr`（部分类型实参推断）；组合子 `resultMap/resultAndThen/
    resultMapErr`（无异常错误传播）；异常桥 `resultTry<T>((() -> T) f) -> Result<T,string>`
    （`catch (string s)` 优先 + `catch (Error e)` 用 `e.message()`）。`error.myp` 补
    `StringError.setMsg`。`tests/result`；全库回归 159/159（-O0+ASAN）。
    **附带修复 5 个既有 bug**：①lambda 捕获分析误捕全局函数/类名（`collectExprCaptures`
    加 `isGlobalName` 过滤，lambda 内可直接调用顶层函数）；②`string + bool` 拼接把 i1 传
    给 `myp_to_string_i32`（改 `myp_to_string_bool` + sext）；③泛型函数内 `new G<T>(args)`
    带参构造不调用构造器（codegen 按具体实例类名 + 实参个数重建 ctor）；④接口值来自函数
    返回值赋给接口变量时把胖指针当实例指针（interface passthrough）；⑤`catch (string)`
    绑定共享 `myp_error_msg` 缓冲、后续 throw 覆写导致存下的错误消息漂移（绑定前
    `myp_strdup` 拷贝——`resultTry` 多连调用即踩中）。
  - **stdlib 缺口补强（§六-4，additive）**：
    - **crypto/hash**：`stdlib/crypto.myp`——`Crc32.crc32/crc32Hex`（IEEE 802.3，
      原始 32 位值 + 8 位无符号十六进制显示）+ `Hash.md5/sha1/sha256`（小写十六进制）。
      核心在 C 运行时（`myp_crc32`/`myp_hash_md5`/`myp_hash_sha1`/`myp_hash_sha256`，
      MYP 侧静态类封装）。已知测试向量回归（含 56 字节跨块消息）。
    - **sprintf 格式化**：`stdlib/fmt.myp`——`Fmt` 静态类（默认参数）：`i`（十进制）、
      `u`（无符号十进制）、`x/X`（无符号十六进制大小写）、`o`（八进制）、`b`（二进制）、
      `f/e/g`（定点/科学/最短，精度）、`s/sR`（字符串左右对齐），全部支持宽度 + 填充字符。
      运行时补 `myp_fmt_u64_base`/`myp_fmt_double_f/e/g`（snprintf）。
    - **随机分布补强**：`random.myp` 加 `range(lo,hi)`（[lo,hi) 均匀）、`exponential(lambda)`
      （逆变换采样）、`poisson(lambda)`（Knuth 算法）。
    - **HTTP 客户端**：`stdlib/http.myp`——基于 `net.myp` TCP 的 HTTP/1.1 客户端
      （仅 `http://`，无 TLS）：`Http.get/post/request` + `HttpResult`（status/body/
      header 大小写不敏感/isOk）。URL 解析（scheme/host/port/path/query）；响应状态行/
      头解析；`Content-Length` 定长体、`Transfer-Encoding: chunked` 分块体、关闭定界体。
      连接失败抛 NetError、非法 URL/非 http scheme 抛 string。自包含测试：本地
      `TcpServer` 响应线程（@thread）验证 GET/404/chunked/POST/scheme 校验。
    - `tests/crypto` + `tests/fmt` + `tests/random_dist` + `tests/http`；全库回归
      **166/166**（-O0+ASAN）。
  - **`mypc run`（仿 `go run`）+ 单类文件自动 `main`（additive）**：
  - `mypc run file.myp [args]`：编译到临时产物 → 链接 → 直接运行 → 清理；退出码=程序
    退出码；args 透传（`main(argc,argv)`/构造器）。
  - **单类文件无 `main` 也可 run**：sema Pass 1 后注入合成 `main()`（实例化类并触发其
    `@startup` 入口）；`FuncDecl.is_auto_main` 豁免 main() 直接调用限制；无 `@startup` /
    多 `@startup` 类编译报错。正常编译（非 run）仍要求显式 main。
  - `tests/test_myp_run.sh` 8 断言；全库回归 142/142（`-O0` + ASAN）。
- **类构造器（M1-M4，additive）**：`@constructor` 注解 + **函数名==类名隐式构造器**。
  - M1 语法 + AST：`parseActionDecl`/`parseFunction` 识别 `@constructor`；`@constructor`
    构造器**无返回类型**（`@constructor Window(...)`，不写 `void`）且名称**必须==类名**
    （编译校验）；类/struct 中方法名==类名 → 隐式构造器（可省略注解，C++/Java 风格）；
    struct 新增 `action:` 节（方法/属性前瞻区分）。
  - M2 class 构造器绑定：`new C(args)` 优先绑定构造器（重载解析 + 数字提升
    `int → long → double`，歧义/无匹配编译报错）；泛型 `new Box<double>(1.5)` 绑定
    单态化实例类的构造器（根治 `@startup` 泛型分发 bug 的根因）；构造器重载
    mangling（`Class_Action_<paramtypes>`）；legacy `@startup init` 回退保留。
  - M3 struct 构造器：函数式构造 `Struct(args)`（栈临时 + 构造器；声明初始化/赋值/
    `return Struct(...)`/与 `operator:` 共存）。
  - M4 `copy()` 深拷贝约定（纯约定方法，无新语法）；manual/design/grammar/CHANGELOG 同步。
  - 修复既有 bug：`generateMemberAccess` 对类实例局部变量的 `.property` 访问错误返回
    实例指针（`c.data_[i]` 写入实例内存）；`checkStructMethods` 排除构造器为兄弟方法
    （其名==struct 名遮蔽 struct 类型名）。
  - 设计见 `docs/constructor.md`；测试 `tests/constructor/` + `tests/copy/`。
- **@startup → 构造器迁移（不留 legacy）**：
  - stdlib 8 文件（`json`/`fs.Path`/`logger`/`net.TcpServer`/`net.TcpClient`/`regex`/
    `text.StringBuilder`/`time.Timer`）+ `stream`×3 与 `layers`×2 空占位删除。
  - 41 个测试 + examples + docs/examples + deeplearning + BNCT 的 legacy `@startup`
    （非 @thread）→ `@constructor`；`tests/startup` 重写为新语义（构造器 + @thread 线程入口）。
  - **移除** codegen 中 `new C(args)` 自动调 `@startup` 的 legacy 绑定；
    `@startup` 严格只作启动信号（@thread 线程入口）。
  - 验证：`-O0`/ASAN 全套 121/121。
- **`@parallel for` 多线程检测 + 线程池 API 扩展**：
  - 运行时新增 TLS `myp_pool_worker_id()`（当前 worker 索引，`@parallel for` body 内
    返回 0..N-1，非池线程 -1）；`emitKernelExpr` 支持其直接调用；
    `tests/parallel_for/` 用 `Parallel.workerId()` 检测多线程真正启动。
  - 池运行时新增 `myp_pool_worker_count()`（实际 worker 数）、`myp_pool_is_active()`
    （是否初始化）、`myp_pool_set_threads(n)`（池大小，首次创建前生效，0=自动）；
    `myp_pool_init_global` 遵循设定的线程数。
  - `stdlib/pool.myp` FFI 集中于此文件，`Parallel` 静态类扩展为完整查询/配置 API：
    `threadCount()`/`workerCount()`/`workerId()`/`isActive()`/`setThreads(n)`。
  - 新增 `tests/pool/`（`setThreads(2)` 确定性断言池大小、worker 索引区间、池内外 workerId）。
  - 验证：`-O0`/ASAN 全套 123/123。

### v3.8.0
- **切片类型 `slice<T>` + `@region` 两级 arena（P4，additive）**：
  - P4a `slice<T>` 值类型：`{ T* data; int64 len }` fat pointer；`new slice<T>(n)`、
    `s[i]`（运行时边界检查）、`.size()`/`.length()`、值传递/返回（逃逸→进程级）、
    浅拷贝共享 data、空 slice；grammar v1.0 列为内置类型。
  - P4b 两级 arena + `@region` 注解：region-local arena（`emitRegionEnter` + TLS 追踪），
    `@region` 函数退出时回收本 region 分配，返回引用类型时逃逸到上层。
  - P4c slice 集合二元：`@op("+")`/`@op("*")` 元素级运算 + 标量广播（A+B / A*k / k*A）。
  - P4d grammar 增量（`slice<T>`/`@region` 入规格）+ region 动态作用域（嵌套/逃逸规则）。
  - 测试：`tests/slice/`、`tests/slice_binop/`、`tests/slice_more/`、`tests/region/`、
    `tests/region_chain/`。设计见 `docs/slice.md`；算子见 `docs/operators.md` P4。
- **集合动态扩容**（`stdlib/collections.myp`）：`ArrayList`/`HashMap`/`Set`/`Deque`/`Queue`/
  `Stack`/`PriorityQueue`/`LinkedList`/`StrHashMap` 全部突破固定 1024 上限。
  - 惰性分配（首操作时 `new T[cap_]`）+ 容量翻倍扩容；哈希类 75% 负载因子翻倍重建；
    环形缓冲 grow 重排；LinkedList 节点池翻倍。
  - 规避 `@startup` 泛型分发 bug（`new Box<double>()` 曾误调模板 init 致堆损坏）。
- **泛型 `new T[n]` 端到端支持**（additive，新语言能力）：
  - parser：`new Ident[n]` → 动态数组；局部声明歧义消除补 `Ident [] name`。
  - codegen：单态化类型参数映射（`current_type_params_`），`new T[n]` 用真实元素类型分配。
  - 支持 `new Foo[n]`（类数组）与 `new T[n]`（泛型参数）。
- **`function:` 段跨方法调用修复**：sema 声明类作用域符号表 + codegen 预声明函数符号，
  方法可调用段中任意位置的方法（此前只能调用靠前的）。
- **LSP 解析错误恢复死循环修复**（内存爆炸根因）：
  - `parseBlock`/`parseMapping` 的 body 循环加"必须前进"保证（`current_` 未变则强制 `advance`），
    畸形输入（如函数体内孤立 `class`、`mapping =>`）不再无限循环耗尽内存（曾致 `myp_lsp`
    膨胀至 35GB）。
  - 模糊测试 5 seed × 600 例随机畸形输入 = 3000 例 0 挂起；全 stdlib 31 文件 LSP 实测 4MB。
- **`stdlib/memory.myp` 修复**（预存坏文件）：`ffi void*` 语法不支持 → 改用 MYP 惯例
  **指针以 `long` 承载**（同 json/regex 的 handle）；类名 `Mem` → `Memory`（对齐 manual）；
  移除基于不存在的 `__myp_ptr_write/read` 的 `DynamicArray`（动态数组统一走
  `collections.ArrayList<T>`）。`Memory` 提供 alloc/free/realloc/release（FFI 裸内存 +
  确定性释放）。新增 `tests/memory/`。
- 验证：`tests/collections_grow/`（全集合超 1024）；`-O0`/`-O2` 全套 115/115；ASAN 115/115。

### v3.7.0
- **DAP 调试支持（M7）**：`src/dap/dap_server.cpp` → `myp_debug`（DAP ↔ gdb MI2 桥）。
  - 支持：initialize/launch/setBreakpoints/configurationDone/continue/next/stepIn/
    stepOut/threads/stackTrace/scopes/variables/evaluate/pause/disconnect。
  - 复用 `-g` DWARF：VS Code 设断点/单步/查局部变量。
  - VS Code 扩展 `vscode-myp` 注册 `myp` 调试器（`myp.debuggerPath` 配置 + 自动探测）。
  - 验证：`tests/test_dap.py`（launch→断点→stopped→栈/局部变量/evaluate，15 断言）；
    `-O0`/`-O2` 全套 114/114。
- **文档手册完善**：`docs/manual.md` 第 12 章补完整命令行选项表、优化（`-O`/`--passes`）、
  调试（`-g`/gdb）、元编程（`@eval`/`macro`/`@macro`+`quote`）章节。

### v3.6.0（当前）
- **元编程 M4：过程宏 `@macro` + `quote`**（`src/eval/eval.cpp` 解释器扩展 + `src/macro/macro_expand.cpp` 集成）。
  - `@macro` 注解修饰函数（sema 跳过 body / codegen 不生成）；`quote { ... }` 上下文关键字
    （仅 `quote {` 识别为 AST 模板，`char quote = ...` 变量不受影响）。
  - 解释器 `EvalValue` 加 AST 值类型：`StmtList + StmtList` 拼接、`quote` 求值、`$x` 插值
    （数值→字面量、字符串→标识符（变量名 `int $name = ...` / 赋值目标 `$x = ...`）、AST→内联）。
  - 展开 pass：`@macro` 调用编译期执行（`evalProcMacro`）→ AST 替换；`main.cpp` Phase 3b
    对 `@macro` 也触发展开（修复：`ast.macros` 空但含 `@macro` 时未展开的 bug）。
  - 验证：`tests/proc_macro/`（`genAssign("x",42)` → `int x = 42`；`makeCalls(3)` → 3 条
    `Console.write(0/1/2)`）；`-O0`/`-O2`/ASAN 全套 114/114。
  - 设计见 `docs/metaprogramming.md` §5。

### v3.5.0（当前）
- **元编程 M3：声明式宏 `macro`**（`src/macro/macro_expand.cpp` + `include/mylang/Macro.h`）。
  - `macro name($a, $b) { ... }` 顶层声明；`$param` 为模板占位（新 token `Dollar` + `Keyword_macro`）。
  - 宏体是普通 MYP 块；展开为 AST 深拷贝 + `$param` → 实参 AST 替换（parse 后、sema 前）。
  - 支持表达式参数（`log($a)`）、语句参数（`repeat($n, $body)`）、赋值参数（`$x = ...`）、
    嵌套宏（`twice(addN(v,10))`，迭代展开 + 深度上限）。
  - `--macro-expand`：展开后 AST dump 调试。
  - 验证：`tests/macro/`（repeat+addN+twice+log → v=37）；`-O0`/`-O2` 全套 113/113。
  - 设计见 `docs/metaprogramming.md`。

### v3.4.0（当前）
- **元编程 M1：`@eval` 编译期求值**：`src/eval/eval.cpp` 轻量 MYP 解释器。
  - `@eval` 标记纯函数（标量/递归/条件/循环/`@eval` 互调/const 引用），编译期执行。
  - 顶层 `const int X = <expr>` 折叠：`const int FIB10 = fib(10)` 生成 `ret i32 55`。
  - 求值器在 sema 后、codegen 前运行（`main.cpp` Phase 4b）；非折叠初始化保持运行时行为。
  - 验证：`tests/eval/`（FIB10=55/FIB20=6765/HALF=2.5/BIG=true/T5=165/BIGL=1000000）。
- **元编程 M2：泛型约束 `where T : Interface`**：
  - 语法 `<T where T : Shape>`；sema monomorphization 时检查类型参数实现接口。
  - `DrawList<Circle>` 通过、`DrawList<int>` 编译期报错；`tests/generic_constraint/` + negative。
  - 验证：`-O0`/`-O2`/ASAN 全套 112/112。
  - 设计见 `docs/metaprogramming.md`。

### v3.3.0（当前）
- **自定义 LLVM pass（M6）**：`src/codegen/myp_passes.cpp` + `include/mylang/MypPasses.h`。
  - `MypRedundantStorePass`（FunctionPass）：消除同基本块内相邻同址死 store
    （如 `int x = 0;` 生成的双 store，实测 -O0 IR 每个变量多一次重复 store）。
  - `mypc --passes myp-pass` 可调用（`runMypPasses` 直接分发）；未知 pass 名报错。
  - `registerMypPasses`：追加到 -O 管线末尾 + 注册 `-passes` 解析回调（opt 风格嵌套）。
  - LLVM 21 备注：`parsePassPipeline` 顶层不解析自定义 pass 名（需嵌套 `module(...)`），
    故 `mypc --passes` 走自建分发。
  - 验证：`tests/test_myp_pass.sh`（6 项断言：可调用/语义/死 store 10→7/-O2 组合/未知拒绝）；
    `-O0`/`-O2` 全套 109/109。
  - 设计见 `docs/optimization_debugging.md` §3.5/M6。

### v3.2.0
- **DWARF 调试信息（M3-M5）**：`-g/--debug` 生成 gdb 可用调试信息。
  - M3：编译单元/文件/函数 DISubprogram + 逐语句行号（`break foo.myp:N` 命中）；
    `main.cpp` 全链路 `-g` 传参（单文件/多文件）。
  - M4：参数（`createParameterVariable`）+ 局部变量（`popScope` 集中 `dbg.declare`）
    变量映射（gdb `print a/b/sum/x/y` 正确）。
  - M5：类型映射（int/long/double/float/bool/char/指针/struct 成员/数组子范围）。
  - 验证：`tests/test_debug.sh`（gdb 批处理 6 项断言）+ `-g -O0`/`-g -O2` gdb 实测；
    `-O0`/`-O2` 全套 109/109、ASAN 109/109。
  - 说明：LLVM 21 已移除 `Type::getPointerElementType`（opaque pointer），
    指针类型统一映射为 `void*`；类实例显示为指针。
  - 设计见 `docs/optimization_debugging.md`。

### v3.1.0
- **IR 优化管线（M1）**：`-O1/-O2/-O3` 真正生效——`writeObjectFile` 用 PassBuilder NewPM
  运行 `buildPerModuleDefaultPipeline`（此前 `-O` 只影响后端指令选择，IR 无优化）。
  修复优化暴露的两个真实 bug：`setjmp` 缺 `returns_twice` 属性、`myp_throw` 误标 `noreturn`
  （后者导致 try/catch 在 `-O1/-O2` 下失效）。
  验证：`-O0` 与 `-O2` 双跑全套 109/109（`tests/run_tests_O2.sh`）+ ASAN 109/109。
  设计见 `docs/optimization_debugging.md`。

### v3.0.0
**里程碑版本**：整合 v2.4 系列全部累积，语言与运行时能力大幅扩展。所有变更 additive，语言规格保持 1.0。

- **协程完整体系（C1-C10）**：`@coro` 方法/顶层函数 + `await` 挂起/恢复 + 值传递 + 返回值槽 +
  自动调度器（`Coro.scheduler`）+ 事件等待（`await ClassName.eventName`）+ 超时（`await event timeout N`）+
  多事件等待（`Coro.waitAny`）+ 嵌套协程（`ret_ctx` 上下文链）+ 诊断（`Coro.status/current/count`）+
  协作式取消（`requestCancel`）+ 协程内异常边界（未捕获异常安全结束协程，不崩进程）+
  栈池复用 + `@coro(stack=N)` 栈可配置 + TLS 线程并用
- **协程间通信**：`stdlib/channel.myp` 缓冲通道（协程阻塞 send/recv + 唤醒）；协程 await Future（非阻塞等待）
- **并发/运行时**：动态事件等待表与动态事件队列（无 1024 硬上限）、工作窃取线程池（条件变量唤醒）、
  Barrier / Future / Promise
- **语言**：class 顶层 const、property 默认值修复（`int x = 5` 在 `new` 时生效）、Range for、
  类型推断 `var`、`long` 字面量后缀
- **语法**：算子系统（struct `operator:` 节 + 顶层 `@op("...")`）、管道 `|>`
- **工程**：异常机制完善（finally 全路径/`throw;` 重抛/对象异常/接口匹配）、LSP 稳定性修复、
  测试基础设施（普通 + ASAN 全套 109/109）

> **V2.0 → V3.0 升级说明**：见 [docs/UPGRADE_V3.md](UPGRADE_V3.md)。

### v2.4.3
- **协程（C1-C4 完整落地，additive）**：`@coro` 类 action 方法 + `await` 挂起/恢复
  - C1：spawn（create/set_entry/入口参数槽/首启）+ 手动 `resume`；`await;` 简单挂起
  - C2：`await` 值传递（`int v = await expr;`）+ `@coro` 返回值槽（`Coro.result`）
  - C3：自动调度器（就绪队列 + `Coro.scheduler()` round-robin）
  - C4：事件集成（`await ClassName.eventName` 阻塞等待 + 事件派发通知）
  - 用户 API 为编译器内建静态类 `Coro`（scheduler/resume/yield/isActive/destroy/result/waitEvent），
    `__myp_coro_*` 符号未注册（用户调用即 undefined）；`stdlib/coro.myp` 无 FFI 声明
  - 回归测试 `tests/coro/` + `tests/coro_auto/` + `tests/coro_event/`（普通 + ASAN 全套 94/94 通过）
  - 设计规范见 `docs/coro.md`
- **异常机制完善（additive）**：`finally` 全路径传播（return/break/continue）、`throw;` 重抛、
  未处理异常消息+abort、标准异常类（FileError/JsonError/NetError 等库接入）、`catch (Error e)` 接口匹配
- **LSP 稳定性**：修复 `didOpen`/`didChange` JSON 解码 bug（`\n`/`\t` 转义处理），消除 35GB 内存爆炸
- **测试基础设施**：`run_tests.sh` diff 改用临时文件（规避 process-substitution FIFO 不可靠）；
  `time`/`timeline` 测试改为确定性断言（消除 sleep 精度 flaky）

### v2.4.2
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
