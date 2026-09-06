# MYP 编程语言

> 事件驱动组件语言 | LLVM 21 后端 | 内置 GPU 支持
>
> 编译器 v3.16.0 | 语言规格 v1.0

<p align="center">
  <img src="logo/logo2.png" alt="MYP Logo" width="460">
</p>

**🌐 [English](README_EN.md)**

MYP 是一门**事件驱动组件**编程语言，以 `class` + `action:` / `event:` 为架构单元，通过 `mapping()` 声明式组装系统。编译器基于 LLVM 21，生成原生可执行文件。

## ✨ 核心特性

| 特性 | 说明 |
|------|------|
| **事件驱动组件** | 组件间只通过事件通信，`mapping()` 声明式组装 |
| **Actor 式并发** | `@thread` 实例天然隔离，无需加锁 |
| **数据并行** | `@parallel for` 自动并行化，work-stealing 线程池 |
| **泛型** | `ArrayList<T>`、`HashMap<K,V>`、`Queue<T>` 等 |
| **接口多态** | `interface` + 虚表分派（胖指针） |
| **协程 + 异步 IO** | `@coro`/`await` 寄存器级纤程（asm 切换，无系统调用）+ `@async` 统一异步抽象（定时器/套接字/文件执行器）+ `Coro.waitAnyOf` 混等 |
| **错误处理** | `Result<T,E>` / `Option<T>`/`T?` 容器 + `catch (Error)` 异常分层 |
| **自动内存管理** | class 实例 ARC（自动引用计数，additive 无新语法） |
| **派生序列化** | `@derive(Json)` 类注解自动生成 toJson/fromJson（serde 式，零运行时反射） |
| **编译期元编程** | 声明式宏 `macro`（语句位 + 表达式/值位，宏卫生）+ 过程宏 `@macro`/`quote` + `@eval` 编译期常量/只读常量表 + `@derive` |
| **算子系统** | `operator:`/`@op("+")` 运算符重载 + `|>` 算子管道 |
| **GPU 支持** | CUDA 后端，`MYP_GPU=1` 激活 |
| **零依赖标准库** | 42 个模块，纯 MYP 实现 |
| **LSP 集成** | 补全、悬停、跳转定义、文档符号 |

## 🚀 快速开始

### 编译安装

```bash
# 依赖: LLVM 21（含 llc/opt/ld.lld 后端工具）、CMake 3.20+、GCC/lld
mkdir build && cd build
cmake .. -DCMAKE_PREFIX_PATH=/usr/lib/llvm-21/lib/cmake/llvm   # 可选 -DMYP_ENABLE_GPU=OFF 跳过 GPU 支持
make -j$(nproc)                                                # 或 cmake --build . -j$(nproc)
```

构建自动完成两条链并产出 `build/mypc`（用户级编译器）：

- **C++ oracle 链**：`mypc-seed`（LLVM 21 直接编译的参考实现，前端对拍契约 `--frontend-dump`）。
- **自举链**：`mypc-seed` 编译 `tools/selfhost/*.myp` → `myp_self`（stage-0）→ 自编 →
  `myp_self2`（stage-1）→ 再自编 → `myp_self3`（stage-2）；`scripts/bootstrap_install.sh`
  **MD5 门禁**校验 `myp_self2 == myp_self3` 字节一致（自举成立）→ 安装 `myp_self2` 为 `build/mypc`。
- **运行时归档**：`runtime_myp/*.myp` 由 `myp_self` 编译成 `libmyp_rt_myp.a`（MYP 运行时，
  de-gcc 迁移），生成程序默认**仅 MYP 运行时链接**（输出 `(MYP runtime only)`）。

主要产物（`build/`）：`mypc`（自举编译器）、`mypc-seed`（oracle）、`myp_self/self2/self3`、
`myp`（包管理）、`myp_fmt2`/`myp_viz2`（自举格式化/可视化）、`myp_lsp`/`myp_debug`，
以及 `libmyp_rt_myp.a`（MYP 运行时归档）/ `libmyp_rt.a`（C 运行时）。

验证构建：

```bash
MYPCC=./build/mypc bash tests/run_tests.sh    # 全量回归（回归/负测试/测试框架/自举/GPU）
```

### Hello World

MYP 事件驱动模型里，输出逻辑放在组件的 `action` 中（`main` 只做接线）——最简单的写法
是 `@startup` + `mypc run`（无需手写 `main`）：

```myp
// hello.myp
import env;

class Hello {
    action:
        @startup void go() {
            Console.writeLine("Hello, MYP!");
        }
}
```

```bash
./build/mypc run hello.myp
# 输出: Hello, MYP!
```

## 📚 语言速览

### 组件与映射

```myp
class Sensor {
    action:
        void read() { valueRead(25.5); }
    event:
        valueRead(double temp);
}

class Display {
    action:
        void show(double v) {
            Console.writeLine("温度: " + v);
        }
}

int main() {
    Sensor sensor = new Sensor();
    Display display = new Display();

    mapping() {
        Sensor.valueRead -> Display.show;  // 声明式接线（节点用类名）
    }
    return 0;
}
```

### 并行计算

```myp
import atomic;

double[1000] tally;
@parallel for (int i = 0; i < 1000; i = i + 1) {
    Atomic.addDouble(tally, i, i * 1.5);  // 线程安全累加
}
```

### 协程与异步 IO

```myp
import env;
import coro;
import async;
import time;

class Worker {
    action:
        @coro long run() {
            Console.writeString("W:start\n");
            await Async.sleep(100);       // 异步睡眠：挂起本协程，不阻塞线程
            Console.writeString("W:woke\n");
            return 0;
        }
}

class Main {
    action:
        @startup void run() {
            Worker w = new Worker();
            long h = w.run();             // 创建并首启协程，返回 handle
            for (int i = 0; i < 10; i++) {
                Coro.scheduler();         // 自动调度：推进所有就绪协程
                Time.sleep(20);
            }
        }
}

int main() { Main m = new Main() @thread; return 0; }
```

### 完整语法

详细语法见 [编程手册](docs/manual.md) 和 [设计文档](docs/design.md)。

## 📦 标准库（42 模块）

| 类别 | 模块 |
|------|------|
| **基础 I/O** | `env`（控制台）、`io`（文件）、`text`（字符串）、`regex`、`base64` |
| **数据结构** | `collections`：`ArrayList`、`HashMap`、`Set`、`Queue`、`Stack`、`Deque`、`PriorityQueue`、`LinkedList`、`Sort`、`StrHashMap`；`option`（`Option<T>`/`T?` 可空）、`setops`（集合运算） |
| **数学** | `math`（三角/双曲/反三角/常数）、`random`（均匀/正态/指数/泊松等分布） |
| **时间日期** | `time`、`timeline`、`date` |
| **文件系统** | `fs`（路径/目录遍历） |
| **网络** | `net`（TCP 客户端/服务器）、`http`（HTTP 客户端） |
| **进程** | `process`（命令执行/输出捕获） |
| **命令行** | `args`（参数解析）、`env`（环境变量） |
| **内存** | `memory`（malloc/free/realloc 裸内存 + Memory 类 + `liveObjectCount` 诊断） |
| **并发** | `atomic`、`barrier`、`future`、`pool`、`sync`（Mutex/RWLock/CondVar/Semaphore）、`coro`（协程调度）、`async`（统一异步 IO：定时器/套接字/文件）、`channel`（协程通道）|
| **错误处理** | `result`（`Result<T,E>` 二态容器）、`error`（异常类型分层） |
| **工具** | `fmt`（printf 风格格式化）、`crypto`（CRC32/MD5/SHA）、`logger`、`json`、`test`、`stream`、`rtti`（运行时类型信息） |
| **图形 / GPU** | `sdl`（SDL2）、`ui`（终端 TUI）、`cuda`（CUDA GPU 编程） |

## 🛠️ 工具链

| 工具 | 用途 |
|------|------|
| `mypc` | 编译器（编译/链接/格式化/`run` 仿 go run 直接运行） |
| `myp` | 包管理（init/build/install/run） |
| `myp_viz` | 可视化（生成 DOT 图） |
| `myp_lsp` | 语言服务器（LSP） |
| `myp_fmt` | 代码格式化 |
| `myp_fmt2` / `myp_viz2` | 自举版格式化器 / 可视化器（MYP 实现，与 C++ 版字节级对拍） |
| `myp_debug` | 调试适配器（DAP ↔ gdb 桥，VS Code 断点/单步） |
| `myp_self` / `myp_self2` | 自举编译器（MYP 写的 mypc，含 GPU NVPTX 发射，两级自举成立） |
| `tools/codegen` | schema 驱动代码生成框架（serde/ffi/autodiff/idl/orm/embed/dsl/infer_ops） |

### VS Code 扩展

`vscode-myp/` 提供语法高亮和 LSP 智能编辑（补全、悬停、跳转定义）。

## ⚙️ 编译选项 / 环境变量 / 无运行时构建（freestanding）

**常用编译选项**（完整列表以 `mypc --help` 及各子命令 usage 为准）：
| 选项 | 作用 |
|------|------|
| `-o <path>` | 输出路径（默认 `<file>.out`） |
| `--emit-llvm` | 仅生成 `<out>.ll`（跳过链接，便于检查 IR） |
| `--stdlib <path>` | 指定标准库目录 |
| `mypc run <file> [args]` | 仿 `go run` 编译并运行 |
| `mypc fmt [--check] <file>` | 格式化（`--check` 仅校验） |
| `mypc --bootstrap` | 自举不动点校验（2 级 MD5 门禁） |
| `--freestanding` | **无 libc/CRT/runtime 的静态 ELF**（codegen 直发 `_start` + syscall 入口，链接免 gcc；已支持，实验/裸机向） |

**环境变量**（常用；内部调试/实验变量众多且随版本变化，不在此穷举——见源码与 CHANGELOG）：
| 变量 | 作用 |
|------|------|
| `MYP_GPU=1` | 启用 CUDA GPU 后端 |
| `MYP_RT_MYP=<归档>` | 强制以 MYP 运行时归档链接（de-gcc：无 libmyp_rt.a/gcc，仅 MYP runtime + libc） |
| `MYP_STDLIB=<路径>` | 默认标准库目录（等效 `--stdlib`） |

## 🧪 测试

```bash
bash tests/run_tests.sh          # 全量回归（编译+运行比对 + 负测试 + 测试框架 + 自举 + LSP）
# 回归测试: 114 通过, 0 失败
# 负测试:   234 通过, 0 失败
# 测试框架: 191 通过, 0 失败
# 自举包管理 2 / 自举格式化 1 / 自举可视化 1 / mypc run 1 / LSP 1 / 协程栈警告 1 / 无崩溃 1
# 总计:     547 通过, 0 失败
# 注：计数随版本增长，以 tests/run_tests.sh 实际输出为准。
bash tests/run_tests_asan.sh     # ASAN（AddressSanitizer）回归
bash tests/run_tests_tsan.sh     # TSan（ThreadSanitizer）回归
bash tests/run_tests_O2.sh       # -O2 优化回归
bash tests/test_myp_bootstrap.sh # 自举不动点（myp_self2 == myp_self3 字节一致，16/16）
bash tests/test_myp_self.sh      # selfhost 对拍（tokens/ast/sema 字节一致，95/95）
bash tests/test_myp_fmt.sh       # 自举格式化器对拍
bash tests/test_myp_viz.sh       # 自举可视化器对拍
bash tests/test_myp_gpu.sh       # GPU CPU 回退（RUN_GPU_TESTS=1）
bash tests/parity_matrix.sh      # 双编译器 parity 矩阵（oracle vs selfhost 同套差分）
bash tests/stress/run_stress.sh  # 压力测试（内存/线程/协程/跨线程 ARC）
```

## 🏗️ 项目结构

```
MYPLanguage/
├── src/              # 编译器源码 (C++17)
│   ├── lexer/        # 词法分析
│   ├── parser/       # 语法分析（parser / parser_expr / parser_stmt）
│   ├── ast/          # AST 定义
│   ├── sema/         # 语义分析（sema / sema_expr / symbol_table / type）
│   ├── eval/         # @macro 过程宏解释器（编译期执行）
│   ├── codegen/      # LLVM 代码生成（codegen / codegen_class / codegen_stmt / codegen_expr / codegen_gpu）
│   ├── macro/        # 宏展开（M3 声明式 + @derive 派生）
│   ├── runtime/      # C 运行时（runtime.c / runtime_gpu.c / runtime_lib.c）+ stdlib/bridges（按需链接：json/net/process/regex/base64/date/hash/sdl…）
│   ├── lsp/          # 语言服务器
│   ├── dap/          # DAP 调试适配器
│   └── fmt/          # 格式化
├── include/mylang/   # 头文件
├── stdlib/           # 标准库 (.myp，42 模块)
├── runtime_myp/      # MYP 运行时（runtime 的 MYP 实现，de-gcc 迁移：shadow C runtime → 归档 `libmyp_rt_myp.a`，仅 MYP 链接 `(MYP runtime only)`；进度见 runtime_myp/MIGRATION_STATUS.md，构建注意：改 runtime_myp 须手动重建归档再重连 mypc）
├── tools/            # 自举工具链（pm 包管理 / fmt 格式化 / viz 可视化 / selfhost 自举编译器 / codegen 代码生成，MYP 实现）
├── examples/         # 示例（含 BNCTDoseEngine 剂量引擎等；深度学习框架已独立 → mypdeeplearning）
├── mypview/          # 通用 UI 框架（零 MOS 依赖，声明式 UIX + MVVM）
├── MOS/              # MYP 原生操作系统（内核/服务/应用，x86 Linux）
├── vscode-myp/       # VS Code 扩展
├── bench/            # 性能基准
├── scripts/          # 构建/工具脚本
├── cmake/            # CMake 配置（含 MinGW 交叉编译 toolchain）
├── logo/             # 语言 Logo
├── docs/             # 文档
└── tests/            # 测试套件（含 tests/stress/ 压力测试）
```

## 🧠 深度学习框架（已独立成仓）

深度学习推理/训练框架已从本仓库迁出为独立项目 **mypdeeplearning**：

- 仓库：`https://gitee.com/tomatosoft_0/mypdeeplearning.git`
- 内容：`examples/deeplearning/`（ONNX + 声明式 JSON 双源；推理 + 训练；CPU + GPU；dl / infer / infer_tests / json_tool / train / llm / diffusion / docs）
- 独立构建：仓库内 `./build.sh`（工具链解析：`MYPC`/`MYP_STDLIB` → 同级 myp-language → `./bootstrap.sh` 自动获取）
- 文档：`examples/deeplearning/docs/manual.md`（中）/ `manual_EN.md`（英）等

历史演进记录保留在本仓库 git 历史（原 `examples/deeplearning/CHANGELOG.md` 已随迁）。

## 🎯 BNCT Dose Engine

仓库自带完整的[硼中子俘获治疗剂量模拟引擎](https://github.com/laomen/BNCTDoseEngine)：

- 多线程粒子输运（`@parallel for`，16 线程）
- H-1 / O-16 / B-10 物理过程
- 事件驱动架构，1e9 粒子规模
- 性能：5M 粒子 ~3s，加速比 ~10x

## 📄 文档

- [编程手册 (中文)](docs/manual.md)
- [English Manual](docs/manual_en.md)
- [设计文档](docs/design.md)
- [语法模板](docs/syntax-template.md)
- [示例代码](examples/)

## 📝 License

[![License: MIT](https://img.shields.io/badge/License-MIT-yellow.svg)](https://opensource.org/licenses/MIT)

MIT License — 宽松许可证，可自由使用/修改/商用（含 runtime 与 stdlib）。
详见 [LICENSE](LICENSE)。
