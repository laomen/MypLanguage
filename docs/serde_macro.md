# 派生序列化宏 `@derive(Json)` 设计（serde 式过程宏）

> 状态：**设计稿（待评审）**
> 定位：方案 B——强化 `@macro` 到类级派生，编译期为 class 自动生成序列化代码，
> 终结"每个类手写 toJson/fromJson"的工程缺口。对标 Rust `#[derive(Serialize)]`（serde）。
> 关联：`docs/metaprogramming.md`（M3 声明式宏 / M4 过程宏）、`stdlib/json.myp`、`docs/next_improvements.md` §三-7。

---

## 1. 背景与动机

MYP 无运行时反射（`rtti` 仅 `typeId/typeOf/sameType`），无法做 Gson/Jackson 式
"遍历对象字段自动序列化"。当前 `json.myp` 是 C 端 JSON **解析器**（路径查询式），
不是对象映射器——每类序列化只能手写：

```myp
string s = "{\"name\":\"" + Json.escape(obj.name) + "\",\"hp\":" + obj.hp + "}";
```

手写序列化的工程代价：样板代码 + 加字段忘改序列化 → 隐蔽 bug + 类型不安全。

**核心思路**：MYP 已有过程宏 `@macro` + `quote`（M4，编译期执行、用 MYP 写宏函数），
且泛型是**单态化**（编译期每类型克隆）——这两者与 Rust serde 的"编译期 derive + 单态化"
架构同构。把 `@macro` 从"语句位置展开"升级为"**类级注解派生**"，就能让宏在编译期
看到类的字段元数据，自动生成序列化代码：**零运行时反射、零运行时开销、类型安全**。

## 2. 目标与范围（v1）

**目标**：`@derive(Json)` 修饰 class，编译器自动生成该类的 `toJson()/fromJson()`。

```myp
@derive(Json)
class Player {
    property:
        string name;
        int hp;
        Vec2 pos;      // Vec2 也 @derive(Json)
        Item[] items;  // 动态数组
}

// 宏自动注入：
//   string toJson()                → "{\"name\":\"A\",\"hp\":100,\"pos\":{...},\"items\":[...]}"
//   void fromJson(string j)        → 用 Json(str) 按路径填回各属性
```

**v1 范围**：
- 类级注解 `@derive(<宏名>)`；宏名解析到已注册的 `@macro` 函数（形如 `MethodList Json(ClassMeta m)`）。
- 编译期新增 `ClassMeta` / `PropMeta` 值类型（仅 @macro 签名/体内可用）。
- 生成**方法列表**并注入到目标类（`toJson` / `fromJson`）。
- 属性类型覆盖：标量（int/long/double/float/bool）、string、enum、嵌套 struct、class、
  `T[]` 动态数组、`slice<T>`、`Option<T>`。
- 泛型类：`@derive` 在**单态化后按具体实例**展开（`Box<int>` / `Box<Point>` 各得一份，
  与 serde 对泛型 struct 的行为一致）。

**v1 不做**：
- 运行时反射/字段表（违背"极简前端"哲学，排除）。
- 循环引用（self 引用类）序列化——编译期报错或退化（见 §9）。
- 自定义字段名/忽略字段的注解参数（`#[serde(rename)]` 式）→ v2。
- 表达式位置的 `@derive`（类级即可）。

## 3. 现状基础（可复用能力）

| 能力 | 现状 | 用途 |
|------|------|------|
| `@macro` + `quote`（M4，v3.6） | 语句位置过程宏，`$var` 插值（int/string/Expr/Stmt/StmtList），`StmtList + StmtList` 拼接 | 生成 toJson/fromJson 代码体 |
| 解释器（`src/eval/eval.cpp`） | M1 纯函数解释器，已支持 `Stmt/StmtList/Expr` AST 变体 | 承载新的 `ClassMeta` 值类型与遍历 |
| `json.myp`（C 端 FFI） | `Json(str)` + `type/getString/getInt/getDouble/getBool/arrayLen(path)` + `free()` | fromJson 的运行时解析 |
| `quote` 内 `$x` 插值 | 字面量/Expr/Stmt 内联 | 生成字段读写 |
| 单态化（sema） | 泛型类按实参克隆（`Box_int_inst`） | 泛型类 derive 按实例展开 |

## 4. 语法设计

```myp
@derive(Json)                      // 类级注解：触发名为 Json 的派生宏
class Player { ... }
```

- `@derive(<Identifier>)` 是**类声明注解**（新注解名，additive，不影响现有语法）。
- 编译器解析：遇到类上 `@derive(X)` → 在已注册的 `@macro` 函数中找名为 `X` 且
  签名为 `MethodList X(ClassMeta m)` 的宏函数 → 编译期调用，传入该类元数据 →
  返回的方法定义注入该类。
- 未找到宏 / 签名不匹配 → 编译期诊断（编译器永不崩溃）。
- 一个类可多个 `@derive(Json) @derive(Debug)`。

## 5. 宏系统扩展（核心）

### 5.1 新编译期值类型：`ClassMeta` / `PropMeta`

| 编译期类型 | 字段 | 含义 |
|---|---|---|
| `ClassMeta` | `string name` | 类名（泛型类为实例名） |
| | `List<PropMeta> props` | 属性表（声明序） |
| | `bool is_generic_inst` | 是否单态化实例 |
| `PropMeta` | `string name` | 属性名 |
| | `string type` | 类型名（int/string/Vec2/Item[]/...） |
| | `int kind` | 0 标量 1 string 2 bool 3 enum 4 struct 5 class 6 array 7 slice 8 option |
| | `string elem_type` | kind=6/7 的元素类型 |
| | `string inner_type` | kind=8 的 Option 内类型 |
| | `bool is_struct_elem` | 嵌套 struct（需递归 toJson） |

这些是**编译期专属类型**（同 `Expr/Stmt/StmtList`）：仅 @macro 签名/体内可用，
不能作运行时变量/参数（sema 校验）。

### 5.2 类级注解展开流程

```
parse → 收集 @derive(X) 类注解 → 收集 @macro 函数（含 MethodList 返回类型新签名）
     → 类展开 pass（扩展 MacroExpander）：
         (1) 解析 @derive(X) 引用的宏函数，构造 ClassMeta（类名 + 属性表）
         (2) 编译期解释执行宏函数体（M1 解释器 + quote + ClassMeta 遍历）
         (3) 返回值 MethodList（方法定义列表）→ 深度克隆 → 注入该类 action 段末尾
     → 迭代展开直到稳定 / 深度上限
     → sema（此时类已含 toJson/fromJson）→ codegen
```

### 5.3 方法注入

- 新增返回类型 `MethodList`（AST 变体：方法声明列表），允许在类内展开为方法定义。
- 注入位置：该类 `action:` 段末尾；注入的方法可自然访问 `this` 与裸属性
  （与手写方法完全一致，无可见性差异）。
- 泛型类：`@derive` 在**单态化后**对该具体实例类（`Box_int_inst`）执行——类属性已
  替换为具体类型（`T` → `int`），宏看到的就是具体字段表。

### 5.4 派生宏函数示例（`@macro Json` 的用户态写法）

```myp
@macro MethodList Json(ClassMeta m) {
    StmtList body = quote {
        string toJson() {
            string s = "{";
        }
    };
    // 遍历 m.props：按 kind 追加字段拼接/解析语句（算法驱动，M1 子集可实现）
    for (i in 0 .. m.props.size()) {
        PropMeta p = m.props.get(i);
        if (p.kind == 0) {
            body = body + quote { s = s + "\"$p.name\":\"" + $p.name + "\","; };
        }
        // ... 按 kind 分派（标量/字符串/嵌套/数组/...）
    }
    // 收尾 + fromJson 由同模板生成
    return quote {
        string toJson() { ... }
        void fromJson(string j) { ... }
    } + body;
}
```

> 说明：`m.props.get(i)`、`p.kind`、`p.name` 依赖 §5.1 的 `ClassMeta/PropMeta` 字段
> 访问（V2 的 AST 字段访问同族能力）。v1 可直接在宏体内用 `List<PropMeta>` + 循环。

## 6. 生成代码形态

对 `@derive(Json) class Player { string name; int hp; Vec2 pos; }`（`Vec2` 也 derive），
宏注入：

```myp
// —— toJson ——
string toJson() {
    string s = "{";
    s = s + "\"name\":\"" + Json.escape(name) + "\",";
    s = s + "\"hp\":" + hp + ",";
    s = s + "\"pos\":" + pos.toJson();
    s = s + "}";
    return s;
}
// —— fromJson（用 json.myp 路径查询）——
void fromJson(string j) {
    Json p = new Json(j);               // C 端解析
    name = p.getString("name");
    hp = p.getInt("hp");
    pos.fromJson(p.getString("pos"));    // 嵌套 struct 递归
    p.free();
}
```

**属性类型 → 生成片段**：

| 属性类型 | toJson 片段 | fromJson 片段 |
|---|---|---|
| `int/long/double/float` | `"\"" + name + "\":" + name` | `name = p.getInt("name")` / `getDouble` |
| `bool` | `"\"b\":" + (b ? "true" : "false")` | `b = p.getBool("b")` |
| `string` | `"\"s\":\"" + Json.escape(s) + "\""` | `s = p.getString("s")` |
| `enum E` | 判别值名（`E.toString` 或 codegen 表） | 反查 |
| 嵌套 struct | `"\"" + x + "\":" + x.toJson()` | `x.fromJson(p.getString("x"))` |
| class（也 derive） | 同 struct（`toJson()`） | 构造 + `fromJson` |
| `T[]` / `slice<T>` | `"[" + 逐元素 toJson + "]"`（`arrayLen` 遍历） | 循环 `p.getInt("items.0")` |
| `Option<T>` | `some ? 值 : "null"` | `isSome` 判断 |

## 7. 与 json.myp 集成

- **toJson**：纯字符串拼接，不依赖 json.myp；需要 `Json.escape`（新增静态方法，v1 由宏
  模板内联或 stdlib 补）。
- **fromJson**：复用 `Json(str)` + 路径查询 API（`getString/getInt/getDouble/getBool/
  arrayLen`）。嵌套对象用 `getString("pos")` 取出子 JSON 再递归 `fromJson`（当前 C 解析器
  返回子串即可，无需新 API）；数组用 `arrayLen("items")` + 逐元素路径 `items.N.field`。
  （若需"取任意子节点原始串"，给 C 解析器加一个 `myp_json_get_raw(handle, path)`，小改动。）

## 8. 与单态化的交互（泛型类 derive）

- `@derive(Json)` 在泛型类模板上声明，**不**在模板期展开（模板属性含占位符 `T`）。
- 单态化产出 `Box_int_inst` / `Box_Point_inst` 后，对每个实例类执行 derive——宏看到
  的字段表是具体类型（`v: int` / `v: Point`），生成对应代码。
- 效果同 Rust serde：`Box<Point>` 得一份能序列化 Point 的方法，零装箱。
- 代价：每实例一份序列化代码（单态化既有的代码膨胀，无新增量）。

## 9. 边界情况

| 情形 | 处理 |
|---|---|
| 嵌套 struct 未 derive | 编译期诊断："X 需 `@derive(Json)`"（或自动推导：见到 struct 类型即递归生成，v2） |
| class 属性是 `interface` 引用 | 无法静态确定具体类 → 编译期诊断（v1 不支持多态序列化） |
| 自引用/循环引用类 | 编译期检测依赖环 → 诊断；不尝试运行时环处理（v1） |
| 私有属性 | 注入方法在类内，天然可访问所有属性（无可见性问题） |
| 属性含 `transient`/需忽略 | v2 注解参数 `@derive(Json, skip = [password])` |
| `Option<T>` 内嵌 struct | 递归生成（isSome 分支） |
| 泛型类无具体实例 | 模板期不展开，无代码（正确） |

## 10. 分阶段实施

| 阶段 | 内容 | 前置 |
|---|---|---|
| **P0** | 编译期 `ClassMeta/PropMeta` 值类型 + `MethodList` 返回类型 + `@derive` 类注解识别 + 类级宏调用 + 方法注入 + 标量/string/bool 生成 | `@macro` V2 的 AST 字段访问（`m.props.get(i)`/`p.kind`） |
| **P1** | 嵌套 struct/class 递归 + enum + `T[]`/`slice` + `Option<T>` + `Json.escape` + 与 `json.myp` fromJson 集成 | P0 |
| **P2** | 泛型类 per-instance derive + 依赖环检测 + `skip` 注解参数（v2 化） | P1 |
| **P3** | `tests/@test/serde_*` 正负测试 + `-O0/-O2/ASAN` 全回归 + 文档 | P2 |

## 11. 风险与替代方案

| 风险 | 缓解 |
|---|---|
| 类级展开时序（在 sema 前注入方法，与单态化先后） | P0 先固定"非泛型类 → sema 前注入；泛型类 → 单态化后注入"两条路径，测试先行 |
| `ClassMeta` 遍历依赖 @macro V2 AST 字段访问未落地 | P0 将字段访问（`List<PropMeta>.get/kind/name`）列为同批交付，不做 `Ast.*` 字符串解析 |
| fromJson 路径查询性能（C 解析器逐 path 查找） | 每字段一次查询可接受；重度场景 v2 走一次性 parse 到对象树 |
| 代码膨胀（每类/每实例一份） | 与单态化一致；LLVM 可内联；非新问题 |

**替代方案**（不采用或降级）：
- **A 外部代码生成器**（`tools/serde.myp` 读 schema 吐代码）：不改编译器、最快落地，
  但脱离语言（改字段要重跑生成器、无注解表达力）。可作为 P0 前的临时方案。
- **C 接口约定 + 泛型辅助**（`interface Serializable { string toJson(); }` + 字段辅助）：
  零编译器改动，但每类仍手写（只是减负），不终结缺口。
- **D 运行时反射**（每类生成字段表 + rtti 字段遍历）：违背"极简前端"哲学，排除。

## 12. 验收标准

1. `@derive(Json) class Player { ... }` → `player.toJson()` 输出合法 JSON；
   `player.fromJson(s)` 还原全部字段，往返一致（round-trip）。
2. 嵌套 struct / `T[]` / `slice` / `Option<T>` / enum / 泛型类（`Box<Point>`）全通过。
3. 负测试：嵌套未 derive、接口属性、自引用 → 编译期诊断，编译器不崩溃。
4. `-O0/-O2/ASAN` 全量回归通过；`@derive` 不产生任何运行时反射开销。
5. `tests/@test/serde_*` 覆盖上述矩阵。

---

### 附：与 Rust serde 的对照

| | Rust serde | MYP `@derive(Json)` |
|---|---|---|
| 机制 | `#[derive]` 过程宏，编译期生成 | `@derive` 类注解，@macro 编译期生成 |
| 运行时 | 零开销、单态化 | 零开销、单态化 |
| 宏语言 | proc macro（Rust 代码） | @macro（MYP 代码，M1 子集） |
| 反射依赖 | 无 | 无 |
| 泛型 | derive 对泛型 struct 按实例展开 | 对单态化实例展开 |
