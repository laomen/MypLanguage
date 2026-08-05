# MYP 包管理器设计（用 MYP 语言自举）

> 状态：**v1 已实施（2026-08-05）**——`tools/myp.myp`（init/build/install/run/legacy）
> 关联：语言规格 v1.0（`docs/grammar.md`）、变更策略（`docs/CHANGELOG.md`）、
> 编译器 `--package-path`（`src/main.cpp` `loadModule`）、现有 Python 版 `myp`（仓库根）。
> 本文档提出**用 MYP 语言重写包管理器**（自举工具链），作为语言稳定性证明与
> 自举 roadmap（`docs/design.md` §11）的第一步。

---

## 1. 背景与动机

### 1.1 现状

已有包管理基础设施：

| 组件 | 语言 | 说明 |
|------|------|------|
| `myp` 脚本（268 行） | **Python 3** | CLI：`init`/`build`/`install`/`run` + legacy 单文件模式 |
| `--package-path` | C++（`mypc`） | `loadModule` 按 `<pkg_path>/<module>/src/<module>.myp` 解析导入 |
| `package.myp` | 文本 | `name`/`version`/`depends` 字段 |

现有 `myp` 是 Python 包装脚本，核心逻辑（参数/路径/子进程/JSON/环境变量）全部可用 MYP
stdlib 等价实现。

### 1.2 动机

1. **自举**：语言自举自己的工具链——包管理器是编译器之上第一层真实工具。
2. **稳定性证明**：一个真实 ~300 行工具，覆盖文件系统、进程、字符串、异常、CLI 全部
   子系统，比任何单测更能暴露语言短板（呼应 v3.9.0 "稳定性未充分证明"关切）。
3. **消除 Python 依赖**：分发 MYP 工具链不再需要 Python 运行时。

---

## 2. 目标与范围

| 里程碑 | 范围 | 工作量 |
|--------|------|--------|
| **v1** | 用 MYP 重写现有 `myp` 全部功能（`init`/`build`/`install`/`run`/legacy），行为与 Python 版**完全一致** | ~1-2 天 |
| **v2** | `depends` 自动安装、版本解析 + lockfile、registry（git 仓库）、远程拉包 | ~3-5 天 |

**非目标（v1/v2）**：依赖冲突解决（MVS）、语义化版本范围运算（^/~）、TLS/HTTPS 原生
下载（v2 用 `curl`/`git` 编排，不引入 TLS FFI）。

---

## 3. 能力映射（Python → MYP stdlib）

| Python | MYP | 状态 |
|--------|-----|------|
| `sys.argv` | `args.myp` | ✅ |
| `os.path` join/dirname/basename/exists/is_dir/is_file | `fs.myp` `Fs`/`Path` | ✅ |
| 递归目录遍历 | `Fs.listDir` + 递归 | ✅ |
| `subprocess.run`（调 `mypc`） | `process.myp` `Process.run/output` | ✅ |
| `os.environ`（`MYP_PACKAGE_PATH`） | `env.myp` get/set | ✅ |
| 读/写文件 | `io.myp` `File` | ✅ |
| 字符串 split/trim/substring | `text.myp` | ✅ |
| 版本解析 / 校验 | `text.myp` + `regex.myp` | ✅ |
| 异常处理 | `error.myp` + `try/catch` | ✅ |
| `os.makedirs`（递归） | **缺** | ⚠️ 需补 FFI |
| `shutil.copytree` / `shutil.rmtree` | **缺** | ⚠️ 需补 FFI |
| `shutil.copy`（单文件） | `io.myp` 逐字节复制 | ✅（或 FFI） |

**结论：仅需补 2 个 fs FFI**（见 §5），其余全部现有 stdlib 覆盖。

---

## 4. 架构

### 4.1 模块划分（v2 形态；v1 可先单文件）

```
myp_pkg/                 # 或 v1 单文件 myp.myp
  main.myp               # CLI 入口 + 命令分发（对应 Python main()/cmd_help）
  meta.myp               # package.myp 解析（load_package_meta）
  build.myp              # find_compiler / resolve_package_path / 调 mypc 编译
  install.myp            # 本地安装（copy → myp_packages/）
  registry.myp           # v2：registry 索引 / 版本解析 / 拉取
  lockfile.myp           # v2：myp.lock 读写
  util.myp               # fs 封装（复制目录 / 递归删除 / 路径）
```

### 4.2 命令面（v1 与现有 `myp` 完全一致）

```
myp init <name>          创建包脚手架（package.myp + src/<name>.myp + @constructor）
myp build [opts]         编译当前包（--stdlib + --package-path + 转发 -O/--trace）
myp install <path>       从本地路径安装包 → myp_packages/<name>/
myp run [opts]           build + 运行可执行
myp <file.myp> [opts]    legacy 单文件编译
```

v2 新增：

```
myp add <pkg>[@ver]      从 registry 解析并安装依赖，写 myp.lock
myp remove <pkg>         卸载依赖
myp update               按 lockfile 重装 / 升级
myp list                 列出已装依赖
```

### 4.3 目录布局（沿用现有）

```
<package>/
  package.myp            # name / version / depends
  src/<name>.myp         # 主模块
  myp_packages/          # 已装依赖（--package-path 指向此处）
  myp.lock               # v2：锁定已解析版本
```

### 4.4 package.myp 格式（沿用现有，兼容）

```
# MYP Package
name: mylib
version: 0.1.0
depends: other_pkg, util
```

- `#` 注释；`key: value` 字段；`depends` 逗号分隔列表
- 解析：`File.readLine` + `text.split` 手写（非 JSON，无需 json.myp）

### 4.5 构建流程（v1，与 Python 版一致）

```
myp build
  ├─ load package.myp（无则报错）
  ├─ 定位 mypc（build/mypc → 遍历 build → PATH）
  ├─ 解析 package path（本地 myp_packages + MYP_PACKAGE_PATH）
  ├─ 检查依赖（v1：仅提示缺失；v2：自动解析安装）
  └─ mypc src/<name>.myp -o <name>.out --stdlib ... --package-path ...
     （转发 -O/--trace/--emit-llvm）
```

### 4.6 registry（v2 设计）

- **形态**：git 仓库作为 registry（自托管 / GitHub / Gitee）。仓库根含索引：
  ```
  registry/
    index.json               # { "packages": { "name": { "versions": [...], "latest": "..." } } }
    packages/<name>/<ver>/   # 各版本包源码（含 package.myp）
  ```
- **拉取**：`Process.run("git clone --depth 1 <url> <cache>")` 或
  `Process.output("curl -s <url>/index.json")`（HTTP 编排，不引入 TLS FFI）
- **缓存**：`~/.myp/cache/<name>@<ver>/`
- **解析**：`myp add foo` → 查索引 → 取 `latest` 或 `@ver` 指定 → 复制到
  `myp_packages/` → 写 `myp.lock`

---

## 5. 需补的 stdlib FFI（最小集）

在 `src/runtime/runtime.c` 补（复用既有 `dirent`/`stat`）：

| FFI | 语义 | 实现 |
|-----|------|------|
| `myp_fs_mkdir_p(string path)` | 递归创建目录（如 `mkdir -p`） | 逐级 `mkdir`，已存在则跳过 |
| `myp_fs_remove_recursive(string path)` | 递归删除文件/目录（如 `rm -rf`） | `opendir` 递归 + `remove`/`rmdir` |

`stdlib/fs.myp` 加 `Fs.mkdirP(path)` / `Fs.removeRecursive(path)` 薄封装 + `tests/fs_more/`。
（单文件复制用 `io.myp` 逐字节即可，无需 FFI。）

---

## 6. 实施里程碑

### v1（MYP 重写现有功能）
1. `runtime.c` 补 `myp_fs_mkdir_p`/`myp_fs_remove_recursive` + `fs.myp` 封装 + 测试
2. 单文件 `myp.myp`：`args` 分发 + `meta` 解析 + `build`（调 mypc）+ `install`（复制）
3. 编译为 `build/myp`，与 Python 版**逐命令对拍**（相同输入 → 相同输出/退出码）
4. 验证：`myp init` → `myp build` → `myp run` 全链路

### v2（增强）
5. `depends` 自动解析安装 + `myp.lock`
6. registry 索引 + `myp add/remove/update/list`
7. `@parallel for` 并行安装多个依赖
8. 测试：本地假 registry 端到端

---

## 7. 自举与稳定性意义

- 交付物是一个**由 MYP 编译的原生可执行** `myp`，不再依赖 Python。
- 覆盖子系统：CLI 参数、文件系统（建/删/复制/递归）、进程调用（mypc、git、curl）、
  字符串解析、异常处理、环境变量；v2 加入并发。
- 这是 `docs/design.md` §11"自举"的第一步（先自举工具链，后自举编译器前端），
  同时是 v3.9.0 稳定性证明的最硬核补充。

---

## 8. 待评审决策点

- **D1**：产物命名/并存策略——直接替换根目录 `myp`（Python 版退役）vs 并存 `myp2` 过渡？
- **D2**：v1 单文件（~300 行，对齐 Python 版）vs 直接模块化拆分？
- **D3**：fs FFI 命名与语义——`myp_fs_mkdir_p` / `myp_fs_remove_recursive` 是否合适？
- **D4**：registry 形态——git 仓库 + `index.json` vs 纯 git 子目录（无索引文件）？
- **D5**：v2 是否补 json 序列化（写 `index.json`/`myp.lock`）vs 统一用 `key: value` 文本格式？

---

## 9. v1 实施记录（2026-08-05）

### 9.1 已交付
- `tools/myp.myp`（单文件，~400 行）：`init`/`build`/`install`/`run`/legacy + `package.myp` 解析。
- runtime 补 `myp_fs_mkdir_p` / `myp_fs_remove_recursive`；`stdlib/fs.myp` 加 `Fs.mkdirP`/`Fs.removeRecursive`。
- `myp_process_run` 改为返回真实退出码（`WEXITSTATUS`，此前 `system()` 原始值致退出码截断）。
- `tests/test_myp_pm.sh`（9 断言）+ 集成进 `run_tests.sh`（-O0/ASAN 全套 124/124）。

### 9.2 与 Python 版的有意差异
| 项 | Python 版 | MYP 版 | 说明 |
|----|-----------|--------|------|
| init 模板 | `printf(...)` 无 import → **编译失败** | `import env` + `Console` + `int main()` → **可编译可运行** | MYP 版产出可用包（改进） |
| init 路径打印 | 绝对路径 | 相对路径 | MYP 无 getcwd；功能一致 |

### 9.3 自举发现
- **语言 bug**：函数返回定长数组共享存储（`Fs.listDir`/`Str.split`），嵌套调用覆写外层数组 → `copyTree` 需快照规避（详见 `next_improvements.md` §九）。
- **io 约束**：`__myp_io_*` 单一全局句柄，不能同时开两个 File → `copyFile` 分两阶段（先读后写）。
