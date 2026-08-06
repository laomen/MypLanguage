# MYP Programming Manual

> Version 3.0 | Event-Driven Component Language
> Language spec v1.0 (frozen): official EBNF in [grammar.md](grammar.md), versioning policy in [CHANGELOG.md](CHANGELOG.md).

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

### Type Alias `type X = ...` (v3.x, additive)

`type Name = Type;` aliases a type; `Name` can be used anywhere a type is expected.
`type` is a **contextual keyword** — only the top-level `type <Id> = <Type> ;` shape is a declaration.

```myp
type MyInt = int;
type Int3 = int[3];
type AliasAlias = MyInt;       // alias of alias

MyInt x = 42;                  // ≡ int x = 42;
```

### Tuple Types (v3.x, additive)

`(Type, Type, ...)` with ≥2 elements; supports **multi-value returns**, **destructuring**,
and **field access `t.N`**.

```myp
// Multi-value return
(int, string) getPair() { return (1, "x"); }

// Declarative destructuring
(int a, string b) = getPair();          // a=1, b="x"

// Tuple variable + field access
(int, int) t = (3, 4);
int c = t.0;                            // 3

// Assignment destructuring (variables must already exist)
int x; int y;
(x, y) = getPair();                     // x=1, y="x"

// Nested destructuring
((int p, int q), int z) = ((1, 2), 5);  // p=1, q=2, z=5
```

> Disambiguated from function types `(A, B) -> R` and lambdas `(a, b) => ...`;
> see `docs/tuple.md`.

### Nullable `Option<T>` / `T?` (v3.x, additive)

Explicit nullable wrapper avoiding bare `null` dereferences. Requires `import option;`.

```myp
import option;
Option<int> none = new Option<int>();      // none
Option<int> some = new Option<int>(42);    // some
int? maybe = new Option<int>(7);           // T? ≡ Option<T> (type position)
if (some.isSome()) {
    int v = some.get();                    // 42 (check isSome before get)
}
int safe = maybe.getOr(0);                 // safe access (default when none)
some.set(9);
some.clear();                              // back to none
```

> API: `isSome()`/`isNone()`/`get()`/`getOr(def)`/`set(v)`/`clear()`.

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

### For-in — Collection Iteration (§四-2)

`for-in` iterates four kinds of iterable with one uniform syntax (parentheses and element type optional):

```myp
// 1) Fixed array T[N] (compile-time length)
int[4] arr = ...;
for (x in arr) { ... }

// 2) slice<T> (uses .size())
slice<int> s = new slice<int>(3);
for (x in s) { ... }

// 3) Collection class — must provide size() + get(int) methods (de-facto iterator protocol, e.g. ArrayList<T>)
ArrayList<int> list = new ArrayList<int>();
list.add(10); list.add(20);
for (x in list) { ... }

// 4) range: for (i in a..b) ≡ for (int i = a; i < b; i++)  (exclusive upper bound)
for (i in 0..5) { ... }        // i = 0,1,2,3,4
```

Four forms:

```myp
for (x in coll) { ... }        // parenthesized, inferred element type
for (int x in coll) { ... }    // parenthesized, explicit element type
for x in coll { ... }          // no parentheses (inferred type only)
for (i in 0..5) { ... }        // no-parentheses range (i < 5, exclusive)
```

- The loop variable is re-declared each iteration (scope-level); `break` / `continue` and nesting are supported.
- The iterable expression is evaluated once; a collection-class temporary reference is released after the loop.
- Class elements (e.g. `Node[]`, `ArrayList<Node>`) follow ARC borrow semantics (retain on iteration, release at loop-variable scope exit) — verified zero-leak.
- **Not iterable** (compile error): dynamic arrays `int[]` (no runtime length — use `slice<T>` or a collection class), classes without `size()/get(int)`, non-collection types (e.g. `int`), and collections whose element type is an array.

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

Fill properties via setters (or pass them through a `@constructor` method), then throw:

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

### Value-Based Error Propagation: Result\<T, E\> (§五-3, additive)

Besides try/catch, `import result` provides **value-based error propagation**: `Result<T, E>`
is an Ok(value)/Err(error) two-state container — errors are passed explicitly as return values
(complementing the typed error hierarchy in `error.myp`).

```myp
import result;

Result<int, string> ok  = new Result<int, string>(42);   // ok
Result<int, string> bad = new Result<int, string>();     // err (E uninitialized)
bad.setErr("oops");

if (ok.isOk())  Console.write(ok.get());      // 42 (gate with isOk before get)
if (bad.isErr()) Console.writeString(bad.getErr());   // "oops"
int v = bad.getOr(-1);                        // safe access → -1
```

Factories (top-level generic functions):

```myp
Result<string, string> s = resultOk<string, string>("hi");
Result<int, string>    b = resultErr<int>("bad");   // T explicit, E inferred from arg
```

Combinators (exception-free error propagation):

```myp
Result<string, string> m = resultMap(f1, (int x) => { return "v" + x; });  // apply f only on ok
Result<int, string>    a = resultAndThen(f1, (int x) => { return resultOk(x * 3); });
Result<int, string>    e = resultMapErr(f2, (string e) => { return "E:" + e; });
```

Exception bridge `resultTry` (turns a possibly-throwing call into `Result<T, string>`,
error unified as a message):

```myp
Result<int, string> r = resultTry<int>(() => { return risky(); });
if (r.isErr()) Console.writeString(r.getErr());
// throw "msg" → err(msg); throw <Error object> → err(e.message())
// Layered errors: precise handling via concrete exception classes + catch,
// unified handling via the Error interface + resultTry.
```

> The combinators are **top-level generic functions** (a generic body cannot call other
> generic functions, so they construct `Result` directly). `tests/result` covers all paths.

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

### Generic Functions (v3.x, additive)

A function name can carry type parameters `T foo<T>(T x)`; calls use **explicit type
arguments** or **argument inference**.

```myp
T id<T>(T x) { return x; }
T max2<T>(T a, T b) { if (a > b) return a; return b; }

int a = id<int>(5);    // explicit
int b = id(7);         // inferred → T=int
string s = id("hi");   // T=string
```

- Generic functions are **monomorphized** per type argument (`id_int_inst`); the
  template itself emits no runtime code.
- `T[]` parameters infer the element type; on inference failure, explicit type
  arguments are required.

### Default & Named Parameters (v3.x, additive)

Parameters can carry default values; calls may omit parameters that have defaults,
or use `name = value` named arguments (which may be out of order).

```myp
// Definition: parameters with defaults may be omitted
int add(int a, int b = 100, int c = 200) { return a + b + c; }

add(1);            // 301 (b, c defaulted)
add(1, 2);         // 203 (c defaulted)
add(1, 2, 3);      // 6 (all given)

// Named arguments (out of order) — `name = value` matches by parameter name
add(1, c = 5);         // 106 (b defaults to 100)
add(1, b = 5, c = 6);  // 12
mul(b = 7, a = 6);     // 42 (named args may be reordered)

// Constructors / methods / static methods / struct construction all support them
Rect r = new Rect(h = 5);            // w defaults to 1
Vec2 v = Vec2(px = 3.0, py = 4.0);   // struct functional construction, named
g.greet(name = "Al", suffix = "?");  // method named arguments
Greeter.scale(5);                    // static method, default f=2
```

- Applies to: top-level functions, class methods (`action:`/`function:`), static
  methods, constructors (`new`), and struct construction.
- **Positional args fill the first N parameters in order; named args fill by name.**
  A parameter cannot be given both by position and by name.
- Default expressions are evaluated **at the call site** (usually constants); their
  type is checked against the parameter type at declaration.
- `name = value` in an argument is parsed as an assignment expression and re-interpreted
  as a named argument when the target identifier matches a parameter name — so **macro
  assignment arguments** (`repeat(3, v = v + 10)`, macro params `$n/$body` never match)
  are unaffected.
- Negative cases (compile-time errors): unknown/duplicate named argument, positional+
  named overlap, missing required argument, too many arguments, default-value type
  mismatch.

### First-Class Functions & Closures (v3.x, additive)

Function types `(A, B) -> R` are first-class values: assignable, passable as
arguments/returns, directly callable. Lambdas `(params) => { body }` create function
values that **capture by value**.

```myp
// Function-type variable + lambda
(int) -> int add1 = (int x) => { return x + 1; };
int r = add1(41);                       // 42

// Higher-order: function value as argument
int apply2(int v, (int) -> int f) { return f(v); }
int r2 = apply2(10, (int x) => { return x * 2; });   // 20

// Function returning a closure (captures n)
(int) -> int makeAdder(int n) { return (int x) => { return x + n; }; }
(int) -> int add5 = makeAdder(5);
int r3 = add5(3);                       // 8

// Generic higher-order: Option.map-style composition
Option<R> mapOpt<T, R>(Option<T> o, (T) -> R f) { ... }
```

- Runtime representation: fat pointer `{closure, call_fn}` + uniform tramp.
- Capture: scalars/strings are **deep-copied**, class references **shallow-copied**
  (shared instance); nested lambdas supported.

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

#### Generic Static Methods (v3.x, additive)

Methods in the `static:` section can carry type parameters — **generic static
methods**: templates are defined in stdlib/`@static class` and callable from any
module (enables `map`/`filter`/`reduce`).

```myp
@static class List {
    static:
        Option<R> map<T, R>(Option<T> o, (T) -> R f) {
            Option<R> r = new Option<R>();
            if (o.isSome()) r.set(f(o.get()));
            return r;
        }
        int foldInt<T>(ArrayList<T> arr, int init, (int, T) -> int f) { ... }
}

// Cross-module call: explicit type arguments + first-class function argument
Option<int> some = new Option<int>(5);
Option<string> m = List.map<int, string>(some, (int x) => { return "v" + x; });
```

- Monomorphized instance name `__gs_<Class>_<method>_<types>_inst`; the template
  itself emits no runtime code.
- Generic static methods have no `this`; support explicit type arguments and
  argument inference.

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

#### Default Methods (trait default implementation, v3.9.0, additive)

Interface methods may carry a **default body** — implementing classes that **omit** the
method inherit the default logic; overriding classes use their own:

```myp
interface IShape {
    double area();                       // signature-only → must be implemented
    double perimeter();
    string describe() {                  // default body → may be omitted
        return "area=" + area() + " perim=" + perimeter();
    }
}

class Circle {
    interface class IShape;
    action:
        double area() { return 3.14 * r * r; }
        double perimeter() { return 2 * 3.14 * r; }
        // no describe() → IShape.describe default is used (this.area → Circle_area)
    property: double r = 1.0;
}

class Square {
    interface class IShape;
    action:
        double area() { return side * side; }
        double perimeter() { return 4 * side; }
        string describe() { return "SQUARE(" + area() + ")"; }  // overrides default
    property: double side = 3.0;
}

IShape c = new Circle();   IShape s = new Square();
c.describe();   // "area=3.14 perim=6.28" (default; this.area dispatches to Circle)
s.describe();   // "SQUARE(9)" (Square override)
```

- **Semantics**: default methods are **specialized per class** (`__ifdef_<Iface>_<method>_<Class>`);
  inside the default body, `this.method()` / bare method calls resolve **statically to that
  concrete class**. A default calling another default method is also supported.
- **Constraint**: signature-only (no default body) interface methods **must** be implemented;
  this guarantees the abstract methods a default body references always exist.
- **Value**: adding a method to an interface doesn't break existing implementers (they get
  the default); shared composed logic lives in the interface itself.

#### Associated Types (v3.9.0, additive)

An interface may declare an **associated type** (Rust associated-type semantics) — interface
method signatures reference this abstract type, and **each implementing class binds a concrete
type** (`int`, `string`, custom classes, ...). The same interface can be instantiated by
implementers with different element types:

```myp
interface Container {
    type Item;                    // associated type declaration (abstract)
    bool contains(Item v);        // param references the associated type
    Item getVal();                // return references the associated type
}

class IntBox {
    interface class Container;
    type Item = int;              // bound to int
    action:
        bool contains(int v) { return v == val; }
        int getVal() { return val; }
    property: int val = 42;
}

class StrBox {
    interface class Container;
    type Item = string;           // bound to string
    action:
        bool contains(string v) { return v == val; }
        string getVal() { return val; }
    property: string val = "hi";
}
```

- **Binding**: an implementing class **must** bind with `type Item = int;` (otherwise a
  compile error; negative test `assoc_unbound`).
- **Direct reference**: a binding is referenced with `X::Item` syntax — `IntBox::Item ≡ int`,
  usable as a local variable, parameter, or return type:

```myp
IntBox::Item x = 5;          // ≡ int x = 5;
Container c = new IntBox();
bool r = c.contains(x);      // virtual-dispatch call on an interface variable
```

- **Generic constraints**: a generic class/function constrains `T` with `where T : I` and
  references the associated type as `T::Item` — at instantiation `T` binds the concrete class
  and `T::Item` monomorphizes to that class's bound type:

```myp
class Processor<T where T : Container> {
    action:
        T::Item peek(T c) { return c.getVal(); }      // returns associated type
        bool check(T c, T::Item v) { return c.contains(v); }
}

Processor<IntBox> pi = new Processor<IntBox>();
int iv = pi.peek(ib);        // T::Item = int → 42
Processor<StrBox> ps = new Processor<StrBox>();
string sv = ps.peek(sb);     // T::Item = string → "hi"
```

- **Value**: interface methods become **decoupled from the element type** — one interface
  adapts to arbitrary concrete types, and generic code keeps type safety without reimplementing
  per element type.

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

### Constructor (`@constructor` / name == class name)

`new ClassName(args)` **automatically calls the constructor** to initialize the object
(setting fields, allocating resources, validation). A constructor is an `action:`
(or `function:`) method annotated `@constructor`; **when the method name equals the class
name it is implicitly a constructor** (the annotation may be omitted, C++/Java-style):

```myp
class Window {
    action:
        @constructor
        Window() {                     // explicit @constructor: no-arg (no return type)
            x = 0; y = 0; w = 80; h = 24;
        }
        void Window(int px, int py, int pw, int ph) {  // name==class → implicit constructor
            x = px; y = py; w = pw; h = ph;
        }
    property:
        int x;
        int y;
        int w;
        int h;
}

int main() {
    Window a = new Window();                // no-arg constructor
    Window b = new Window(10, 5, 100, 50);  // overloaded constructor
    return 0;
}
```

- **Overloading**: same name (= class name) with different parameters = multiple
  constructors; `new C(args)` matches by argument types (implicit numeric promotion
  `int → long → double`); no match / ambiguity → compile error.
- **Order**: on `new` — allocate instance → apply property defaults → run constructor
  body (may override defaults).
- **Generics**: `new Box<double>(1.5)` binds the monomorphized instance's constructor;
  `T` resolves to `double`.
- **struct**: functional construction `Vec2(1.0, 2.0)` — create a stack struct value
  like a function call.
- **Deep copy**: explicit `copy()` convention method (`A b = a;` is a reference alias,
  not a copy; see `docs/constructor.md`).

**Constructor ≠ `@startup`**: the constructor does **initialization** (synchronously on
`new`); `@startup` does **beginning operations** (runs when the instance's thread/event
loop starts, next section). They are orthogonal and do not replace each other.
Design: `docs/constructor.md`.

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

> **`@startup` is a start signal, not an initializer**: it runs when the instance's
> thread / event loop **begins operating** (e.g. `@thread` startup, starting timers,
> firing the first event). Object **initialization** (setting fields / allocating
> resources / validation) goes through the constructor (`@constructor` annotation or
> a method named after the class) — `new ClassName(args)` calls it automatically.
> They are orthogonal and do not replace each other; design: `docs/constructor.md`.

### Thread Model

```
Main thread: Create instances → Register mappings → Event loop
                                    ↕  Async
Worker thread: @startup → Event loop → Handle events → Fire new events
```

- Per-thread independent event queue (lock-free)
- Cross-thread communication via `mapping()` with automatic async delivery
- No explicit locking required

### Synchronization Primitives (stdlib, §五-2, v3.9.0, additive)

`import sync` provides pthread-based mutex / read-write lock / condition variable /
semaphore / call-once. All use the **handle pattern** (like `Barrier`): `create`
returns an `int` handle; call `destroy` when done (64 slots each, reused after
destroy).

```myp
import sync;

// Mutex (plain + recursive)
int m = Mutex.create();
Mutex.lock(m);
try { ... } finally { Mutex.unlock(m); }   // pair with finally
Mutex.destroy(m);

// RWLock (many readers / one writer)
int rw = RWLock.create();
RWLock.readLock(rw);    // shared read
RWLock.writeLock(rw);   // exclusive write
RWLock.unlock(rw);
RWLock.destroy(rw);

// CondVar (paired with a Mutex; while-loop idiom avoids lost wakeups)
int cv = CondVar.create();
Mutex.lock(m);
while (!ready) { CondVar.wait(cv, m); }    // atomically releases m and blocks
CondVar.signal(cv);                        // or broadcast(cv)
Mutex.unlock(m);
CondVar.destroy(cv);

// Semaphore (P/V)
int s = Semaphore.create(2);   // initial count
Semaphore.wait(s);             // P: decrement, block at 0
Semaphore.post(s);             // V: increment, wake a waiter
Semaphore.destroy(s);

// Once (call-once: only the first caller runs the init)
int once = Once.create();
if (Once.enter(once) == 1) { ...init...; Once.done(once); }
Once.destroy(once);
```

Share mutable state across threads with `@static class` properties (globals):

```myp
@static class Shared { property: int mutex; int count = 0; }

class Worker {
    action:
        @startup void run() {
            Mutex.lock(Shared.mutex);
            Shared.count = Shared.count + 1;   // critical section
            Mutex.unlock(Shared.mutex);
        }
}
```

- **Return values**: `tryLock`/`tryWait`/`tryReadLock`/`tryWriteLock` → `1`=acquired,
  `0`=failed, `-1`=bad handle; `Once.enter` → `1`=first caller (run init),
  `0`=already done.
- **Constraints**: handles are not auto-reclaimed (like `Barrier`) — call `destroy`;
  `CondVar.wait` releases and re-acquires the associated Mutex — use a `while` loop.

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

### `import option` — Nullable Container (v3.x)

Explicit nullable wrapper `Option<T>`: `Option()`=none, `Option(T v)`=some.

```myp
import option;
Option<int> none = new Option<int>();
Option<int> some = new Option<int>(42);
int? maybe = new Option<int>(7);       // T? ≡ Option<T> (type position)
if (some.isSome()) {
    int v = some.get();                // 42
}
int safe = maybe.getOr(0);             // default when none
some.set(9);
some.clear();                          // back to none
```

> Combines with tuples/first-class functions: `Option<R> mapOpt<T, R>(Option<T> o, (T) -> R f)`.

### `import collections` — Collection Types

```myp
import collections;

// ArrayList<T> — dynamic array (auto-growing, no 1024 limit)
ArrayList<int> list = new ArrayList<int>();
list.add(10);
list.add(20);
int first = list.get(0);   // 10
list.set(1, 30);
int n = list.size();        // 2

// HashMap<K,V> — hash table (linear probing, auto-growing)
HashMap<int, string> map = new HashMap<int, string>();
map.put(1, "one");
map.put(2, "two");
string v = map.get(1, "?");  // "one"
bool has = map.contains(2);  // true
map.remove(1);

// Set<T> — hash set (auto-growing)
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

Random.init(12345);               // Set seed
int r = Random.next();            // [0, RAND_MAX]
int d = Random.below(10);         // [0, 10)
double u = Random.uniform();      // [0.0, 1.0)
double g = Random.gaussian();     // standard normal N(0,1) (Box-Muller)
double rg = Random.range(5.0, 10.0);   // uniform in [lo, hi)
double e = Random.exponential(2.0);    // exponential, mean 1/lambda (inverse transform)
int p = Random.poisson(3.0);           // Poisson integer (Knuth's algorithm)
Random.shuffle(arr, n);           // Fisher-Yates shuffle
```

### `import fmt` — printf-style Formatting (§六-4, v3.9.0, additive)

Fills the `sprintf` gap: previously only string interpolation `${x}` was available;
now width / precision / base / padding control is provided.

```myp
import fmt;

Fmt.i(42)               // "42"          decimal (signed)
Fmt.i(42, 6)            // "    42"      right-aligned, space padded
Fmt.i(42, 6, 48)        // "000042"      '0' padded
Fmt.u(-1)               // "4294967295"  unsigned decimal (bit pattern)
Fmt.x(255, 4)           // "00ff"        lowercase hex (unsigned, default '0' pad)
Fmt.X(255, 4)           // "00FF"        uppercase hex
Fmt.o(8)                // "10"          octal
Fmt.b(5)                // "101"         binary
Fmt.f(3.14159, 2)       // "3.14"        fixed point %.2f
Fmt.f(123.456, 2, 10)   // "    123.46"  fixed + width
Fmt.e(123.456, 2)       // "1.23e+02"    scientific %.2e
Fmt.g(0.0001, 4)        // "0.0001"      shortest %.4g
Fmt.s("hi", 5)          // "   hi"       string right-aligned
Fmt.sR("hi", 5)         // "hi   "       string left-aligned
```

Default-parameter signatures: integer family `(v, width = 0, pad = space/48)`,
float family `(v, precision = 6, width = 0, pad = space)`; `width <= 0` means no padding.

### `import crypto` — Checksum / Hashing (§六-4, v3.9.0, additive)

Fills the `crypto/hash` gap. Hash cores live in the C runtime; the MYP side is a static-class wrapper.

```myp
import crypto;

string h = Crc32.crc32Hex("hello");   // 8-digit lowercase hex (unsigned display)
int c = Crc32.crc32("hello");         // raw 32-bit value (may be negative as int)

Hash.md5("abc");     // 32-char lowercase hex
Hash.sha1("abc");    // 40-char lowercase hex
Hash.sha256("abc");  // 64-char lowercase hex
```

Algorithms: CRC-32 (IEEE 802.3), MD5 (RFC 1321), SHA-1 / SHA-256 (FIPS 180).
Regression uses standard known-answer vectors (including a 56-byte two-block message).

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
Coro.destroy(h);                               // FORCE-cancel (no cleanup; safe on running coroutine)
long r  = Coro.result(h);                      // read coroutine return value
long v  = Coro.waitEvent(eventId, val);        // block on an event (= await ClassName.eventName)
long v  = Coro.waitEventTimeout(id, ms, val);  // event wait with timeout: -1 on timeout, else val
long v  = Coro.waitAny(ids, count, ms, val);   // wait for any listed event: returns fired event id, -1 on timeout
long c  = Coro.current();                      // handle of the running coroutine (-1 if none)
long n  = Coro.count();                        // number of active coroutines on this thread
long s  = Coro.status(h);                      // state: -1 invalid / 0 finished / 1 ready/running / 2 waiting event
Coro.requestCancel(h);                         // cooperative cancel: request the coroutine exit after its next await/yield
long q  = Coro.cancelRequested();              // is the current coroutine cancel-requested? (inside coroutine, 1/0)
Coro.clearCancel();                            // clear the current coroutine's cancel request
```

> **Language-level timeout syntax**: `await Signal.go timeout 30;` is equivalent to
> `Coro.waitEventTimeout(go, 30, 0)` — returns `-1` on timeout, otherwise the value passed in by `resume`.
>
> **Cancellation semantics**: `Coro.destroy(h)` is a **force-cancel** (immediate; `finally`/resource
> cleanup is not run). If a coroutine needs cleanup, use cooperative cancellation — call
> `Coro.requestCancel(h)` externally; the coroutine checks `Coro.cancelRequested()` after each
> `await`/`yield` and exits itself (running cleanup) when it sees 1.

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

> **Blocking caveat**: MYP coroutines are cooperative — calling a blocking operation
> (`Time.sleep`, blocking I/O, `Thread.join`, ...) inside a coroutine **blocks the whole
> thread** (including the thread's other ready coroutines). To wait for time use
> `await Timeline.timeout` (with `Timeline.startTimeout(ms)`); to wait for an external
> condition use `await ClassName.eventName`. Genuinely blocking work (disk/network/
> cross-thread sync) belongs on a `@thread` thread. See `coro.md` §10.

> Semantics: an `@coro` method or top-level `@coro` function call compiles to a spawn
> (`create` + arg slots + `set_entry` + first `resume`) and returns a `long` handle; `await`
> compiles to `__myp_coro_yield(val)` (`await expr` is an expression binding the full operand;
> after resume its value equals the value passed in by `resume`); an `@coro` body's `return val`
> is stored into its per-coroutine result slot, read via `Coro.result(h)`. Finished coroutines
> recycle their slot automatically and stacks are freed at process exit. A top-level `@coro`
> function's entry wrapper has no `this` slot (params start at slot 1); codegen pre-scans to
> create wrappers, so a coroutine function may be defined after its call site.

---

### `import pool` — Parallel Computing Utilities

```myp
import pool;

// Parallel static class — thread pool query / configuration API
// The language-level parallel primitive is @parallel for (which uses the global
// work-stealing thread pool automatically).

int cpus = Parallel.threadCount();      // hardware concurrency (sysconf)
Parallel.setThreads(4);                 // set pool size before the first @parallel for (0 = auto)
@parallel for (int i = 0; i < 100; i = i + 1) {
    int wid = Parallel.workerId();      // current worker index (0..N-1)
    // ...
}
int nw = Parallel.workerCount();        // actual pool worker threads (0 = not initialized)
int on = Parallel.isActive();           // whether the pool is initialized (1=yes 0=no)
```

| Method | Description |
|--------|-------------|
| `threadCount()` | Hardware concurrency — the pool's default size |
| `workerCount()` | Actual worker threads of the global pool (available after the first `@parallel for`; 0 = not initialized) |
| `workerId()` | Pool worker index of the current thread (0..N-1 inside a `@parallel for` body; -1 on non-pool threads) |
| `isActive()` | Whether the thread pool is initialized (1=yes 0=no) |
| `setThreads(n)` | Set the pool size (0 = auto = hardware concurrency; only effective before first creation, no-op afterwards) |

Backed by a work-stealing thread pool; use with `Atomic` for shared data.

---

### `import memory` — Dynamic Memory Management

```myp
import memory;

// Bridge to the C standard library malloc/free/realloc (pointers carried as `long`,
// same convention as the json/regex handles)
long p = Memory.alloc(1024);            // allocate (returns a pointer)
Memory.free(p);                         // free
p = Memory.realloc(p, 2048);            // reallocate
Memory.release(p);                      // alias for free
```

> **Use cases**: ① **Deterministic release** — arena-allocated `new T[n]` is only reclaimed
> at process exit / `@region` end; for temporaries with a clear lifetime, `Memory` releases
> immediately (controls peak memory). ② **FFI pointer interop** — raw pointers passed to C
> libraries (SDL/net/GPU/third-party). ③ **Byte buffers / manual layout** — raw buffers for
> binary protocols and file formats. For dynamic arrays use `ArrayList<T>` from `collections`
> (auto-growing); this module only manages raw memory.

## 12. Compilation & Tools

### Compiler

```bash
# Compile and run
./build/mypc myapp.myp && ./myapp.out

# Compile + run in one step (go-run style; single-class files without main
# are auto-`main`'d when the class has @startup)
./build/mypc run myapp.myp
./build/mypc run myapp.myp arg1 arg2     # forward program args

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

#### `mypc run` (v3.9.0, go-run style)

`mypc run file.myp [args...]` compiles → links to a temp binary → runs → cleans up:
- **Args**: `args...` are forwarded to the program (`main(argc, argv)` / constructor `(int argc, string[] argv)`).
- **Auto-main for single-class files**: when the file has **no `main`** and exactly one
  class carries an `@startup` annotation, the compiler generates
  `int main() { ClassName c = new ClassName(); c.startupAction(); return 0; }` and
  triggers the `@startup` entry.
- **Constraint**: no `@startup` class / multiple `@startup` classes → compile error
  (suggests defining `main()`).
- **Normal compilation unchanged**: non-run mode still requires an explicit `main`
  (link-time error).
- Temp artifacts are removed automatically; exit code = the program's exit code.

```myp
// hello.myp — no main; auto-generated by run
import env;
class Hello {
    action:
        @startup void go() {
            Console.writeString("Hello from @startup!\n");
        }
}
```
```bash
./build/mypc run hello.myp     # prints: Hello from @startup!
```

#### Full Command-Line Options

| Option | Description |
|---|---|
| `-o <file>` | Output file name |
| `-O0` / `-O1` / `-O2` / `-O3` | Optimization level (default `-O0`; `-O2` runs the IR optimization pipeline) |
| `-g`, `--debug` | Generate DWARF debug info (breakpoints/lines/variables) |
| `--passes <p>` | Run a custom MYP pass (e.g. `myp-pass`) |
| `--emit-llvm` | Write LLVM IR to a `.ll` file (skips linking) |
| `--test` | Generate and run a test runner (`@test`) |
| `--shared` / `--static` | Build a shared / static library |
| `--trace` | Enable runtime event tracing |
| `--package-path <dir>` | Local package directory |
| `--macro-expand` | Dump the AST after macro expansion |
| `--stdlib <path>` | stdlib directory |
| `--version` / `--help` | Version / help |

Compiling multiple files (`mypc a.myp b.myp`) merges them into one module and naturally
enjoys cross-file optimization (no LTO needed).

#### Optimization (`-O` and custom passes)

```bash
./build/mypc -O2 myapp.myp        # IR optimization pipeline (mem2reg/GVN/inline/loops...)
./build/mypc -O0 myapp.myp        # default: fast compile, debug-friendly
./build/mypc --passes myp-pass -O0 myapp.myp   # append a custom MYP pass
```

- `-O1/-O2/-O3` run the LLVM standard optimization pipeline (default `-O0` disables optimization).
- `--passes myp-pass` runs the MYP-specific pass (removes compiler-generated dead stores).
- Design & implementation: `docs/optimization_debugging.md`.

#### Debug (`-g` DWARF + gdb)

```bash
./build/mypc -g myapp.myp          # or --debug; recommend -g -O0
gdb ./myapp.out
(gdb) break myapp.myp:10           # breakpoint by source line
(gdb) run
(gdb) print variable                # inspect arguments/locals
(gdb) next / step / continue
```

- `-g` generates DWARF: function breakpoints, source line numbers, arguments/locals, type info.
- Class methods appear as `Class_method` symbols; debugging inside coroutines is a known limitation.
- Design: `docs/optimization_debugging.md` (Part B).

#### Debug (VS Code DAP)

MYP ships `myp_debug` (a DAP ↔ gdb bridge) for breakpoints/stepping/variables in VS Code:

```bash
# Compile an executable with debug info
./build/mypc -g myapp.myp

# Run the DAP server directly (for VS Code / any DAP client)
./build/myp_debug
```

**VS Code**: after installing the `vscode-myp` extension, use a `.vscode/launch.json`:

```json
{
  "version": "0.2.0",
  "configurations": [
    {
      "name": "MYP Launch",
      "type": "myp",
      "request": "launch",
      "program": "${workspaceFolder}/myapp.out"
    }
  ]
}
```

- `program` points to the `-g`-compiled executable.
- Set `myp.debuggerPath` to specify the `myp_debug` path (auto-detected by default).
- Supports: breakpoints (source lines), stepping (next/stepIn/stepOut), call stack, locals,
  hover evaluation.

#### Metaprogramming (`@eval` / `macro` / `@macro`)

MYP offers three layers of metaprogramming (design: `docs/metaprogramming.md`):

**1. `@eval` compile-time evaluation (pure functions)**

```myp
@eval int fib(int n) {
    return n < 2 ? n : fib(n - 1) + fib(n - 2);
}
const int FIB10 = fib(10);   // computed at compile time: 55 (ret i32 55)
```

**2. `macro` declarative macros (AST templates)**

```myp
macro repeat($n, $body) {
    for (int _i = 0; _i < $n; _i++) { $body }
}
repeat(3, total = total + 10);   // expands to a for loop ×3
```

**3. `@macro` procedural macros (`quote` code templates)**

```myp
@macro StmtList makeCalls(int n) {
    StmtList out = quote {};
    for (int i = 0; i < n; i++) {
        out = out + quote { Console.write($i); };
    }
    return out;
}
makeCalls(3);                    // generates 3 Console.write(...) statements
```

- Debugging: `--macro-expand` dumps the expanded AST.

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
| `myp.debuggerPath` | Path to `myp_debug` (auto-detected by default) |

### Project Structure

```
MYPLanguage/
├── myp                  # package-manager CLI (Python: init/build/install/run)
├── CMakeLists.txt
├── include/mylang/      # compiler headers: AST/CodeGen/Sema/Parser/Lexer/Eval/
│   └── ...              #   Macro/MypPasses/Fmt/LSP/Token/Type/...
├── src/                 # compiler source
│   ├── main.cpp         # mypc driver (lexer→parser→sema→codegen→link)
│   ├── ast/ lexer/ parser/ sema/ codegen/ runtime/
│   ├── eval/            # @eval compile-time evaluator
│   ├── macro/           # macro expansion
│   ├── fmt/             # formatter
│   ├── lsp/             # language server (myp_lsp)
│   └── dap/             # debug adapter (myp_debug, DAP↔gdb bridge)
├── stdlib/              # standard library (pure MYP classes)
│   ├── env/io/fs/text/stream/math/random/time/timeline
│   ├── collections/setops/atomic/pool/barrier/future/memory
│   ├── coro/channel/net/json/regex/base64/date/process/args
│   ├── logger/sdl/ui/error/cuda
│   └── test
├── tests/               # run_tests.sh / run_tests_O2.sh / run_tests_asan.sh /
│   └── ...              #   run_tests_tsan.sh / test_debug.sh / test_dap.py /
│                        #   expected/ / negative/ / <feature>/
├── examples/            # complete examples (hello/fib/ad/BNCT/sdl/tui)
├── BNCTDoseEngine/      # BNCT Monte-Carlo engine (pure MYP + HDF5 cross-sections)
├── deeplearning/        # MLP + MNIST training/inference
├── vscode-myp/          # VS Code extension (syntax highlight + LSP + DAP)
├── docs/                # design/grammar/manual/manual_en/coro/exceptions/
│   └── ...              #   operators/metaprogramming/constructor/...
├── build/               # build outputs: mypc, myp_debug, myp_lsp, myp_viz, myp_fmt
└── build-asan/          # ASAN/UBSAN build
```

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
| `@constructor` annotation / name == class | Object initialization (auto-called on `new`) |
| `struct` | Pure data container (pass by value) |
| Top-level `function` | Pure computation functions |
| `action:` + `@startup` | Start signal / begin operations (runs when thread/event loop starts) |
| `@thread` | Component needing a dedicated thread |
