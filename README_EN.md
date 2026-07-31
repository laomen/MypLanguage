# MYP Programming Language

> Event-Driven Component Language | LLVM 21 Backend | Built-in GPU Support

**🌐 [中文](README.md)**

MYP is an **event-driven component** programming language built around `class` + `action:` / `event:` as architectural units, assembled declaratively via `mapping()`. The compiler is based on LLVM 21 and produces native executables.

## ✨ Key Features

| Feature | Description |
|---------|-------------|
| **Event-Driven Components** | Components communicate only via events, assembled with `mapping()` |
| **Actor-Style Concurrency** | `@thread` instances are naturally isolated, no locking needed |
| **Data Parallelism** | `@parallel for` auto-parallelization with a work-stealing thread pool |
| **Generics** | `ArrayList<T>`, `HashMap<K,V>`, `Queue<T>` and more |
| **Interface Polymorphism** | `interface` + vtable dispatch (fat pointers) |
| **GPU Support** | CUDA backend, activated with `MYP_GPU=1` |
| **Zero-Dependency Stdlib** | 34+ modules, pure MYP implementations |
| **LSP Integration** | Completion, hover, go-to-definition, document symbols |

## 🚀 Quick Start

### Build & Install

```bash
# Dependencies: LLVM 21, CMake 3.20+, GCC
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
# Output: Hello, MYP!
```

## 📚 Language Overview

### Components & Mapping

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
            Console.writeLine("Temperature: " + v);
        }
}

int main() {
    Sensor sensor = new Sensor();
    Display display = new Display();

    mapping() {
        sensor.valueRead -> display.show;  // declarative wiring
    }
    return 0;
}
```

### Parallel Computing

```myp
import atomic;

double[1000] tally;
@parallel for (int i = 0; i < 1000; i = i + 1) {
    Atomic.addDouble(tally, i, i * 1.5);  // thread-safe accumulation
}
```

### Full Syntax

See the [Programming Manual](docs/manual_en.md) and [Design Document](docs/design.md) for full details.

## 📦 Standard Library (34+ Modules)

| Category | Modules |
|----------|---------|
| **Basic I/O** | `env` (console), `io` (files), `text` (strings), `regex`, `base64` |
| **Data Structures** | `collections`: `ArrayList`, `HashMap`, `Set`, `Queue`, `Stack`, `Deque`, `PriorityQueue`, `LinkedList`, `Sort`, `StrHashMap` |
| **Mathematics** | `math` (trig/hyperbolic/inverse/constants), `random` (uniform/normal/shuffle) |
| **Time & Date** | `time`, `timeline`, `date` |
| **File System** | `fs` (paths/directory traversal) |
| **Networking** | `net` (TCP client/server) |
| **Process** | `process` (command execution/output capture) |
| **CLI** | `args` (argument parsing), `env` (environment variables) |
| **Memory** | `memory` (malloc/DynamicArray) |
| **Concurrency** | `atomic`, `barrier`, `future`, `pool` |
| **Utilities** | `logger`, `json`, `test`, `stream` |
| **Graphics** | `sdl` (SDL2), `ui` (terminal TUI) |
| **Experimental** | `coro` (coroutines) |

## 🛠️ Toolchain

| Tool | Purpose |
|------|---------|
| `mypc` | Compiler (compile/link/format) |
| `myp` | Package manager (init/build/install/run) |
| `myp_viz` | Visualization (DOT graph generation) |
| `myp_lsp` | Language server (LSP) |
| `myp_fmt` | Code formatter |

### VS Code Extension

`vscode-myp/` provides syntax highlighting and LSP smart editing (completion, hover, go-to-definition).

## 🧪 Testing

```bash
bash tests/run_tests.sh
# Regression tests: 55 passed, 0 failed
# Negative tests:   10 passed, 0 failed
# Total:            66 passed, 0 failed
```

## 🏗️ Project Structure

```
MYPLanguage/
├── src/              # Compiler source (C++17)
│   ├── lexer/        # Lexical analysis
│   ├── parser/       # Syntax analysis
│   ├── sema/         # Semantic analysis
│   ├── codegen/      # LLVM code generation
│   ├── runtime/      # C runtime
│   ├── lsp/          # Language server
│   └── fmt/          # Formatter
├── include/mylang/   # Headers
├── stdlib/           # Standard library (.myp)
├── BNCTDoseEngine/   # BNCT dose simulation engine (example)
├── vscode-myp/       # VS Code extension
├── docs/             # Documentation
└── tests/            # Test suite
```

## 🎯 BNCT Dose Engine

The repository ships a complete [Boron Neutron Capture Therapy dose simulation engine](https://github.com/laomen/BNCTDoseEngine):

- Multi-threaded particle transport (`@parallel for`, 16 threads)
- H-1 / O-16 / B-10 physics
- Event-driven architecture, 1e9 particle scale
- Performance: 5M particles ~3s, ~10x speedup

## 📄 Documentation

- [Programming Manual (Chinese)](docs/manual.md)
- [Programming Manual (English)](docs/manual_en.md)
- [Design Document](docs/design.md)
- [Syntax Template](docs/syntax-template.md)
- [Examples](examples/)

## 📝 License

MIT License
