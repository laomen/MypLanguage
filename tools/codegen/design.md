# tools/codegen — MYP 代码生成框架（torchgen 式）

> 状态：**P0-P4 已实施（2026-08-11）** · P5 起规划中
> 定位：**MYP 自举的 schema 驱动代码生成框架**——声明式 schema → 生成 MYP/C/C++ 源码。
> 对标：PyTorch torchgen（算子 schema → C++/CUDA/Python）、gRPC/Thrift（IDL → 各语言 stub）。
> 关联：`docs/next_improvements.md` §六-6、`docs/serde_macro.md`（编译器内 `@derive` 为另一路线）、
> `tools/pm`（自举先例）、`stdlib/json.myp`（schema 解析）。

---

## 1. 定位与背景

MYP 无运行时反射，且手写桥接/序列化/样板代码是生态痛点（`hdf5_bridge.c`、
`sdl_bridge.c` 手写 C、`mlp.myp` 手写 forward/backward、每类手写 JSON）。
**代码生成**是 MYP 对"反射"的系统性替代——把"遍历类型/字段/算子自动产出代码"
从运行时搬到**编译期（工具期）**。

`tools/codegen` 是这一生成器生态的**统一引擎**：

```
            ┌────────────── tools/codegen（纯 MYP）──────────────┐
 schema  ──►│  schema.myp   解析 schema（JSON，复用 json.myp）    │──► 生成
 (声明)    │  model.myp    模型（类/字段/算子/方法 结构化数据）   │    MYP 源码
            │  emit.myp     发射器（缩进/拼接/转义/多目标语言）    │──►  C 源码
            │  gen_<X>.myp  各生成器（serde/ffi/autodiff/idl…）   │    C++ 源码
            └──────────────────────────────────────────────────┘
```

**为什么先做框架**：serde / FFI / autodiff / IDL / ORM / DSL 各生成器 80% 的骨架相同
（读 schema → 建模型 → 拼代码），抽成框架后，每个新生成器只剩"生成规则"本身。

## 2. 工程约束（沿用 tools/pm/fmt/viz 的硬约束）

1. **不改编译器**：纯现有 MYP 语言特性 + stdlib（`json.myp`/`fmt.myp`/`fs.myp`）实现，
   不修改 `src/`、不新增语法/注解。
2. **仅 bug 修正例外**：自举中若发现编译器 bug，可只做 bug 修正并跑全回归（`run_tests.sh` 不破）。
3. **层级 1 外部工具**：通过 `mypc` 子进程（`MYP_CC` 定位，复用 `tools/pm/build.myp` 模式）
   与编译器交互，产出的源码再交给 `mypc` 编译。
4. **自举**：框架本身用 MYP 写，`mypc run tools/codegen/main.myp ...` 运行。

## 3. 项目结构

```
tools/codegen/
  design.md          ← 本文档
  README.md          ← 用法（实施时补）
  main.myp           ← CLI：mypc run main.myp <gen> <schema> [-o out] [options]
  schema.myp         ← schema 解析（JSON → 模型）
  model.myp          ← 模型类型：Schema/TypeDecl/Field/Op/Method
  emit.myp           ← 发射器框架：源码生成器（文本行/缩进/转义/块）
  gen_serde.myp      ← 生成器①：JSON 序列化代码（MYP）
  gen_ffi.myp        ← 生成器②：C 绑定（ffi 声明 + 桥 + 资源 RAII）
  gen_autodiff.myp   ← 生成器③：自动微分（正向 → 反向）
  gen_idl.myp        ← 生成器④：IDL → 接口 + JSON-RPC 编解码 + socket 传输
  gen_orm.myp        ← 生成器⑤：ORM（tables → 实体 + CRUD SQL）
  tests/             ← 每生成器正/负测试
```

## 4. 核心架构：Schema → 模型 → 发射器

```
输入 schema（JSON 声明）       模型（MYP 数据）           输出（目标语言源码）
┌──────────────────┐   parse  ┌──────────────────┐  emit  ┌─────────────────┐
│ { "types": [      │ ───────► │ Schema {          │ ─────► │ // 生成的 MYP/C │
│    {"name":"Vec2",│ schema.  │   List<TypeDecl>  │ emit.  │ class Vec2 {    │
│     "fields":[    │ myp      │   List<Op> ... }  │ myp    │   string toJson…│
│      {"n":"x","t":"double"}…│                  │        │                 │
└──────────────────┘          └──────────────────┘        └─────────────────┘
```

- **schema.myp**：读 JSON schema 文件 → `Schema` 模型（类型化，非裸 JSON）。
- **model.myp**：`Schema/TypeDecl/Field/Op/Param` 等结构化类型（MYP class），
  各生成器共享。
- **emit.myp**：发射器基础——按缩进层级追加行、字符串转义（MYP/C 双目标）、
  块开闭（`{`/`}`）、`emit 源文件写入`（`fs.myp`）。生成器只写"规则"不碰缩进细节。

### 4.1 一次运行的流水线

```
mypc run main.myp serde player.schema.json -o gen/
  → schema.myp 解析 player.schema.json → Schema 模型
  → gen_serde.myp 遍历模型 → 用 emit.myp 拼 Player.toJson/fromJson 源码
  → 写入 gen/player_serde.myp
  → （可选 --compile）调 mypc 编译验证产出合法
```

## 5. Schema 格式（JSON 声明）

schema 用 JSON（复用 `json.myp` 解析，零新语法）。示例 `player.schema.json`：

```json
{
  "types": [
    { "name": "Vec2", "kind": "struct",
      "fields": [ {"name": "x", "type": "double"}, {"name": "y", "type": "double"} ] },
    { "name": "Player", "kind": "class",
      "fields": [
        { "name": "name", "type": "string" },
        { "name": "hp",   "type": "int" },
        { "name": "pos",  "type": "Vec2", "ref": "type" },
        { "name": "items", "type": "int", "array": true }
      ] }
  ],
  "ops": [ ]   // autodiff/ffi 用：算子签名
}
```

- `kind`: `struct` / `class` / `enum` / `op`（算子）/ `ffi`（C 函数）。
- `ref`: `type` = 引用另一 schema 类型（嵌套）；`array:true` = 动态数组。
- 一个 schema 可同时含类型与算子（serde 只吃 types，autodiff 吃 ops）。

## 6. 发射器框架（emit.myp）

```myp
class Emitter {
    action:
        void push(int indent);            // 增加缩进
        void pop();
        void line(string s);              // 当前缩进 + s + "\n"
        void block(string header);        // "header {" + indent++
        void endBlock(string tail = "}"); // indent-- + tail
        string escape(string s);          // MYP/C 字符串转义（按目标切换）
        void writeFile(string path);      // 写源码文件（fs.myp）
}
```

多目标语言：`Emitter` 带 `lang`（`myp`/`c`/`cxx`），转义与注释风格随之切换。
生成器只写"生成什么"，框架管"怎么排版/转义/落盘"。

## 7. 内置生成器路线

| 阶段 | 生成器 | schema | 产出 | 价值 / 验收 |
|------|--------|--------|------|-------------|
| **P0** | **serde**（JSON 序列化，方案 A 正式化） | types | 每类的 `toJson()/fromJson()`（MYP） | 终结手写序列化；纯 MYP 输出、最快闭环；**验收：round-trip 一致** |
| **P1** | **ffi**（C 绑定） | ffi 函数签名 | `ffi` 声明 + C 桥 `*_bridge.c` + 资源 RAII 包装类 | 最大生态杠杆；目标案例 `hdf5_bridge.c`/`sdl_bridge.c` 重构；**验收：产出桥可编译 + 现有测试等价** |
| **P2** | ✅ **autodiff**（表达式符号求导） | exprs 表达式 | 前向 + 梯度函数（MYP） | deeplearning 进化（表达式级反向）；**验收：数值梯度与有限差分一致**（见 §10 P2 行） |
| **P3** | **idl**（RPC） | 服务/方法 | client/server stub + 编解码（MYP，配 net/http/json） | 网络生态；**验收：示例 RPC 通** |
| P4 | orm / DSL / 资源嵌入 | 表 / 规则 | 数据类 + CRUD / 生成代码 | 生态补强 |

> 顺序逻辑：P0 用纯 MYP 输出验证框架闭环；P1 上最大杠杆（C 输出）；
> P2 接 deeplearning；P3 起网络生态。每阶段独立可交付。

### 7.1 P0 生成器示例：`gen_serde.myp` 产出

输入 `player.schema.json`（§5）→ 输出 `player_serde.myp`：

```myp
// ── 由 tools/codegen 生成，勿手改 ──
string Vec2_toJson(Vec2 v) {
    return "{\"x\":" + v.x + ",\"y\":" + v.y + "}";
}
void Vec2_fromJson(Vec2 v, Json j, string p) {
    v.x = j.getDouble(p + ".x");
    v.y = j.getDouble(p + ".y");
}
string Player_toJson(Player p) {
    string s = "{\"name\":\"" + Json.escape(p.name) + "\",";
    s = s + "\"hp\":" + p.hp + ",";
    s = s + "\"pos\":" + Vec2_toJson(p.pos) + ",";
    s = s + "\"items\":[" + Json.intArray(p.items, p.items.size()) + "]}";
    return s;
}
```

### 7.2 P1 生成器示例：`gen_ffi.myp` 产出（概念）

输入（schema 内 `"ffi": [ {"c":"hdf5_open", "ret":"long", "params":[...]} ]`）→

```myp
// 自动生成 ffi 声明 + 资源类
ffi long hdf5_open(string path, int mode);
ffi int  hdf5_close(long handle);
class H5File {            // 资源 RAII：构造 open、析构 close（ARC 销毁桩挂钩）
    @constructor H5File(string path) { handle_ = hdf5_open(path, 1); }
    ~H5File() { if (handle_ != 0L) hdf5_close(handle_); }
    property: long handle_;
}
```

## 8. 与 `@macro` / 编译器的边界

- `tools/codegen` 是**层级 1 外部工具**：不改编译器，产出源码再交给 `mypc`。
- 编译器内 `@derive(Json)`（`docs/serde_macro.md` 方案 B）是**层级 2 语言内路线**，
  与之互补：**先做外部 codegen（本轮）**，验证 schema/模型/发射器设计；
  未来 `@derive` 若落地，生成规则可复用 codegen 的模型设计。
- 不引入运行时反射：生成代码是静态、类型安全、零开销的。

## 9. 测试与验收

- 每生成器：schema 正例 → 生成 → 编译产出 → 运行验证（如 serde round-trip）。
- 框架自测：emit 缩进/转义/块平衡的单元用例。
- 回归：`run_tests.sh` 不破；codegen 产出经 `mypc` 编译通过。
- `tests/` 下按生成器分目录：`serde/`、`ffi/`、`autodiff/`。

## 10. 分阶段实施

| 阶段 | 内容 | 前置 | 验收 |
|------|------|------|------|
| **P0a** | ✅ 框架骨架：`schema.myp` + `model.myp` + `emit.myp` + `main.myp` CLI | 无 | `mypc run main.myp serde schema.json -o dir` 可读 schema、emit 文件 |
| **P0b** | ✅ `gen_serde.myp`：标量/string/嵌套 struct 的 toJson/fromJson | P0a | `tests/schema.json` + `test_serde.myp` round-trip 一致；`run_tests.sh` 通过。数组字段检测告警跳过（class 属性私有 → toJson 待方法化） |
| **P1** | ✅ `gen_ffi.myp`（P1a 声明 + **P1b 资源包装类**） | P0b | P1a：4 个 runtime C 函数链接运行正确。P1b：`resources` 段 → 包装类（构造/`open`/`close`/`getHandle`，`invalid` 哨兵；MYP 无用户析构器 → 显式生命周期 RAII）。`schema_res.json` + `test_res.myp`：barrier open/close 幂等通过 |
| **P2** | ✅ `gen_autodiff.myp`：表达式符号求导 → 前向 + 梯度函数 | P1 | `schema_autodiff.json` + `test_autodiff.myp`：f1=x*x+y / f2=x·sin(y)+exp(x) / f3=log(x)/sqrt(y) 解析梯度与有限差分一致。表达式语法 v1：+ - * / 一元负 括号 sin/cos/exp/log/sqrt；MYP 自写 tokenizer/parser/求导/简化（常量折叠+浅层） |
| **P3** | ✅ `gen_idl.myp`（P3a 协议层 + **P3b socket 传输**） | P2 | P3a：`schema_idl.json` + `test_idl.myp`：Calc{add/echo/mul3} JSON-RPC 进程内验证（add=7/echo=hi!/mul3=7.5/未知方法 ok:0）。P3b：生成 `<Svc>_server_once`（accept→recvLine→dispatch→sendLine）+ `<Svc>_client_call`（connect→sendLine→recvLine），`test_idl_socket.myp` 用 @thread 服务器 + 真实 TCP 回环验证 |
| **P4** | ✅ `gen_orm.myp`：tables → 实体 struct + CRUD SQL 生成 | P3 | `schema_orm.json` + `test_orm.myp`：Player{id key,name,hp}/Item{id key,price double,label} 的 CREATE/INSERT/SELECT_ALL/SELECT_BY_KEY/UPDATE/DELETE 语句精确匹配；类型映射 int/long/bool→INTEGER、double/float→REAL、string→TEXT |
| P5 | 资源嵌入（embed）/ DSL / `--verify` 编译校验增强 | P4 | 各自验收 |

## 11. 风险与决策

| 风险 | 缓解 |
|------|------|
| schema 设计过早定型 | P0 用 serde 一个真实生成器驱动 schema 演进，不一次性定全 |
| 生成器代码膨胀（每生成器重复） | emit 框架收敛缩进/转义/落盘；生成规则保持纯声明式 |
| 产出与手写风格不一致难维护 | 头部"生成勿手改"标记 + `--verify` 编译校验 |
| 与编译器内 `@derive` 路线分叉 | 模型层（TypeDecl/Field）设计对齐，未来可复用 |

## 12. 附：与主流 codegen 对照

| | torchgen (PyTorch) | gRPC/Thrift | MYP tools/codegen |
|---|---|---|---|
| 输入 | 算子 yaml | IDL | JSON schema |
| 引擎语言 | Python | 生成器各语言 | **MYP 自举** |
| 产出 | C++/CUDA/Python | 多语言 stub | MYP/C/C++ |
| 是否自举 | 否 | 否 | **是**（MYP 生成 MYP） |

---

**下一步**：P0 已闭环（框架 + serde 生成器 + 自测）。下一步按 §10 实施 **P1 `gen_ffi.myp`**
（C 函数 → ffi 声明 + 资源 RAII，重构 hdf5/sdl 手写桥），以及 P0 延伸：数组字段、
class 属性方法化 toJson/fromJson。
