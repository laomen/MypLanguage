# MYP Programming Language

> Event-Driven Component Language | LLVM 21 Backend | Built-in GPU Support

<p align="center">
  <img src="logo/logo2.png" alt="MYP Logo" width="460">
</p>

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
| **Coroutines + Async I/O** | `@coro`/`await` user-space coroutines + `@async` unified async abstraction (timers/sockets/file executor) + `Coro.waitAnyOf` mixed waits |
| **Error Handling** | `Result<T,E>` / `Option<T>`/`T?` containers + layered `catch (Error)` exceptions |
| **Automatic Memory Mgmt** | ARC for class instances (automatic reference counting, additive, no new syntax) |
| **Operator System** | `operator:`/`@op("+")` overloading + `|>` operator pipe |
| **GPU Support** | CUDA backend, activated with `MYP_GPU=1` |
| **Zero-Dependency Stdlib** | 39 modules, pure MYP implementations |
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

### Coroutines & Async I/O

```myp
import env;
import coro;
import async;
import time;

class Worker {
    action:
        @coro long run() {
            Console.writeString("W:start\n");
            await Async.sleep(100);       // async sleep: suspend this coroutine, don't block the thread
            Console.writeString("W:woke\n");
            return 0;
        }
}

class Main {
    action:
        @startup void run() {
            Worker w = new Worker();
            long h = w.run();             // create + first-run the coroutine, returns its handle
            for (int i = 0; i < 10; i++) {
                Coro.scheduler();         // auto-schedule: advance all ready coroutines
                Time.sleep(20);
            }
        }
}

int main() { Main m = new Main() @thread; return 0; }
```

### Full Syntax

See the [Programming Manual](docs/manual_en.md) and [Design Document](docs/design.md) for full details.

## 📦 Standard Library (39 Modules)

| Category | Modules |
|----------|---------|
| **Basic I/O** | `env` (console), `io` (files), `text` (strings), `regex`, `base64` |
| **Data Structures** | `collections`: `ArrayList`, `HashMap`, `Set`, `Queue`, `Stack`, `Deque`, `PriorityQueue`, `LinkedList`, `Sort`, `StrHashMap`; `option` (`Option<T>`/`T?` nullable), `setops` (set operations) |
| **Mathematics** | `math` (trig/hyperbolic/inverse/constants), `random` (uniform/normal/exponential/poisson distributions) |
| **Time & Date** | `time`, `timeline`, `date` |
| **File System** | `fs` (paths/directory traversal) |
| **Networking** | `net` (TCP client/server), `http` (HTTP client) |
| **Process** | `process` (command execution/output capture) |
| **CLI** | `args` (argument parsing), `env` (environment variables) |
| **Memory** | `memory` (malloc/free/realloc raw memory + Memory class + `liveObjectCount` diagnostics) |
| **Concurrency** | `atomic`, `barrier`, `future`, `pool`, `sync` (Mutex/RWLock/CondVar/Semaphore), `coro` (coroutine scheduler), `async` (unified async I/O: timers/sockets/files), `channel` |
| **Error Handling** | `result` (`Result<T,E>` two-state container), `error` (layered exception types) |
| **Utilities** | `fmt` (printf-style formatting), `crypto` (CRC32/MD5/SHA), `logger`, `json`, `test`, `stream`, `rtti` (runtime type info) |
| **Graphics / GPU** | `sdl` (SDL2), `ui` (terminal TUI), `cuda` (CUDA GPU programming) |

## 🛠️ Toolchain

| Tool | Purpose |
|------|---------|
| `mypc` | Compiler (compile/link/format/`run` go-style direct execution) |
| `myp` | Package manager (init/build/install/run) |
| `myp_viz` | Visualization (DOT graph generation) |
| `myp_lsp` | Language server (LSP) |
| `myp_fmt` | Code formatter |
| `myp_fmt2` / `myp_viz2` | Self-hosted formatter / visualizer (in MYP, byte-identical to C++ versions) |

### VS Code Extension

`vscode-myp/` provides syntax highlighting and LSP smart editing (completion, hover, go-to-definition).

## 🧪 Testing

```bash
bash tests/run_tests.sh          # full regression (compile+run compare + negative + self-hosted)
# Regression tests: 127 passed, 0 failed
# Negative tests:   47 passed, 0 failed
# Total:            181 passed, 0 failed
bash tests/run_tests_asan.sh     # ASAN (AddressSanitizer) regression
```

## 🏗️ Project Structure

```
MYPLanguage/
├── src/              # Compiler source (C++17)
│   ├── lexer/        # Lexical analysis
│   ├── parser/       # Syntax analysis (parser / parser_expr / parser_stmt)
│   ├── sema/         # Semantic analysis (sema / sema_expr)
│   ├── codegen/      # LLVM code generation (codegen / codegen_class / codegen_stmt / codegen_expr / codegen_gpu)
│   ├── runtime/      # C runtime
│   ├── lsp/          # Language server
│   └── fmt/          # Formatter
├── include/mylang/   # Headers
├── stdlib/           # Standard library (.myp)
├── tools/            # Self-hosted toolchain (pm / fmt / viz, in MYP)
├── logo/             # Language logo
├── BNCTDoseEngine/   # BNCT dose simulation engine (example)
├── vscode-myp/       # VS Code extension
├── docs/             # Documentation
└── tests/            # Test suite (incl. tests/stress/: `bash tests/stress/run_stress.sh`)
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
