# MYP 编程语言

> 事件驱动组件语言 | LLVM 21 后端 | 内置 GPU 支持

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
| **算子系统** | `operator:`/`@op("+")` 运算符重载 + `|>` 算子管道（v2.4+） |
| **GPU 支持** | CUDA 后端，`MYP_GPU=1` 激活 |
| **零依赖标准库** | 34+ 个模块，纯 MYP 实现 |
| **LSP 集成** | 补全、悬停、跳转定义、文档符号 |

## 🚀 快速开始

### 编译安装

```bash
# 依赖: LLVM 21, CMake 3.20+, GCC
mkdir build && cd build
cmake .. -DCMAKE_PREFIX_PATH=/usr/lib/llvm-21/lib/cmake/llvm
make -j$(nproc)
```

### Hello World

```myp
// hello.myp
import env;

int main() {
    Console.writeLine("Hello, MYP!");
    return 0;
}
```

```bash
./build/mypc hello.myp
./hello.out
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
        sensor.valueRead -> display.show;  // 声明式接线
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

### 完整语法

详细语法见 [编程手册](docs/manual.md) 和 [设计文档](docs/design.md)。

## 📦 标准库（34+ 模块）

| 类别 | 模块 |
|------|------|
| **基础 I/O** | `env`（控制台）、`io`（文件）、`text`（字符串）、`regex`、`base64` |
| **数据结构** | `collections`：`ArrayList`、`HashMap`、`Set`、`Queue`、`Stack`、`Deque`、`PriorityQueue`、`LinkedList`、`Sort`、`StrHashMap` |
| **数学** | `math`（三角/双曲/反三角/常数）、`random`（均匀/正态/洗牌） |
| **时间日期** | `time`、`timeline`、`date` |
| **文件系统** | `fs`（路径/目录遍历） |
| **网络** | `net`（TCP 客户端/服务器） |
| **进程** | `process`（命令执行/输出捕获） |
| **命令行** | `args`（参数解析）、`env`（环境变量） |
| **内存** | `memory`（malloc/DynamicArray） |
| **并发** | `atomic`、`barrier`、`future`、`pool`、`coro`（协程）、`channel`（协程通道）|
| **工具** | `logger`、`json`、`test`、`stream` |
| **图形** | `sdl`（SDL2）、`ui`（终端 TUI） |

## 🛠️ 工具链

| 工具 | 用途 |
|------|------|
| `mypc` | 编译器（编译/链接/格式化） |
| `myp` | 包管理（init/build/install/run） |
| `myp_viz` | 可视化（生成 DOT 图） |
| `myp_lsp` | 语言服务器（LSP） |
| `myp_fmt` | 代码格式化 |

### VS Code 扩展

`vscode-myp/` 提供语法高亮和 LSP 智能编辑（补全、悬停、跳转定义）。

## 🧪 测试

```bash
bash tests/run_tests.sh
# 回归测试: 67 通过, 0 失败
# 负测试:   25 通过, 0 失败
# 总计:     94 通过, 0 失败
```

## 🏗️ 项目结构

```
MYPLanguage/
├── src/              # 编译器源码 (C++17)
│   ├── lexer/        # 词法分析
│   ├── parser/       # 语法分析
│   ├── sema/         # 语义分析
│   ├── codegen/      # LLVM 代码生成
│   ├── runtime/      # C 运行时
│   ├── lsp/          # 语言服务器
│   └── fmt/          # 格式化
├── include/mylang/   # 头文件
├── stdlib/           # 标准库 (.myp)
├── BNCTDoseEngine/   # BNCT 剂量模拟引擎 (示例)
├── vscode-myp/       # VS Code 扩展
├── docs/             # 文档
└── tests/            # 测试套件
```

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
