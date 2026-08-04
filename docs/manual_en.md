# MYP Programming Manual

> Version 2.3 | Event-Driven Component Language

---

## Table of Contents

1. [Quick Start](#1-quick-start)
2. [Basic Syntax](#2-basic-syntax)
3. [Type System](#3-type-system)
4. [Control Flow](#4-control-flow)
5. [Functions](#5-functions)
6. [Class Component System](#6-class-component-system)
7. [Struct Data Type](#7-struct-data-type)
8. [Events & Mapping](#8-events--mapping)
9. [Concurrent Programming](#9-concurrent-programming)
10. [Modules & Imports](#10-modules--imports)
11. [Standard Library](#11-standard-library)
12. [Compilation & Tools](#12-compilation--tools)
13. [Complete Example](#13-complete-example)

---

## 1. Quick Start

### Installation

```bash
# Build the MYP compiler
cd MYPLanguage/build
cmake .. -DCMAKE_PREFIX_PATH=/usr/lib/llvm-21/lib/cmake/llvm
make -j$(nproc)

# Verify installation
./build/mypc --help
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

### Compiler Options

```bash
mypc <file.myp>                  # Compile and link
mypc -o myapp <file.myp>         # Specify output filename
mypc -O2 <file.myp>              # Optimization level
mypc --emit-llvm <file.myp>      # Save LLVM IR to .ll file
mypc --trace <file.myp>          # Enable runtime event tracing
mypc --stdlib <path> <file.myp>  # Specify stdlib path
mypc --package-path <path> <file.myp>  # Specify local package search path
mypc --shared <file.myp>              # Build shared library (.so)
mypc --static <file.myp>              # Build static library (.a)
```

---

## 2. Basic Syntax

### Comments

```myp
// Single-line comment
/* Multi-line
   comment */
```

### Variable Declarations

```myp
// Explicit type
int count = 0;
double pi = 3.14159;
string name = "MYP";
bool flag = true;
char letter = 'A';

// Type inference (v2+)
var x = 42;         // int
var y = 3.14;       // double
var s = "hello";    // string

// Variables must be explicitly initialized
int a;              // Default-initialized to 0
```

### Literals

```myp
42          // Integer (auto-inferred as byte/short/int/long)
3.14        // Float (double)
1.0e-5      // Scientific notation
0xFF        // Hexadecimal
true false  // Boolean
'A' '\n'    // Character (supports \n \t \\ \' \" \0)
"hello"     // String (supports \n \t \\ \" \0)
null        // Null value
```

### String Interpolation (v2+)

```myp
var name = "world";
var msg = "Hello, $name!";   // → "Hello, world!"
var x = 42;
var s = "x = $x";            // → "x = 42"
```

### Operators

| Precedence | Category | Operators |
|------------|----------|-----------|
| 10 | Assignment | `=` `+=` `-=` `*=` `/=` `%=` |
| 9 | Pipe | `\|>` |
| 8 | Ternary | `? :` |
| 7 | Logical OR | `||` |
| 6 | Logical AND | `&&` |
| 5 | Equality | `==` `!=` |
| 4 | Relational | `<` `>` `<=` `>=` |
| 3 | Addition | `+` `-` |
| 2 | Multiplication | `*` `/` `%` |
| 1 | Unary | `!` `-` `++` `--` |
| 0 | Postfix | `.` `[]` `()` `++` `--` |

```myp
// Compound assignment
x += 5;      // x = x + 5
x *= 2;      // x = x * 2

// Increment / Decrement
x++;         // x = x + 1
--x;         // Prefix also works

// String concatenation
var msg = "Hello, " + "world!";  // string + string
var s = "count: " + x;           // string + int (auto-converted)

// Ternary operator
var max = a > b ? a : b;

// Range (v2+)
var r = 0..10;  // Range expression (0 to 10, inclusive)

// Pipe (v2.4+): A |> Op calls the operator component's transform (left-assoc)
// Op is an operator class name (auto-instantiated) or an instance (reused)
var v = A |> ScaleOp;
```

---

## 3. Type System

### Primitive Types

| Type | Description | Size |
|------|-------------|------|
| `byte` | Signed 8-bit | 8 |
| `short` | Signed 16-bit | 16 |
| `int` | Signed 32-bit | 32 |
| `long` | Signed 64-bit | 64 |
| `ubyte` | Unsigned 8-bit | 8 |
| `ushort` | Unsigned 16-bit | 16 |
| `uint` | Unsigned 32-bit | 32 |
| `ulong` | Unsigned 64-bit | 64 |
| `char` | Character (8-bit) | 8 |
| `float` | Single-precision float | 32 |
| `double` | Double-precision float | 64 |
| `bool` | Boolean | 1 |
| `string` | String pointer | pointer |
| `void` | No type | — |

### Numeric Promotion

Automatic implicit conversion between numeric types:

```
byte/char → short → int → long → float → double
```

```myp
int a = 42;
long b = a;       // ✅ int → long auto-promotion
double c = a;     // ✅ int → double auto-promotion
int d = b;        // ❌ long → int does NOT auto-demote
```

### Composite Types

```myp
int[] arr;              // Array
string[] names;         // String array
int[10] fixed;          // Fixed-size array
ClassName obj;          // Class type (pointer)
ClassName::StructType;  // Nested struct type
```

---

## 4. Control Flow

### If / Else

```myp
if (x > 0) {
    Console.writeLine("positive");
} else if (x == 0) {
    Console.writeLine("zero");
} else {
    Console.writeLine("negative");
}
```

### While

```myp
var i = 0;
while (i < 10) {
    Console.writeLong(i);
    i++;
}
```

### For

```myp
for (var i = 0; i < 10; i++) {
    Console.writeLong(i);
}
```

#### For with Empty Clauses

```myp
// Infinite loop (all clauses empty)
for (;;) {
    // loop body
}

// Only condition
for (; i < 10;) {
    // loop body
}
```

### Break / Continue

```myp
for (var i = 0; i < 10; i++) {
    if (i == 5) break;        // Exit loop
    if (i % 2 == 0) continue; // Skip even numbers
}
```

### Return

```myp
int add(int a, int b) {
    return a + b;
}
void log(string msg) {
    Console.writeLine(msg);
    return;  // Optional
}
```

### Exception Handling (try / catch / finally / throw)

MYP uses `try` / `catch` / `finally` / `throw` for structured exception handling, built on C `setjmp`/`longjmp` (one handler per try, thread-local).

#### Basic try/catch

```myp
try {
    int v = parseValue(s);
} catch (e) {                 // catch (e): catch-all, e is the string message
    Console.writeLine("failed: " + e);
}
```

catch clause forms:
- `catch (e)` — catch-all, `e` is the string message
- `catch (string e)` — explicitly catch a string exception
- `catch (ClassName e)` — catch exactly that exception class
- `catch (Error e)` — catch any exception object implementing the `Error` interface (`e.message()`)

#### Multiple catch, matched in order

```myp
try {
    ...
} catch (FileError e) {
    ...
} catch (ParseError e) {
    ...
} catch (e) {                 // catch-all
    ...
}
```

Unmatched exceptions propagate outward automatically; if no handler exists, the runtime prints `uncaught exception: <msg>` and aborts.

#### throw

```myp
throw "some message";      // string shortcut
throw new FileError();     // exception object (implements Error interface)
```

`throw;` inside a catch **rethrows** the current exception (message/type preserved; runs `finally` first if present):

```myp
try {
    doWork();
} catch (e) {
    log("failed");
    throw;                  // hand it to the outer handler
}
```

#### finally

The `finally` block runs on every exit path — normal end of try, a matching catch, exception propagation, and `return` / `break` / `continue`:

```myp
try {
    File f = new File();
    f.open(path, "r");
} finally {
    Console.writeLine("cleanup");   // runs on every path
}
```

#### Expression try

`try <expr> catch (e) <expr>` is an expression that yields a fallback on error:

```myp
int n = try parseInt(s) catch (e) -1;   // parseInt result on success, -1 on failure
```

#### Standard exceptions (`import error`)

Standard exception classes all implement the `Error` interface and can be caught via `catch (Error e)`:

| Exception | Purpose | Key properties |
|---|---|---|
| `FileError` | file operation failure | `op`, `path` |
| `IOError` | generic I/O failure | `op`, `detail` |
| `NetError` | network failure | `op`, `host`, `port` |
| `ParseError` | parse failure | `source`, `line`, `detail` |
| `JsonError` | JSON parse failure | `line`, `col`, `detail` |
| `ArgumentError` | bad argument | `arg`, `detail` |
| `MathError` | math domain error | `op`, `detail` |
| `IndexError` | index out of range | `index`, `size` |

Fill properties via setters, then throw (MYP classes have no parameterized constructors):

```myp
FileError e = new FileError();
e.setOp("open");
e.setPath("config.myp");
throw e;
```

#### Library integration

The `io` / `json` / `net` libraries throw standard exceptions on failure: `File.open` failure throws `FileError`, `new Json(...)` with invalid input throws `JsonError`, `TcpClient.connect` failure throws `NetError`.

```myp
import io;
try {
    File f = new File();
    f.open("/no/such/file", "r");
} catch (FileError e) {
    Console.writeLine(e.message());   // "file error: open /no/such/file"
}
```

#### Custom exception objects

Implement the `Error` interface to use any class as an exception object:

```myp
import error;
class MyError {
    interface class Error;
    action:
        string message() { return "my error: " + code_; }
        void setCode(int v) { code_ = v; }
    property:
        int code_;
}

try {
    MyError e = new MyError();
    e.setCode(42);
    throw e;
} catch (Error e) {
    Console.writeLine(e.message());   // "my error: 42"
}
```

> Full design: [`docs/exceptions.md`](exceptions.md).

---

## 5. Functions

### Top-Level Functions

```myp
int add(int a, int b) {
    return a + b;
}

int main() {
    var result = add(10, 20);  // 30
    return 0;
}
```

### main() Function Rules

`main()` is the program entry point with **strict restrictions** — this is central to MYP's event-driven model:

```myp
int main() {
    // ✅ Allowed: create instances
    Sensor sensor = new Sensor();
    Display display = new Display();

    // ✅ Allowed: declare mappings
    mapping() {
        sensor.valueRead -> display.show;
    }

    // ❌ Forbidden: direct method calls
    sensor.readValue();       // Compile error

    // ❌ Forbidden: property access
    sensor.propertyName = 42; // Compile error

    return 0;
}
```

> **Principle**: main() does only "wiring", not "operations". All logic lives in class actions/functions.

### main(argc, argv) (v2+)

```myp
int main(int argc, string[] argv) {
    // argc: number of command-line arguments
    // argv: array of argument strings
    return 0;
}
```

---

## 6. Class Component System

### Three-Section Structure

MYP classes are event-driven components with three sections:

```myp
class Sensor {
    action:          // Callable methods (receive messages)
        void init(int id);
        float readValue() { return lastValue; }

    event:           // Firable events (send messages)
        valueRead(float temp);
        thresholdExceeded(float value);

    property:        // Internal state (private)
        int sensorId;
        float threshold;
        float lastValue;
}
```

### Section Rules

| Section | Contents | Rules |
|---------|----------|-------|
| `action:` | Methods (with return type) | Can be `;` declared or `{ }` implemented |
| `event:` | Events (no return type) | `;` declaration only |
| `property:` | Member variables | Variable declarations only |
| `function:` | Internal methods | Only callable within the class |
| `static:` | Static methods | No instance needed, `ClassName.method()` call |

### Access Control

```myp
class Counter {
    action:
        void increment() { count = count + 1; }  // ✅ this.count is readable/writable
        int getCount() { return count; }          // ✅
    property:
        int count;     // Private — inaccessible from outside
}

int main() {
    Counter c = new Counter();
    c.increment();     // ✅ action is callable
    c.count = 5;       // ❌ Compile error — property is private
    return 0;
}
```

### function: Internal Methods

```myp
class Calculator {
    action:
        int compute(int n) {
            return helper(n);  // ✅ Internal call to function
        }
    function:
        int helper(int n) {   // Not exposed to mapping
            return n * n;
        }
}
```

### static: Static Methods

```myp
class Math {
    static:
        double sqrt(double v) { return __myp_math_sqrt(v); }
}

int main() {
    var r = Math.sqrt(64.0);  // ✅ Direct call, no 'new' needed
    return 0;
}
```

### Interface Support (v2+)

```myp
// Declare an interface
interface ILogger {
    void log(string msg);
    onFlush();
}

// Class implements the interface
class FileLogger {
    interface class ILogger;
    action:
        void log(string msg) {
            Console.writeString("LOG: ");
            Console.writeLine(msg);
        }
    event:
        onFlush();
}

// The compiler validates that all interface actions/events
// are implemented by the class. Missing members cause a compile error.
```

---

## 7. Struct Data Type

### File-Level Struct

```myp
struct Vec2 {
    double x;
    double y;

    // Struct methods (optional)
    double length() {
        return Math.sqrt(x * x + y * y);
    }
}

// Usage
Vec2 v;
v.x = 3.0;
v.y = 4.0;
var len = v.length();  // 5.0
```

### Nested Struct

```myp
class Sensor {
    struct Config {
        int rate;
        bool enabled;
    }
}

// External definition (unfolded outside class)
struct Sensor::Config {
    int rate;
    bool enabled;
}
```

### struct vs class

| Dimension | `struct` | `class` |
|-----------|----------|---------|
| Allocation | Stack | Heap (`new`) |
| Passing | Value copy | Reference (pointer) |
| Field access | Public | Private |
| Event system | ❌ | ✅ Participates in mapping |
| `@thread` | ❌ | ✅ |

---

## 8. Events & Mapping

### Event Declaration

Events are declared in the `event:` section with no return type:

```myp
class Sensor {
    event:
        valueRead(float temp);
        thresholdExceeded(float value);
}
```

### Mapping Declaration

Mapping connects events to actions:

```myp
// Type-level mapping (file-level, global)
mapping() {
    Sensor.valueRead -> Display.showTemperature;
}

// Instance-level mapping (local, inside a function)
int main() {
    Sensor sensor;
    Display display;

    mapping() {
        sensor.valueRead -> display.showTemperature;
    }
}
```

### Event Chains

```myp
mapping() {
    A.event -> B.process -> C.onResult;
}
// Semantics: A fires event → B.process is called → result passed to C.onResult
```

### Multi-Target Mapping (v2+)

```myp
mapping() {
    // One event triggers multiple actions
    sensor.valueRead -> display.show, logger.log;

    // Equivalent to:
    sensor.valueRead -> display.show;
    sensor.valueRead -> logger.log;
}
```

### Mapping Semantics

- One event can map to multiple actions
- Multiple events can map to the same action
- Mapping establishes an event bus at runtime
- Same thread = synchronous processing, cross-thread = async delivery

---

## 9. Concurrent Programming

### @thread Annotation

```myp
int main() {
    // Without @thread: runs on the current thread
    Sensor sensor;

    // With @thread: runs on a dedicated thread
    Worker worker @thread;

    mapping() {
        sensor.valueRead -> worker.process;
    }
    return 0;
}
```

### @threadpool

```myp
// Create 4 Workers, each running on its own thread
Worker[4] pool @threadpool;

mapping() {
    sensor.valueRead -> pool[0].process;
}
```

### @startup Annotation

```myp
class Worker {
    action:
        @startup void run() {
            // Automatically executed when the thread starts
            Console.writeLine("worker started");
            taskCompleted(42);
        }
    event:
        taskCompleted(int result);
}
```

### Thread Model

```
Main thread: Create instances → Register mappings → Event loop
                                    ↕  Async
Worker thread: @startup → Event loop → Handle events → Fire new events
```

- Per-thread independent event queue (lock-free)
- Cross-thread communication via `mapping()` with automatic async delivery
- No explicit locking required

---

## 10. Modules & Imports

### Import Syntax

```myp
import env;              // Standard library module
import timeline;          // Standard library module
import "./helper.myp";    // User file (relative path)
import "/abs/lib.myp";    // User file (absolute path)
```

### Import Rules

- Standard library is looked up in the `stdlib/` directory
- User files support relative/absolute paths
- Automatic deduplication (same file is not imported twice)
- Recursive loading (imports within imported files are also loaded)
- Search paths: `--stdlib` → executable's `../stdlib/` → source file's `./stdlib/` → `--package-path` directory
- Package import: `import mylib;` searches `<package_path>/mylib/src/mylib.myp` or `<package_path>/mylib/mylib.myp`

### Project Organization

```myp
// sensors.myp — Sensor components
class TempSensor { /* ... */ }
class MotionSensor { /* ... */ }

// processing.myp — Processing logic
import "sensors.myp";
class DataProcessor { /* ... */ }

// main.myp — Top-level orchestration
import "processing.myp";
mapping() {
    TempSensor.valueRead -> DataProcessor.process;
}
```

---

## 11. Standard Library

### `import env` — Console I/O

```myp
import env;

Console.writeLine("hello");     // Print string + newline
Console.writeString("text");    // Print string (no newline)
Console.write(42);              // Print integer
Console.writeLong(1234567890L); // Print long integer
Console.writeFloat(3.14);       // Print float
Console.writeBool(true);        // Print boolean
Console.readString();           // Read line from stdin
Console.kbhit();                // Non-blocking key check
Console.getch();                // Non-blocking read character
```

### `import collections` — Collection Types

```myp
import collections;

// ArrayList<T> — dynamic array (capacity 1024)
ArrayList<int> list = new ArrayList<int>();
list.add(10);
list.add(20);
int first = list.get(0);   // 10
list.set(1, 30);
int n = list.size();        // 2

// HashMap<K,V> — hash table (capacity 1024, linear probing)
HashMap<int, string> map = new HashMap<int, string>();
map.put(1, "one");
map.put(2, "two");
string v = map.get(1, "?");  // "one"
bool has = map.contains(2);  // true
map.remove(1);

// Set<T> — hash set (capacity 1024)
Set<int> s = new Set<int>();
s.add(42);
s.add(17);
bool b = s.contains(42);     // true
s.remove(17);
int sz = s.size();
```

### `import math` — Math Functions

```myp
import math;

Math.sqrt(64.0);        // 8.0
Math.abs(-3.5);         // 3.5
Math.sin(3.14159);      // ~0
Math.cos(3.14159);      // ~-1
Math.pow(2.0, 10.0);    // 1024.0
Math.max(10, 20);       // 20
Math.min(10, 20);       // 10
Math.absInt(-42);       // 42
```

### `import time` — Time & Timers

```myp
import time;

long t = Time.nowMs();        // Current time (ms)
Time.sleep(1000);             // Sleep 1 second

// Timer (for use with mapping)
// Timer(eventName, delayMs, intervalMs)
// intervalMs = 0 for one-shot
```

### `import random` — Random Numbers

```myp
import random;

Random.init(12345);           // Set seed
int r = Random.next();         // [0, RAND_MAX]
int d = Random.below(10);      // [0, 10)
```

### `import text` — Text Processing

```myp
import text;

StringBuilder sb = new StringBuilder();
sb.append("Hello");
sb.append(", World");
string result = sb.toString();  // "Hello, World"
```

### `import coro` — Coroutines

MYP coroutines are ucontext-based user-space fibers: an `@coro`-annotated **class action method**
or **top-level function** + `await` to suspend + `Coro.resume` to resume (C1-C7 implemented).
Users access the scheduling/lifecycle API through the static class `Coro`.

> `await` is only allowed inside an `@coro` method or top-level `@coro` function. In a plain
> action / `function:` / `static:` section or a plain top-level function, `await` is a compile
> error: `'await' is only allowed inside an '@coro' method`.

**Declaring a coroutine method** (`@coro`, may take parameters; use `await` to suspend;
`@coro(stack=N)` sets the stack size in KB, default 128):

```myp
import env;     // Console
import coro;    // coroutines (static class Coro)

class Worker {
    property:
        string label_;
    action:
        void setLabel(string s) { label_ = s; }
        @coro void run() {                    // coroutine method (default 128KB stack)
            Console.writeString(label_); Console.writeString(":1\n");
            await;                            // suspend, yield control
            Console.writeString(label_); Console.writeString(":2\n");
        }
        @coro(stack=2048) void heavy() {      // deep recursion / large stack: 2MB
            await;
        }
}

// Top-level @coro function: no class wrapper needed; calling spawns and returns a handle
@coro long worker(long n) {
    long x = Coro.yield(n * 2);     // suspend, yield n*2; on resume x = value passed in
    return x + 100;                 // result read via Coro.result(h)
}
```

**Calling = spawning**: `obj.meth(args)` or a top-level `fn(args)` returns a `long` handle
(creates the coroutine and runs it up to the first `await`):

```myp
class Main {
    action:
        @startup void run() {
            Worker a = new Worker();  a.setLabel("A");
            long h = a.run();               // spawn, returns handle
            Console.writeString("main\n");
            Coro.resume(h, 0);              // resume (continue after await)
            Coro.destroy(h);                // cancel early (optional)
        }
}
```

**C2 — value passing and return values**:

```myp
class Worker {
    action:
        @coro void echo(int n) {
            int v = await n * 2;            // suspend passing out n*2; on resume v = passed-in value
            Console.writeString("v="); Console.write(v); Console.writeString("\n");
        }
        @coro int compute() {
            await;
            return 42;                       // return stored into result slot
        }
}

class Main {
    action:
        @startup void run() {
            Worker w = new Worker();
            long h = w.echo(5);
            long out = Coro.resume(h, 100); // pass 100 → v=100; out = yielded 10
            long hc = w.compute();
            Coro.resume(hc, 0);
            int r = Coro.result(hc);        // 42
        }
}
```

**User API (static class `Coro`)**:

```myp
Coro.scheduler();                              // auto-schedule: one step per ready coroutine (C3)
long r  = Coro.resume(h, val);                 // resume, pass val in; returns coroutine's yielded value
long v  = Coro.yield(val);                     // suspend, pass val out; returns passed-in value (= await expr)
long a  = Coro.isActive(h);                    // still active (1/0)
Coro.destroy(h);                               // destroy / cancel early
long r  = Coro.result(h);                      // read coroutine return value
long v  = Coro.waitEvent(eventId, val);        // block on an event (= await ClassName.eventName)
```

> `__myp_coro_*` are compiler internals (the `Coro` class is a built-in static class;
> codegen emits the underlying call directly). The symbols are **not registered** — user
> code calling them gets `undefined symbol`. `stdlib/coro.myp` exposes no FFI declarations;
> users only use the `Coro` static class.

**C3 — automatic scheduler**: spawned coroutines automatically join a ready queue;
`Coro.scheduler()` advances every ready coroutine by one await per round
(processes pending events first, then round-robin resumes) — no per-coroutine manual resume:

```myp
long h1 = a.run();
long h2 = b.run();
Coro.scheduler();   // every ready coroutine advances one await
Coro.scheduler();
```

**C4 — event waiting**: a coroutine can block on an event with `await ClassName.eventName`;
when the event is fired and dispatched, the coroutine is re-readied and driven by the scheduler:

```myp
class Signal {
    action:
        void send() { go(); }        // fire event by bare name inside a class action
    event:
        go();
}

@coro void waiter() {
    Console.writeString("waiting\n");
    await Signal.go;                 // block until the go event
    Console.writeString("got go\n");
}
```

**Coroutines + threads combined**: coroutine state is **thread-local** (a coroutine belongs to
its creating thread). Multiple `@thread` threads can create/schedule their own coroutines
independently:

```myp
class Main {
    action:
        @startup void run() {
            // run a coroutine on this thread
            Ping p = new Ping();
            long h = p.loop(3L);
            Worker w = new Worker() @thread;   // another thread (its @startup may run coroutines too)
            Coro.scheduler();
            Coro.scheduler();
            Coro.scheduler();
        }
}
```

> Thread-local concurrency (coroutines) + inter-thread parallelism (`@thread`); coroutine
> state is cleaned up automatically when a thread exits.

> Semantics: an `@coro` method or top-level `@coro` function call compiles to a spawn
> (`create` + arg slots + `set_entry` + first `resume`) and returns a `long` handle; `await`
> compiles to `__myp_coro_yield(val)` (`await expr` is an expression binding the full operand;
> after resume its value equals the value passed in by `resume`); an `@coro` body's `return val`
> is stored into its per-coroutine result slot, read via `Coro.result(h)`. Finished coroutines
> recycle their slot automatically and stacks are freed at process exit. A top-level `@coro`
> function's entry wrapper has no `this` slot (params start at slot 1); codegen pre-scans to
> create wrappers, so a coroutine function may be defined after its call site.

---

## 12. Compilation & Tools

### Compiler

```bash
# Compile and run
./build/mypc myapp.myp && ./myapp.out

# Custom output
./build/mypc myapp.myp -o /tmp/myapp

# Optimization
./build/mypc -O2 myapp.myp

# Event tracing (debug)
./build/mypc --trace myapp.myp
./myapp.out 2>trace.log

# Save LLVM IR
./build/mypc --emit-llvm myapp.myp
cat myapp.myp.ll

# Specify package path
./build/mypc --package-path myp_packages myapp.myp
```

### Test Framework

```bash
# Compile with test runner generation
./build/mypc --test mytests.myp && ./a.out
# Output:
# === MYP Test Runner ===
#   RUN: test_math
#   PASS: test_math
# === MYP Tests Complete ===
```

Mark test functions/actions with `@test` and use the `Test` class:

```myp
import test;

@test void test_example() {
    Test.assert(1 == 1);
    Test.assertEq(2 + 2, 4);
    Test.assertStrEq("hello", "hello");
    Test.report("test_example", true);
}
```

### Code Formatter

```bash
# Format files in-place
./build/mypc fmt source.myp

# Check-only mode
./build/mypc fmt --check source.myp

# Standalone formatter
./build/myp_fmt [--check|--stdout] source.myp
```

### myp — Package Manager CLI

`myp` is the MYP package management command-line tool, providing project initialization, building, and dependency management:

```bash
# Create a new package
myp init mylib
# Output:
#   mylib/package.myp
#   mylib/src/mylib.myp

# Build the current package
cd myapp
myp build

# Install a dependency
myp install /path/to/mylib
# → Copies to myp_packages/mylib/

# Build and run
myp run
```

### Package Structure

```
mypackage/              # Package root
├── package.myp          # Package metadata
│   name: mypackage
│   version: 0.1.0
│   depends: other_lib
├── src/
│   └── mypackage.myp    # Main source file
└── myp_packages/        # Installed dependencies (auto-managed)
    └── other_lib/
        ├── package.myp
        └── src/
            └── other_lib.myp
```

### Environment Variable

```bash
export MYP_PACKAGE_PATH=/path/to/packages:/path/to/more
```

### myp_viz — Visualization Tool

```bash
# Generate DOT graph
./build/myp_viz myapp.myp > graph.dot

# Render as PNG (requires graphviz)
dot -Tpng graph.dot -o graph.png

# One-step generation
./build/myp_viz myapp.myp | dot -Tpng -o graph.png
```

### myp_lsp — Language Server

`myp_lsp` is the MYP Language Server Protocol implementation, providing intelligent editing features:

```bash
# Start the language server (called by editors)
./build/myp_lsp --stdlib ./stdlib

# Debug: log LSP communication
./build/myp_lsp --stdlib ./stdlib 2>lsp.log
```

**Editor capabilities**:

| LSP Feature | Description |
|-------------|-------------|
| Diagnostics | Real-time compile errors on open/edit |
| Completion | Auto-complete keywords, class/method/property names |
| Hover | Show type signatures on mouse hover |
| Document Symbols | Outline view of classes, functions, enums |
| Go to Definition | Ctrl+click to jump to definition |

### VS Code Extension

MYP provides a VS Code extension (`vscode-myp/`) with syntax highlighting and LSP integration:

```bash
# Method 1: Copy to extensions directory
cp -r vscode-myp ~/.vscode/extensions/myp-lang.vscode-myp
# Restart VS Code to activate

# Method 2: Package and install (requires @vscode/vsce)
cd vscode-myp
npm install
npx vsce package
code --install-extension vscode-myp-*.vsix
```

Extension settings (search `myp` in VS Code settings):

| Setting | Description |
|---------|-------------|
| `myp.lspPath` | Path to `myp_lsp` (auto-detected by default) |
| `myp.stdlibPath` | Path to stdlib (auto-detected by default) |
| `myp.trace.server` | LSP trace log level |

## 13. Complete Example

### IoT Temperature Monitoring System

```myp
// ===== iot_monitor.myp =====
import env;
import timeline;
import math;

// === Sensor Component ===
class TempSensor {
    action:
        @startup void run() {
            // Read temperature every 2 seconds
            t.startInterval(2000);
        }
        double readValue() {
            // Simulate temperature reading
            return 20.0 + Math.sin(Timeline.now() / 1000.0) * 5.0;
        }
    event:
        temperatureRead(double value);
    property:
        Timeline t;
}

// === Alarm Component ===
class Alarm {
    action:
        void check(double v) {
            if (v > 25.0 && !alarming) {
                alarming = true;
                alarmTriggered(v);
            } else if (v <= 25.0) {
                alarming = false;
            }
        }
        void sound() {
            Console.writeLine("🚨 ALARM! Temperature too high!");
        }
    event:
        alarmTriggered(double value);
    property:
        bool alarming;
}

// === Logger Component ===
class Logger {
    action:
        void log(double v) {
            Console.writeString("[LOG] Temp: ");
            Console.writeFloat(v);
            Console.writeLine("°C");
        }
}

// === Display Component ===
class Display {
    action:
        void show(double v) {
            Console.writeString("Current: ");
            Console.writeFloat(v);
            Console.writeLine("°C");
        }
        void showAlert(double v) {
            Console.writeString("⚠️ High: ");
            Console.writeFloat(v);
            Console.writeLine("°C");
        }
}

// === System Architecture ===
int main() {
    TempSensor sensor = new TempSensor();
    Alarm alarm = new Alarm();
    Logger logger = new Logger();
    Display display = new Display();

    sensor.t = new Timeline();

    mapping() {
        // Temperature reading → display and log
        sensor.temperatureRead -> display.show, logger.log;

        // Temperature reading → alarm check
        sensor.temperatureRead -> alarm.check;

        // Alarm triggered → sound + prominent display
        alarm.alarmTriggered -> alarm.sound, display.showAlert;
    }

    return 0;
}
```

---

## Appendix: MYP Design Philosophy

### Core Principles of Event-Driven Components

1. **Components communicate only through events** — no direct method calls, no shared state
2. **Architecture as code** — reading `mapping()` reveals the system architecture
3. **Actor-style concurrency** — `@thread` instances are naturally isolated, no locking needed
4. **Declarative assembly** — adding/removing/replacing a component only needs one mapping line

### When to Use What?

| Construct | Purpose |
|-----------|---------|
| `class` + `action:` + `event:` | Event-driven component (primary architectural unit) |
| `class` + `function:` | Internal component helper logic |
| `class` + `static:` | Utility function namespace (e.g. Math) |
| `struct` | Pure data container (pass by value) |
| Top-level `function` | Pure computation functions |
| `action:` + `@startup` | Component initialization (auto-called) |
| `@thread` | Component needing a dedicated thread |
