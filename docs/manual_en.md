# MYP Programming Manual

> Version 3.12 | Event-Driven Component Language
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
12. [Metaprogramming](#12-metaprogramming)
13. [Compilation & Tools](#13-compilation--tools)
14. [Complete Example](#14-complete-example)

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

In MYP's event-driven model, output logic lives in a component's `action:` (and `main`
only does "wiring" — see below). Three equivalent forms:

**① `@startup` + `mypc run` (simplest, no `main` needed)**

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
# Output: Hello, MYP!
```

**② Constructor output (`@constructor`, runs synchronously on `new`)**

```myp
// hello2.myp
import env;

class Hello {
    action:
        @constructor Hello() {
            Console.writeLine("Hello, MYP!");
        }
}

int main() {
    Hello h = new Hello();   // constructor body runs on `new` → prints
    return 0;
}
```

```bash
./build/mypc hello2.myp && ./hello2.out
# Output: Hello, MYP!
```

**③ Multithreaded output (`@thread` instance, `@startup` runs when the thread starts)**

```myp
// hello3.myp
import env;

class Hello {
    action:
        @startup void go() {
            Console.writeLine("Hello from worker thread!");
        }
}

int main() {
    Hello h = new Hello() @thread;   // dedicated thread; @startup fires on start
    return 0;
}
```

```bash
./build/mypc hello3.myp && ./hello3.out
# Output: Hello from worker thread!
```

> **`main` wiring rule**: `main()` only does "wiring" — create instances + declare
> `mapping`; **direct method calls are forbidden** (writing `Console.writeLine(...)` in
> `main` is a compile error: `direct function call not allowed in main() — use
> mapping() instead`). Logic goes in component actions: `@startup` runs when the instance
> starts operating (`mypc run` auto-generates `main` and triggers it for single-class files,
> see §13; with an explicit `main`, use a `@thread` instance), `@constructor` runs
> synchronously on `new`.

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
0b1010      // Binary (= 10)
0o17        // Octal (= 15)
0755        // Leading-zero C-style octal (= 493)
1_000_000   // Underscore separator (= 1000000)
0xFF_FF     // Hexadecimal + underscore (= 65535)
1_000.5     // Float + underscore
1e1_0       // Exponent + underscore (= 1e10)
true false  // Boolean
'A' '\n'    // Character (supports \n \t \\ \' \" \0)
"hello"     // String (supports \n \t \\ \" \0)
null        // Null value
```

> Literal suffixes: `42L` (long), `0xFFu` (unsigned, width by value), `1.5f` (float32),
> `1_000_000L` (underscores can combine with suffixes). Underscores are readability
> separators only (between digits), stripped at compile time.

> **`null` semantics**: `null` can be assigned to reference types (class/interface/struct
> pointer) and tested with `x == null`. **`string` is value-semantic** (char buffer) and
> **cannot** be `null` (compile error). Dereferencing a `null` class reference is a runtime
> error (no guarantee of protection) — check for null before calling.

### String Interpolation (v2+)

```myp
var name = "world";
var msg = "Hello, $name!";   // → "Hello, world!"
var x = 42;
var s = "x = $x";            // → "x = 42"
```

### Operators

| Precedence | Category | Operators | Assoc |
|------------|----------|-----------|-------|
| 15 | Assignment | `=` `+=` `-=` `*=` `/=` `%=` | right |
| 14 | Pipe | `\|>` | left |
| 13 | Ternary | `? :` | right |
| 12 | Logical OR | `\|\|` | left |
| 11 | Logical AND | `&&` | left |
| 10 | Bitwise OR | `\|` | left |
| 9 | Bitwise XOR | `^` | left |
| 8 | Bitwise AND | `&` | left |
| 7 | Equality | `==` `!=` | left |
| 6 | Relational | `<` `>` `<=` `>=` | left |
| 5 | Shift | `<<` `>>` | left |
| 4 | Addition | `+` `-` | left |
| 3 | Range | `..` | left |
| 2 | Multiplication | `*` `/` `%` | left |
| 1 | Unary | `!` `~` `-` `++` `--` | right |
| 0 | Postfix | `.` `[]` `()` `++` `--` | left |

> Precedence matches C family (**higher = looser**): `..` (range) sits between addition and
> multiplication; `~` is bitwise NOT (integer/bitvector/bit); unary `++`/`--` also appear in
> the postfix column (`a++`/`a--`).

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
| `byte` | Signed 8-bit (`int8` alias) | 8 |
| `short` | Signed 16-bit (`int16` alias) | 16 |
| `int` | Signed 32-bit (`int32` alias) | 32 |
| `long` | Signed 64-bit (`int64` alias) | 64 |
| `ubyte` | Unsigned 8-bit (`uint8` alias) | 8 |
| `ushort` | Unsigned 16-bit (`uint16` alias) | 16 |
| `uint` | Unsigned 32-bit (`uint32` alias) | 32 |
| `ulong` | Unsigned 64-bit (`uint64` alias) | 64 |
| `char` | Character (8-bit) | 8 |
| `float` | Single-precision float | 32 |
| `double` | Double-precision float | 64 |
| `bool` | Boolean | 1 |
| `bit` | Single bit (v3.12, LLVM i1) | 1 |
| `bitvector<N>` | Fixed-width bit vector (N = 8/16/32/64, v3.12, LLVM iN) | N |
| `string` | String pointer | pointer |
| `void` | No type | — |

> `bit`/`bitvector<N>`/`bitfield` semantics: see the "Bit types" subsection below (§3);
> `float4`/`double2`/`int4` vector types (with `load4`/`store4` packed access) see
> `design.md` §3.6.

### Numeric Promotion

**Implicit conversion is lossless-only** (widening / precision-preserving); anything that
may lose data / wrap / truncate requires an explicit cast.

```
Signed integer widening (same sign): i8 → i16 → i32 → i64        （SExt）
Unsigned integer widening (same sign): u8 → u16 → u32 → u64      （ZExt）
Integer → float: i8/i16/i32, u8/u16/u32 → f64 (exact within 32 bits)
Float widening:                f32 → f64                          （FPExt）
Character:                    char → i32/u32/i64                   （ZExt, char = u8）
```

```myp
int a = 42;
long b = a;       // ✅ int → long auto-promote (SExt)
double c = a;     // ✅ int → double auto-promote (exact within 32 bits)
int d = b;        // ❌ long → int does NOT auto-demote
double e = 3.0f;  // ✅ float → double auto-promote
int f = 0xFF;     // ✅ int → int
```

**Explicit cast required (lossy)**:
- Cross-sign: `int→uint`, `uint→int`, `char↔byte` (char is a u8-semantics alias, not
  interchangeable with signed byte);
- Integer→float beyond precision: **`i64/u64 → f64`**, `int/long → float` must be explicit
  (silent precision loss costs more than writing `double(x)`);
- All narrowing: `long→int`, `double→float`, `int→byte`;
- `ulong` involved: small-unsigned→large-unsigned implicit (ZExt), cross-sign/float explicit
  (symmetric with the `uint` family).

```myp
long big = 9007199254740993L;
double dd = double(big);   // explicit (|x|≥2^53 loses precision, never silent)
int g = int(big);          // explicit narrowing
```

> **Overflow semantics**: signed/unsigned overflow = **wrap** (deterministic, no UB); when
> detection is needed use `checkedAdd(a,b)`/`checkedMul(a,b)` (return `(value, overflow:bool)`
> tuple, see below).

### Unsigned Types (uint family)

Unsigned types `ubyte`/`ushort`/`uint`/`ulong` (fixed-width aliases `uint8`/`uint16`/
`uint32`/`uint64`) have **unsigned semantics**, matching C:

- **`u` literal suffix**: `0xFFFFFFFFu` is an unsigned integer literal (width by value:
  ≤0xFF→`ubyte`, ≤0xFFFF→`ushort`, ≤0xFFFFFFFF→`uint`, larger→`ulong`).
- **Logical right shift**: `uint`'s `>>` is logical (`lshr`), not arithmetic.
- **Unsigned division/modulo**: `/`→`udiv`, `%`→`urem`.
- **Unsigned comparison**: `<`/`>`/`<=`/`>=` use unsigned predicates.
- **Wrap**: add/sub wrap at 32 bits automatically, no `& 0xFFFFFFFF` needed.
- **uint→long widening** uses ZExt (`0xFFFFFFFFu` → `4294967295L`, not -1).
- **Native rotate**: `(x >> n) | (x << (32 - n))` is recognized by LLVM as a single
  `rorl`/`rol`.

```myp
uint a = 0xFFFFFFFFu;
uint b = a >> 4;          // logical shift → 0x0FFFFFFF
uint c = a / 3u;          // unsigned division → 1431655765
bool big = a > 100u;      // unsigned comparison → true
long v = a * 4294967296L; // uint→long ZExt → 4294967295 << 32
```

> Note: to print an unsigned value as `long`, widen explicitly via a binary op (e.g.
> `x * 1L`); `ulong` does not implicitly convert to `long` (may overflow signed range).

### Explicit Conversion `uint8(x)`

Built-in type names used as function calls perform explicit conversion:
`uint8(x)`/`byte(x)`/`int(x)`/`long(x)`/`double(x)`/`bool(x)` etc. Rules:

- **Wide→narrow truncation**: `byte(200L)` → -56 (0xC8 truncated to i8); `long(3.99)` → 3.
- **Narrow→wide by source sign**: unsigned source ZExt (`uint8`→`long` is 0..255), signed SExt.
- **double↔int**: `int(3.99)` → 3 (truncate); `double(42)` → 42.0.
- **int↔uint bit-preserving**: `uint(-1)` → 4294967295.
- **bool in conversion chains**: `int(b)` → b?1:0; `bool(n)`/`bool(f)` → value≠0;
  `double(b)` → 0.0/1.0.
- **char = u8 semantics**: `char(0xFF)` is a u8 value; `char`→`int` ZExt (0xFF → 255, not -1).
- **bit(x)** = x≠0; `bitvector<N>(uint)` bit-preserving; `uintN(bv)` passthrough (see below).

Typical use: filling a byte array from `long` computed values (previously impossible
without a cast):

```myp
uint8[] in = new uint8[n];
long rng = seed;
while (i < n) {
    rng = (rng * 1103515245L + 12345L) % 2147483648L;
    in[i] = uint8((rng >> 16) & 0xFFL);   // explicit truncation to a byte
}
```

### Bit Types: `bit` / `bitvector<N>` / `bitfield` (v3.12, additive)

- **`bit`**: single bit (LLVM i1). `bit(x)` = x≠0; `bool(bit)` passthrough; usable as a
  boolean context.
- **`bitvector<N>`**: fixed-width bit vector (N = 8/16/32/64, underlying LLVM `iN`).
  `bitvector<8>` ≡ u8 bit view. Supports indexing `v[i] : bit`, bit ops `& | ^ ~ << >>`,
  index write `v[i] = x`, `bitvector<N>(uintN)` bit-preserving interop, `bytesOf(bv)` → `ubyte[]`.
- **`bitfield`**: struct bit-field packing (backing integer ≤8→i8/≤16→i16/≤32→i32/else i64);
  field access is bit extraction / read-modify-write. For file headers/protocols/GPU control words.

```myp
bitvector<8> bv = bitvector<8>(0x5A);
bit bit0 = bv[0];               // bit 0
bv[1] = bit(1);                 // index write
uint u = uint(bv);              // passthrough 0x5A
ubyte[] bytes = bytesOf(bv);    // serialize (built-in, no import)

bitfield Flags { bit read; bit write; bit[6] reserved; }
Flags f;                        // zero-init, packed into 1 byte
f.write = bit(1);               // or f.write = true (bool→bit implicit)
Console.writeLong(int(f.write) << 1);
```

### `bitcast<T,U>(x)` (v3.12)

`bitcast` preserves bits, does not interpret values (numeric `T(x)` changes the value).
Requires equal width (8/16/32/64); cross-width is an explicit error.

```myp
uint bits = bitcast<uint>(1.0f);   // 0x3F800000 (bit pattern of float)
float back = bitcast<float>(bits); // 1.0
```

### Bit Operations (v3.12)

Integer-family bit ops map directly to LLVM intrinsics (return type = argument integer
type): `popcount(x)`, `clz(x)`/`ctz(x)` (0 → bit width), `bitreverse(x)`, `rotl(x,n)`/`rotr(x,n)`.

```myp
Console.writeLong(popcount(0b1011));     // 3
Console.writeLong(rotr(0b1000, 1));      // 4
```

### Overflow Detection: `checkedAdd` / `checkedMul` (v3.12)

Return `(value, overflow:bool)` tuples (signed integers, common-type promotion).

```myp
(int v, bool ov) = checkedAdd(2147483647, 1);  // v=-2147483648, ov=true
(int ok2, bool no) = checkedAdd(1, 2);         // ok2=3, no=false
(int, bool) t = checkedMul(46341, 46341);      // t.0 overflow value, t.1=true
```

### Parsing: `parse*` and `parseIntOpt` (v3.12)

Unified `strtol/strtoull/strtod` semantics (**signed with base**, `0x` prefix, leading-zero
octal); return 0 on failure.

```myp
int a = parseInt("42");        // 42
long b = parseLong("0xFF");    // 255 (hex)
double c = parseDouble("3.14");// 3.14
float f = parseFloat("1.5");
uint u = parseUint("4294967295");

// parseIntOpt distinguishes legal "0" from failure (parseInt returns 0 on failure):
(int v, bool ok) = parseIntOpt("42");   // v=42, ok=true
(int v2, bool ok2) = parseIntOpt("abc");// v2=0, ok2=false
(int v3, bool ok3) = parseIntOpt("0");  // v3=0, ok3=true (legal 0)
```

### Numeric Traits & `Math` Polymorphism (v3.12)

Built-in numeric traits: `Numeric` (`+ - * / %`), `Integer`, `Float`, `Ordered`
(`< <= > >=`, incl. string). Generic functions/static methods can constrain
`where T : Trait`, verified at instantiation, zero runtime cost.

```myp
T twice<T where T : Numeric>(T v) { return v + v; }
Console.writeLong(twice(21));          // 42
Console.writeString("" + twice(1.5)); // 3

// Math library polymorphism: float arg returns float (inside GPU kernels → __nv_xf)
float s = Math.sqrt(4.0f);     // 2.0f (no float(...) wrap needed)
double d = Math.sqrt(2.0);     // double
double p = Math.pow(2.0, 10.0);// double
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

// `_` ignore: skip elements you don't care about (no binding, no type check)
(int _, string name) = getPair();       // discard the first element
(int a, string _) = getPair();          // discard the second element
(int _, string _) = getPair();          // keep only the multi-value side effect
```

> Disambiguated from function types `(A, B) -> R` and lambdas `(a, b) => ...`;
> see `docs/tuple.md`. `_` is a valid identifier and can be used normally elsewhere.

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

### Enum & Match (v2.1, additive)

`enum` declares an enumeration type (sealed — the variant set is fixed); `match` matches
against enum variants (enum + pattern matching).

```myp
// Simple enum
enum Color { Red; Green; Blue; }

// Enum with data (variants can carry data)
enum Shape {
    Circle(double radius);
    Rect(double width, double height);
}

Color c = Color.Red;
string label = "";
match (c) {
    Color.Red   => { label = "Red"; }
    Color.Green => { label = "Green"; }
    Color.Blue  => { label = "Blue"; }
}

Shape s = Shape.Circle(5.0);
double r = 0.0;
match (s) {
    Shape.Circle(radius) => { r = radius; }   // bind single data
    Shape.Rect(w, h)     => { r = -1.0; }     // bind multiple data
}
```

- `enum` variants are `;`-separated; a variant can carry 0..n data items (e.g. `Circle(double radius)`).
- `match` is a **statement**: `match (expr) { EnumName.Variant => { ... } ... }` matches each
  branch; data-bearing variants bind data with `Variant(v1, v2, …)`.
- Enums are sealed (no else fallback), so `match` must cover the variants used.

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

    // ✅ Allowed: declare mappings (nodes use CLASS names)
    mapping() {
        Sensor.valueRead -> Display.show;
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
Vec2 v = Vec2(3.0, 4.0);             // struct functional construction (positional; named NOT supported)
g.greet(name = "Al", suffix = "?");  // method named arguments
Greeter.scale(5);                    // static method, default f=2
```

- **Struct functional construction** (`Vec2(3.0, 4.0)`) requires a declared `@constructor`
  (see §9), and **does NOT support named arguments** (named-arg reinterpretation applies
  only to functions/methods/constructors).

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
- **`nonlocal` by-reference capture** (shared mutable): declaring `nonlocal 变量名;`
  inside a lambda routes reads/writes of that variable to the same storage as the outer
  function (heap cell, ARC-managed) — the closure and the outer function see each other's
  changes. The standard way to write stateful closures (v1: scalar types only; nested
  lambdas / struct methods not yet supported).

```myp
// Nonlocal capture: lambda mutates an outer counter
int counter() {
    int k = 0;
    (int) -> int inc = (int d) => {
        nonlocal k;
        k = k + 1;
        return k;
    };
    return inc;              // returns the closure; k lives in a shared cell
}
(int) -> int c = counter();
c(0);                        // 1
c(0);                        // 2
```

- The classic **Man or Boy** test (Knuth; recursive closure + first-class thunk +
  `nonlocal`) validated against the Go reference: `A(10, 1, -1, -1, 1, 0) = -67`
  (see `tests/@test/man_or_boy.myp`), verifying by-reference capture + recursive
  thunk passing.

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

### Four-Section Structure

MYP classes are event-driven components with four main sections `action:` / `event:` /
`property:` / `function:` (plus `static:` and `struct:` sections — see "Section Rules"):

```myp
class Sensor {
    action:          // Callable methods (receive messages)
        void init(int id);
        float readValue() { return lastValue; }

    event:           // Firable events (send messages)
        valueRead(float temp);
        thresholdExceeded(float value);

    function:        // Internal methods (not part of mapping, class-internal only)
        void calibrate() { }

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
| `struct:` | Nested structs | Field/method definitions (see §7) |

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

### Struct Method Advanced Features

Struct methods support the following:

#### `this` keyword

```myp
struct MyStruct {
    double x;
    void setX(double v) {
        this.x = v;  // `this` refers to the current instance
    }
}
```

#### Returning a struct

```myp
struct Inner { double val; }

struct Outer {
    Inner inner;
    Inner getInner() {
        return inner;  // return a struct by value
    }
}
```

#### Sibling-method calls

```myp
struct Helper {
    double calc(double x) { return x * 2.0; }
    double process(double v) {
        return calc(v) + 1.0;  // call calc directly
    }
}
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

Mapping connects events to actions. **Mapping nodes always use CLASS names**
(`Class.event` / `Class.action`), even for instance-level mappings declared inside a
function — nodes must be class names, never instance variable names:

```myp
// Type-level mapping (file-level, global)
mapping() {
    Sensor.valueRead -> Display.showTemperature;
}

// Instance-level mapping (local, inside a function): nodes still use class names
int main() {
    Sensor sensor;
    Display display;

    mapping() {
        Sensor.valueRead -> Display.showTemperature;
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
    // One event triggers multiple actions (nodes use class names)
    Sensor.valueRead -> Display.show, Logger.log;

    // Equivalent to:
    Sensor.valueRead -> Display.show;
    Sensor.valueRead -> Logger.log;
}
```

### Mapping Semantics

- One event can map to multiple actions
- Multiple events can map to the same action
- Mapping establishes an event bus at runtime
- Same thread = synchronous processing, cross-thread = async delivery

### Scope Management `@scope` (v2.3)

By default a mapping stays active forever. `@scope` binds the handler lifetime to a function scope:

```myp
void run() {
    Sensor s;
    mapping() @scope {
        s.ready -> log.write;
    }
}  // handler auto-unregisters when the function exits
```

### Conditional Filter `where` (v2.3)

Only events satisfying the condition are forwarded:

```myp
mapping() {
    rs.valueEmitted where value >= 3 -> Console.write;
}
```

The `where` expression can use event parameter names and supports comparisons and arithmetic.

### Lambda Transform Nodes (v2.3)

Inline data transforms via a lambda in the mapping chain:

```myp
mapping() {
    rs.valueEmitted -> (int v) => { return v * 2; } -> display.show;
}
```

### Timer Transforms (v2.3)

- `delay(ms)` — delay forwarding
- `throttle(ms)` — rate-limit

```myp
mapping() {
    sensor.data -> delay(100) -> display.update;
    sensor.data -> throttle(50) -> logger.write;
}
```

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
        Sensor.valueRead -> Worker.process;   // nodes use class names
    }
    return 0;
}
```

### @threadpool

```myp
// Create 4 Workers, each running on its own thread
Worker[4] pool @threadpool;

mapping() {
    Sensor.valueRead -> Worker.process;   // class names (not pool[0].process)
}
```

### @parallel for — Data Parallelism

`@parallel for` is MYP's data-parallelism primitive for auto-parallelizing compute-heavy loops.
The compiler extracts the loop body onto a work-stealing thread pool automatically — no
events/message passing needed:

```myp
import atomic;

int[1000] tally;
@parallel for (int i = 0; i < 1000; i = i + 1) {
    Atomic.addInt(tally, i, i);
}
```

#### How it works

```
Compiler:
  1. scan the outer scope, collect variables referenced by the loop body
  2. build a capture struct filled with current values of all variables
  3. extract the loop body into a standalone function parallel_body(i, arg)
  4. call myp_pool_parallel_for() to distribute the iterations

Runtime:
  5. the work-stealing pool splits the iteration range into chunks
  6. each thread executes its chunk serially
  7. a barrier waits for all to finish → return
```

#### Variable capture

Outer variables are auto-captured into a struct passed via `void* arg`:

| Type | How |
|------|-----|
| `int`/`long`/`double` | by value (per-thread copy) |
| `double[]`/`int[]` | by pointer (shared heap array) |
| class instance | by pointer |
| static-method call | direct LLVM call |

#### Thread safety

Protect shared writes with `Atomic`:
```myp
@parallel for (int i = 0; i < size; i = i + 1) {
    // ✅ correct
    Atomic.addDouble(tally, idx, value);
    // ❌ race condition
    // tally[idx] = tally[idx] + value;
}
```

#### Limits

- loop variable can be `int` or `long` (`long` indices auto-convert)
- each iteration must be **independent** (no data dependency)
- no `break` / `continue`
- loop bounds fixed at entry
- **the parallel body only captures outer LOCAL variables**: it cannot directly access
  class/static properties (arrays etc.) — such accesses are parsed as outer locals and
  cause LLVM verify failure or a runtime crash. Copy them to a local first (see the BNCT
  example below: `double[] depthDose = TallyData.depthDose;`).

#### BNCT example

```myp
class Transport {
    action:
        void runBatch(int batchId, int size) {
            double[] depthDose = TallyData.depthDose;
            @parallel for (int i = 0; i < size; i = i + 1) {
                long state = (batchId * size + i) * 152917L + 1L;
                double E = Physics.sampleEnergy(state);
                // ... transport ...
                Atomic.addDouble(depthDose, iz, energy);
            }
        }
}
```

#### Performance reference (16 cores)

| Particles | Time | Speedup |
|-----------|------|---------|
| 5M | ~3s | ~10x |
| 1e9 | ~9.5min | ~10x |

### @gpu for — GPU Offload

`@gpu for` is MYP's GPU-parallelism primitive: compute-heavy loops are offloaded to an NVIDIA CUDA
GPU:

```myp
import math;

long n = 1000000L;
double[] data = new double[n];
for (long i = 0L; i < n; i = i + 1L) data[i] = 1.0;

@gpu for (long i = 0L; i < n; i = i + 1L) {
    data[i] = Math.sqrt(data[i]) + Math.sin(1.0);
}
```

#### How it works

```
Compiler:
  1. emit an NVPTX kernel (myp_kernel), loop index → GPU thread id
  2. collect captured arrays/scalars, generate data-transfer code
  3. link math functions with CUDA libdevice (libdevice.10.bc)
     → the generated PTX is fully self-contained (no runtime JIT linking)
Runtime:
  4. cuModuleLoadData loads the PTX → launch kernel (grid/block auto-computed)
  5. copy array results back → sync → done
```

#### Enable & fallback

- set `MYP_GPU=1` (default CPU)
- requires the NVIDIA CUDA driver (`libcuda.so.1`)
- no GPU / no `MYP_GPU` → **auto-fallback to sequential CPU execution**, identical results
- math needs `libdevice.10.bc` (auto-located; `MYP_CUDA_LIBDEVICE` overrides the path)

#### GPU math functions

Inside a `@gpu for` kernel, the following `Math` functions map to CUDA libdevice (full precision):

| Function | libdevice | Function | libdevice |
|----------|-----------|----------|-----------|
| `Math.sqrt` | `__nv_sqrt` | `Math.exp` | `__nv_exp` |
| `Math.sin` | `__nv_sin` | `Math.log` | `__nv_log` |
| `Math.cos` | `__nv_cos` | `Math.pow` | `__nv_pow` |
| `Math.tan` | `__nv_tan` | `Math.abs` | `__nv_fabs` |
| `Math.floor` | `__nv_floor` | `Math.ceil` | `__nv_ceil` |
| `Math.asin` | `__nv_asin` | `Math.acos` | `__nv_acos` |
| `Math.atan` | `__nv_atan` | `Math.atan2` | `__nv_atan2` |
| `Math.sinh` | `__nv_sinh` | `Math.cosh` | `__nv_cosh` |
| `Math.tanh` | `__nv_tanh` | | |

#### `import cuda` — CUDA standard library

```myp
import cuda;
```

High-level GPU API:

```myp
// Cuda — device info
int ok = Cuda.available();       // 1=GPU available, 0=will use CPU
int n = Cuda.count();            // number of GPUs
string gpu = Cuda.name();        // e.g. "NVIDIA GeForce RTX 2070 SUPER"
long mem = Cuda.memory();        // VRAM (bytes)
int cc = Cuda.capability();      // compute capability (e.g. 705 = 7.5)
int sm = Cuda.multiProcessors(); // streaming multiprocessor count
int mt = Cuda.maxThreads();      // max threads per block
int ws = Cuda.warpSize();        // warp size (usually 32)

// Device — kernel math (GPU full precision, CPU uses the stdlib)
@gpu for (long i = 0L; i < n; i = i + 1L) {
    data[i] = Device.pow(data[i], 2.0) + Device.cos(0.0) + Device.atan2(1.0, 2.0);
}

// Vectors — vectorized ops based on @gpu for (auto GPU, CPU fallback)
Vectors.add(a, b, out, n);        // out[i] = a[i] + b[i]
Vectors.sub(a, b, out, n);        // out[i] = a[i] - b[i]
Vectors.mul(a, b, out, n);        // out[i] = a[i] * b[i]
Vectors.scale(data, 2.0, n);      // data[i] *= 2.0
Vectors.addScalar(data, 1.0, n);  // data[i] += 1.0
Vectors.fill(data, 0.0, n);       // data[i] = 0.0
Vectors.saxpy(3.0, x, y, out, n); // out[i] = 3.0*x[i] + y[i]
Vectors.copy(dst, src, n);        // dst[i] = src[i]
Vectors.negate(data, n);          // data[i] = -data[i]
Vectors.clamp(data, lo, hi, n);   // data[i] = clamp(...)
Vectors.pow(data, 2.0, n);        // data[i] = pow(data[i], 2.0)
Vectors.sqrt(data, n);  Vectors.sin(data, n);  Vectors.cos(data, n);
Vectors.tan(data, n);   Vectors.exp(data, n);  Vectors.log(data, n);
Vectors.abs(data, n);   Vectors.floor(data, n); Vectors.ceil(data, n);

// Vectors — reductions (GPU atomics)
double s = Vectors.sum(a, n);         // Σ a[i]
double m = Vectors.mean(a, n);        // mean
double v = Vectors.variance(a, n);    // variance (single-pass)
double sd = Vectors.stddev(a, n);     // stddev
double ns = Vectors.normSquared(a, n);// Σ a[i]^2
double no = Vectors.norm(a, n);       // sqrt(Σ a[i]^2)
Vectors.normalize(data, n);           // data[i] /= ||data||
double mn = Vectors.min(a, n);        // min (CPU)
double mx = Vectors.max(a, n);        // max (CPU)
double d = Vectors.dot(a, b, n);      // dot (CPU)

// Matrix — elementwise (row-major double[], size rows*cols)
Matrix.add(a, b, c, rows, cols);      // c = a + b (GPU)
Matrix.sub(a, b, c, rows, cols);      // c = a - b (GPU)
Matrix.mul(a, b, c, rows, cols);      // c = a .* b (GPU)
Matrix.scale(a, s, rows, cols);       // a *= s (GPU)
Matrix.fill(a, s, rows, cols);        // a = s (GPU)
Matrix.transpose(a, t, rows, cols);   // t = a^T (CPU)
```

#### Limits

- loop variable `long`, bound `i < n` or `i <= n`
- captured arrays are copied to the device whole before launch (element count = loop upper bound n), copied back after
- each iteration must be **independent**
- no `break` / `continue`
- GPU math needs `libdevice.10.bc`
- `min`/`max`/`dot`/`transpose` are CPU for now (correct but not GPU-accelerated)

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
  not a copy; see design.md §6.5 copy constructor).

**Constructor ≠ `@startup`**: the constructor does **initialization** (synchronously on
`new`); `@startup` does **beginning operations** (runs when the instance's thread/event
loop starts, next section). They are orthogonal and do not replace each other.
Design: design.md §6.5.

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
> They are orthogonal and do not replace each other; design: design.md §6.5.

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
import gpu.hal;          // Standard library submodule (dotted name → stdlib/gpu/hal.myp)
import "./helper.myp";    // User file (relative path, resolved against the importing file's dir)
import "/abs/lib.myp";    // User file (absolute path)
```

### Import Rules

- Standard library is looked up in the `stdlib/` directory; **dotted module names**
  (`import gpu.hal;` → `stdlib/gpu/hal.myp`)
- User files support relative/absolute paths; relative paths resolve against the
  **importing file's directory**
- Automatic deduplication (same file is not imported twice; note: dedup is by path
  STRING — `..` is not normalized, so the same file via different relative paths
  (e.g. direct `./helper.myp` + `../helper.myp` from a submodule) can double-load and
  report duplicate — see tests/bugs/)
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

### `import option` — Nullable Container (v3.9.0)

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

### `import result` — Value-Based Errors (v3.9.0)

`Result<T, E>` is an Ok(value)/Err(error) two-state container — errors are passed
 explicitly as return values (detailed design/combinators: §4).

```myp
import result;

Result<int, string> ok  = new Result<int, string>(42);   // ok
Result<int, string> bad = new Result<int, string>();     // err
bad.setErr("oops");

if (ok.isOk())   Console.write(ok.get());        // 42
if (bad.isErr()) Console.writeString(bad.getErr());  // "oops"
int v = bad.getOr(-1);                          // safe access → -1
```

Factories (top-level generic functions): `resultOk<T,E>(v)` / `resultErr<T,E>(e)`;
combinators `resultMap` / `resultAndThen` / `resultMapErr`; exception bridge
`resultTry<T>(() => { ... })` turns a possibly-throwing call into `Result<T, string>`
(`throw "msg"` → `err(msg)`).

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

### `import setops` — Unified Set-Operator Contract

Abstracts "transform" as one interface: any operator (scale/activation/filter/reduce…)
that implements `transform` can be reused as a stage of a collection pipeline
(elementwise / size-changing / filtering is free inside the operator).

```myp
import setops;

// The SetOp contract is provided by the module: double[] transform(double[] A)
// An implementation only needs `interface class SetOp;` + transform to join pipelines
class ScaleOp {
    interface class SetOp;
    action:
        double[] transform(double[] A) {
            // any internal transform (example: identity; real ops do scaling etc.)
            return A;
        }
    property:
        double k = 2.0;
}

// Single operator:      double[] B = op.transform(A);
// Pipeline composition: double[] B = op2.transform(op1.transform(A));  // A ->op1-> op2-> B
```

> Unified operator model (operator = set operator): `docs/operators.md`; complements
> mapping's event operators (event-stream operators handle scalar events, set
> operators handle collection transforms).

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
Math.abs(-42);          // 42 (generic abs — absInt removed, int arg returns int)
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

### `import rtti` — Runtime Type Information / RTTI (§五-4, additive)

Every class instance's object header `{rc, type_id}` carries a runtime type id;
codegen emits `__myp_type_name_table` (type_id → class name string). The `Rtti`
static class exposes type-identity queries:

```myp
import rtti;

Tank t = new Tank();
string n  = Rtti.typeOf<Tank>(t);             // "Tank" (runtime class name)
int    id = Rtti.typeId<Tank>(t);             // runtime type id (same class → same, across classes → different)
int    ok = Rtti.sameType<Tank, Tank>(t, t2); // 1 (same type); across classes → 0

Tank nul = null;
Rtti.typeId<Tank>(nul);                       // 0 (null)
Rtti.typeOf<Tank>(nul);                       // "" (empty string)
```

- Generic T/U should be a direct class reference (an object); interface
  references should first be downcast to the concrete class with `isa`.
- Use for type-identity queries in logging / serialization / debugging; type
  conversion still uses `isa` (§三-4).

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

### `import http` — HTTP Client (§六-4, v3.9.0, additive)

An HTTP/1.1 client built on `net.myp`'s TCP (only `http://`, no TLS). Supports
GET/POST, URL parsing, status-line/header parsing, `Content-Length` bodies,
`Transfer-Encoding: chunked`, and close-delimited bodies.

```myp
import http;

HttpResult r = Http.get("http://example.com/api?page=1");
if (r.isOk()) {
    string body = r.getBody();          // response body
    string ct  = r.header("Content-Type");   // header (case-insensitive)
}
int status = r.getStatus();             // status code (0 = parse failure)
HttpResult p = Http.post("http://example.com/items", "{\"k\":1}");
```

Network-level errors throw: connect failure throws `NetError` (from
`TcpClient.connect`); malformed URL / non-`http` scheme throws a string exception
(`https` needs TLS and is not supported yet).

### `import net` — TCP Sockets

```myp
import net;

// --- Server ---
TcpServer srv = new TcpServer(8080);   // bind + listen
TcpClient cl = new TcpClient();
srv.accept(cl);                        // blocking accept (result into cl)
string data = cl.recvLine();           // blocking read one line
cl.send("Hello!\n");
cl.close();
srv.close();

// --- Client ---
TcpClient c = new TcpClient();
int ret = c.connect("example.com", 80);    // 0 on success; throws NetError on failure
c.sendLine("GET / HTTP/1.0");
string resp = c.recv(4096);
c.close();
```

- `TcpServer(port)` / `accept(client)` / `close()`
- `TcpClient.connect(host, port)` / `send(data)` / `sendLine(data)` / `recv(maxLen)` /
  `recvLine()` / `getFd()` / `close()`; connection failure throws `NetError` (with op/host/port)
- **Async**: `recvAsync` / `recvLineAsync` / `sendAsync` (see `import async`, §五-5 P2)

### `import text` — Text Processing

```myp
import text;

StringBuilder sb = new StringBuilder();
sb.append("Hello");
sb.append(", World");
string result = sb.toString();  // "Hello, World"
```

### `import atomic` — Atomic Operations

```myp
import atomic;

// Array atomic ops (thread-safe accumulation)
int[100] counters;
double[50] values;

Atomic.addInt(counters, idx, 1);        // counters[idx] += 1
Atomic.subInt(counters, idx, 1);        // counters[idx] -= 1
Atomic.addDouble(values, idx, 3.14);    // values[idx] += 3.14
Atomic.xchgInt(counters, idx, 0);       // counters[idx] = 0 (returns old value)
Atomic.loadInt(counters, idx);          // atomic read
Atomic.storeInt(counters, idx, 42);     // atomic write
```

Used with `@parallel for` to protect shared Tally arrays from concurrent writes.

### `import io` — File I/O

```myp
import io;

File f = new File();
f.open("data.txt", "r");           // open for read
bool has = f.hasNext();             // is there another line?
string line = f.readLine();         // read one line
f.close();                          // close

f.open("out.txt", "w");             // open for write
f.write("hello");                   // write string (no newline)
f.writeLine("world");               // write string + newline

// Binary I/O (File methods; `__myp_io_*` are stdlib-internal intrinsics and
// CANNOT be called directly from user code — that's a compile error)
File fb = new File();
fb.open("data.bin", "rb");
int b8 = fb.readByte();           // read 1 byte
int i32  = fb.readI32BE();        // read 4-byte big-endian int
fb.close();

fb.open("out.bin", "wb");
fb.writeByte(0xFF);               // write 1 byte
fb.writeI32BE(42);                // write big-endian 4-byte int
fb.writeDouble(3.14);             // write 8-byte double
fb.close();
```

### `import stream` — Streaming Data Sources

```myp
import stream;

// Event-driven data sources: run() iterates the source and fires the
// valueEmitted event, consumed via mapping()
RangeStream rs = new RangeStream();      // integer range [start, end) — one value per fire
IntStream is = new IntStream();          // int[] — one element per fire
DoubleStream ds = new DoubleStream();    // double[] — one element per fire

mapping() {
    rs.valueEmitted -> Console.write;        // int
    is.valueEmitted -> Console.write;        // int
    ds.valueEmitted -> Console.writeFloat;   // double
}

rs.run(0, 10);        // fires 0,1,...,9
is.run(data, n);      // fires data[0..n-1]
ds.run(data, n);      // fires data[0..n-1]
```

### `import barrier` — Barrier Synchronization

```myp
import barrier;

int handle = Barrier.create(4);      // create barrier (waits for 4 threads)
Barrier.wait(handle);                // wait until all threads arrive
Barrier.destroy(handle);             // destroy
```

### `import future` — Async Results

```myp
import future;

int handle = Future.create();        // create a Future
Future.set(handle, 42);              // set the result (producer)
int result = Future.get(handle);     // get the result (consumer, blocks)
Future.destroy(handle);              // destroy
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
long s  = Coro.sleep(ms);                      // §五-5 P1: suspend the current coroutine ms ms (doesn't block the thread; non-coroutine falls back to blocking sleep)
long f  = Coro.waitFd(fd, rd, wr, ms);         // §五-5 P2: wait for fd readable/writable; 1 when ready, -1 on timeout (call inside @coro)
long k  = Coro.waitAnyOf(spec, cnt, ms, val);  // §五-5 P4: unified wait mixing EVENT/TIMER/FD; returns fired spec index (see `import async`)
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

### `import async` — Async I/O Unified Abstraction (§五-5, v3.9.0, additive)

Unifies "waiting for something to complete" (timers / sockets / files / events) onto `await` + the
coroutine scheduler (reactor model, like Rust tokio / Node event loop). Design: `docs/async_io.md`.

**`@async` annotation + await form-3**: an action / `static:` method / top-level function marked
`@async` is called with `await f()` inside a `@coro` — it runs directly (parking/resuming internally
via the runtime), and `await`'s value = the function's return value (no yield-value handshake).
Sema rejects calling `@async` outside a `@coro` context.

**Async sleep (P1)**:
```myp
import coro;
import async;

@coro long worker() {
    await Async.sleep(100);      // suspend this coroutine 100ms, doesn't block the thread
    Console.writeString("woke\n");
    return 0;
}
```
Equivalent low-level `Coro.sleep(ms)` (`Async.sleep` wraps it).

**Async sockets (P2, `import net`)**: fd is set O_NONBLOCK; the scheduler batch-polls each round and
resumes a coroutine only when the fd is readable/writable:
```myp
@coro long client() {
    TcpClient c = new TcpClient();
    c.connect("127.0.0.1", 8080);
    string d1 = await c.recvAsync(4096);   // non-blocking read once readable
    string l1 = await c.recvLineAsync();   // byte-by-byte to \n
    long n  = await c.sendAsync("hi");     // send once writable
    c.close();
    return 0;
}
```
With timeouts: `recvAsync(maxLen, timeoutMs)` returns "" on timeout; `recvLineAsync(timeoutMs)`
returns what was received; `sendAsync(data, timeoutMs)` returns bytes sent.

**Async files (P3, `import io`)**: blocking fgets/read-all run on a worker thread pool; results are
delivered cross-thread and the coroutine resumes (`readAllAsync` returns the rest incl. trailing \n):
```myp
@coro long reader() {
    File f = new File();
    f.open("/tmp/x.txt", "r");
    string line = await f.readLineAsync();   // worker-thread read line
    string all  = await f.readAllAsync();    // worker-thread read all
    f.close();
    return 0;
}
```

**Unified waitAnyOf (P4, `Coro.waitAnyOf`)**: wait on a mix of EVENT/TIMER/FD at once, returning the
index of the spec that fired first. `spec` is a flat `long[]`, 3 entries per spec `[kind, id, flag]`:
`kind` 0=EVENT (id=event id, flag=0), 1=TIMER (id=-1, flag=relative ms), 2=FD (id=fd, flag=1/2/3
read/write/both). Returns fired spec index (0-based), -1 on overall timeout, -2 outside a coroutine:
```myp
@coro long waitMix(int fd) {
    long[] spec = new long[9];
    spec[0] = 0; spec[1] = 0;  spec[2] = 0;     // EVENT event id 0
    spec[3] = 2; spec[4] = fd; spec[5] = 1;     // FD read
    spec[6] = 1; spec[7] = -1; spec[8] = 300;   // TIMER 300ms
    return Coro.waitAnyOf(spec, 3, 1000, 0);    // index of the first that fired
}
```

> **Mechanism**: the wait table is unified as `myp_coro_wait_t.kind` — 0=EVENT / 1=TIMER / 2=FD /
> 3=EXEC. Each scheduler round: process events → expire deadlines → batch-poll fds → executor inbox
> → ready snapshot. Fully additive; coexists with `await ClassName.eventName` / `Coro.waitEvent*`.

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

> **Use cases**: ① **Deterministic release** — for temporaries with a clear lifetime, `Memory`
> releases immediately (controls peak memory). ② **FFI pointer interop** — raw pointers passed
> to C libraries (SDL/net/GPU/third-party). ③ **Byte buffers / manual layout** — raw buffers
> for binary protocols and file formats. For dynamic arrays use `ArrayList<T>` from `collections`
> (auto-growing); this module only manages raw memory.

**Memory diagnostics (M9)** — `Memory` static methods expose live counts and resource usage
for leak / peak-regression diagnosis (combine with `Rtti` for per-type statistics):

```myp
// Live counts (thread-local)
long n = Memory.liveObjectCount();           // total live class instances
long s = Memory.liveStringCount();           // live counted strings
long a = Memory.liveArrayCount();            // live counted array/slice backings
long t = Memory.liveTotalCount();            // sum of the above
long bt = Memory.liveObjectCountByType(tid); // instances of a runtime type_id (Rtti.typeId)

// arena / @region bytes (reserved/used)
Memory.arenaReservedBytes();  Memory.arenaUsedBytes();
Memory.regionReservedBytes(); Memory.regionUsedBytes();

// coroutine resources (thread-local)
Memory.coroSlotCount();      Memory.coroSlotCapacity();   Memory.coroFreeSlotCount();
Memory.stackPoolCount();     Memory.stackPoolBytes();     Memory.stackPoolMaxBytes();
Memory.stackPoolCapacity();  Memory.retiredCount();       Memory.retiredBytes();

// deterministic allocation-failure injection: abort on the nth allocation
// (or set env MYP_FAIL_ALLOC=n at startup)
Memory.failAllocEnable(nth);  Memory.failAllocDisable();  long x = Memory.failAllocGet();

// strict header checks: release underflow / double free / corrupted header abort;
// on by default in ASAN builds
Memory.setStrictChecks(1);  long on = Memory.getStrictChecks();
```

### Memory model & weak references (ARC + `@weak`)

Class instances, `string`, dynamic arrays and `slice` backings are all **automatically
reference-counted** (ARC): they are released when a reference goes out of scope / is
overwritten — no manual `delete`, no GC pauses. `Memory.liveObjectCount()` observes live
instances (leak detection).

**`@weak` references (M7)** — the default reference is **strong** (holds the object, prevents
reclamation); cyclic references (bidirectional parent/child, callbacks capturing `self`,
observers) make strong references lock each other and leak. Use `@weak` on **one side** of the
cycle: a weak reference does **not** increment the count, and it is **auto-nulled** when the
target is destroyed (reading it yields `null`, never a dangling pointer).

```myp
class Parent {
    property: Child child;        // strong: holds child
}
class Child {
    property:
        @weak Parent parent;      // weak: no count; auto-null after Parent dies
}

Parent p = new Parent();
Child c = new Child();
p.child = c;          // strong: p → c
c.parent = p;         // weak: c → p (does not prevent p's reclamation)
Parent q = c.parent;  // weak→strong upgrade: strong ref if p is alive; null if destroyed
```

- `@weak` is only allowed on **class / interface reference fields** (`string`/`slice`/numeric
  and struct fields are rejected at compile time).
- Reading a weak field is a one-shot "weak→strong upgrade": a strong reference if the target is
  alive, `null` if it was destroyed — **always null-check** the result.
- Use for: back-references (child→parent), observers/subscribers, callbacks capturing `self`.
  Use the default strong reference when there is no cycle.

### `import channel` — Coroutine Channels

Go-style buffered channel (owned by the creating thread's TLS, consistent with the coroutine model):
```myp
import channel;
Channel ch = new Channel();
ch.init(4);                    // buffer capacity > 0
ch.send(v);                    // in a coroutine: suspend when full; non-coroutine: -1 when full
long v = ch.recv();            // in a coroutine: suspend when empty; non-coroutine: -1 when empty
ch.trySend(v);  ch.tryRecv();  // always non-blocking (0/value, -1 full/empty)
ch.size();                     // current buffered element count
ch.close();                    // close and wake all waiters
ch.destroy();                  // release the buffer
```

### `import fs` — File System

```myp
import fs;
Fs.exists("/tmp");         Fs.isDir("/tmp");        Fs.isFile("/tmp/x");
long sz = Fs.fileSize("/tmp/x");
long mt = Fs.modifiedTime("/tmp/x");
Fs.dirname(p);   Fs.basename(p);   Fs.join(dir, file);
string[] files = Fs.listDir("/tmp");      // filename array (dynamic, no 1024 cap)
Fs.listCount("/tmp");
Fs.mkdirP("/a/b/c");                      // recursive mkdir (mkdir -p)
Fs.removeRecursive("/a");                 // recursive delete (rm -rf)

// Path wrapper: path-oriented methods
Path p = new Path("/home/user/file.txt");
p.dirname();  p.basename();  p.join("x.txt");
p.exists();  p.isDir();  p.isFile();  p.fileSize();  p.modifiedTime();
p.listDir();  p.toString();
```

### `import process` — Process Management

```myp
import process;
int code = Process.run("ls -l");          // run command, return exit code
string out = Process.output("uname -a");  // run and capture stdout
int pid  = Process.getPid();              // current process PID
int ppid = Process.getParentPid();        // parent process PID
int alive = Process.isRunning(pid);       // is the process running?
```

### `import args` — Command-Line Arguments

```myp
import args;
int n = Args.count();                   // argument count (including program name)
string a = Args.get(i);                 // i-th argument (0 = program name)
Args.hasOption("-v");                  // does an option exist?
string v = Args.getOption("-o", "def");// option value (default if absent)
```

### `import json` — JSON Parsing

```myp
import json;
Json doc = new Json("{\"name\":\"myp\",\"v\":1}");  // throws JsonError on invalid input
string n = doc.getString("name");
int v    = doc.getInt("v");
double d = doc.getDouble("pi");
int b    = doc.getBool("ok");
int len  = doc.arrayLen("items");      // array length
int t    = doc.type("field");          // value type
string g = doc.getString("a.b[0]");    // path supports nested fields / indices

doc.free();
```

### `@derive(Json)` — Derived Serialization (auto toJson/fromJson)

The class annotation `@derive(Json)` makes the compiler auto-inject
`toJson()` / `fromJson(string)` methods, eliminating hand-written serialization:

```myp
import json;   // needs Json.escape + Json path queries

@derive(Json)
class Player {
    property:
        string name;
        int hp;
        bool alive;
}

Player p = new Player();
// after assigning p.name / p.hp / p.alive:
string j = p.toJson();          // {"name":"A","hp":100,"alive":true} (strings escaped)
Player q = new Player();
q.fromJson(j);                  // round-trips all fields
```

**v1 rules**:
- Supported property types: `int/long/short/byte/uint/ulong/ushort/ubyte`,
  `double/float`, `bool`, `string`; array/class/struct/tuple/function properties are
  diagnosed at compile time (unsupported).
- Generic classes with `@derive` are not yet supported (compile-time diagnostic);
  non-`Json` derive names are diagnosed.
- Injected methods live inside the class, so they can access private properties.

### `import regex` — Regular Expressions

```myp
import regex;
Regex re = new Regex("^[A-Z][a-z]+$");   // POSIX extended regex
re.test("Hello");   // 1
re.test("hello");   // 0
re.free();
```

### `import base64` — Base64

```myp
import base64;
string enc = Base64.encode("hello");
string dec = Base64.decode(enc);
```

### `import date` — Date & Time

```myp
import date;
long ms  = Date.nowMs();                 // wall-clock milliseconds
string s = Date.now();                   // "YYYY-MM-DD HH:MM:SS"
string f = Date.formatNow("%Y-%m-%d");  // formatted current time
string t = Date.format(ms, "%H:%M:%S"); // formatted given time
int y = Date.getYear();  Date.getMonth();  Date.getDay();
int h = Date.getHour();  Date.getMinute(); Date.getSecond();
Date.getWeekday();  Date.getDayOfYear();
Date.getYearOf(ms);  Date.getMonthOf(ms); Date.getSecondOf(ms);  // fields of a given time
```

### `import logger` — Logging

```myp
import logger;
Logger log = new Logger("myapp");
log.debug("...");  log.info("...");  log.warn("...");  log.error("...");
log.setLevel(0);   // 0=DEBUG 1=INFO(default) 2=WARN 3=ERROR (LogLevel enum)
log.getLevel();
```

### `import timeline` — Timeline

```myp
import timeline;
Timeline tl = new Timeline();
long now = tl.now();  tl.sleep(100);
tl.startTimeout(1000);   // fires timeout(ms) event after 1s
tl.startInterval(500);   // fires interval(ms) event every 500ms
tl.startTick(200);       // fires tick() event every 200ms
// events: timeout(long ms) / interval(long ms) / tick()
Stopwatch sw = new Stopwatch();  sw.start();  ...  sw.elapsed();
```

### `import test` — Test Assertions

```myp
import test;

Test.assert(1 == 1);                 // assert a condition is true
Test.assertEq(2 + 2, 4);             // assert int equality
Test.assertStrEq("hi", "hi");        // assert string equality
Test.report("test_name", true);      // report a test result
```

### `import sdl` — SDL Graphics Window

```myp
import sdl;

// SDL2-based window and input management
SDL.open("Title", 800, 600);            // create window
while (SDL.running()) {
    SDL.clear(0, 0, 0, 255);            // clear screen
    SDL.drawRect(10, 10, 100, 50, 255, 0, 0, 255);  // filled rect
    SDL.drawLine(0, 0, 800, 600, 0, 255, 0, 255);   // line
    SDL.present();                       // present
}
SDL.close();                            // close window

int key = SDL.getKey();                  // get a key press
int w = SDL.width();  int h = SDL.height();  // window size
```

### `import gpu` — GPU High-Level API (L1 array primitives + L3 submodules)

`import gpu` provides the `Gpu` static class: host array primitives, auto-GPU-accelerated;
falls back to **CPU** without a GPU / when `MYP_GPU=1` is unset, with identical results.

```myp
import gpu;

Gpu.add(a, b, out, n);          // out = a + b
Gpu.sub(a, b, out, n);          // out = a - b
Gpu.mul(a, b, out, n);          // out = a .* b (elementwise)
Gpu.scale(data, s, n);          // data[i] *= s
Gpu.addScalar(data, s, n);      // data[i] += s
Gpu.saxpy(3.0, x, y, out, n);   // out[i] = 3.0*x[i] + y[i]
Gpu.copy(dst, src, n);          // dst = src
Gpu.negate(data, n);            // data = -data
Gpu.clamp(data, lo, hi, n);     // data = clamp(data, lo, hi)
Gpu.sum(a, n);                  // Σ a[i]
Gpu.dot(a, b, n);               // dot product
Gpu.mean(a, n);  Gpu.variance(a, n);  Gpu.stddev(a, n);
Gpu.norm(a, n);  Gpu.normSquared(a, n);  Gpu.normalize(data, n);
Gpu.gemm(A, B, C, m, n, k);     // matrix multiply (row-major double[])
Gpu.sqrt(data, n);  Gpu.exp(data, n);  Gpu.log(data, n);  Gpu.abs(data, n);  Gpu.pow(data, p, n);
```

> **L3 — `gpu/` submodules (dotted imports, explicit device memory)**:
> - `import gpu.memory;` — `GpuBuffer`/`GpuBufferF` (device buffers), `GpuPool` (pool)
> - `import gpu.ops;` — `GpuOps` device-side operators (`*D` consume devicePtr directly)
> - `import gpu.stream;` — `GpuStream`/`GpuEvent` (async streams/events)
> - `import gpu.algo;` — `GpuAlgo` data-parallel algorithms (histogram/compact/unique)
> - `import gpu.graph;` — `GpuGraph`/`GpuGraphExec` (CUDA Graph capture/replay)
> - `import gpu.byoc;` — `GpuByoc`/`GpuLib` (BYOC self-compiled kernels + cuBLAS)
> - `import gpu.device;` — `GpuDevice` (device attribute queries)

```myp
import gpu.memory;

double[] host = new double[1024];
GpuBuffer buf = new GpuBuffer(host, 1024);   // wrap host array → device memory
buf.copyToHost(host, 0, 0, 1024);            // device → host
```

### `import ui` — Terminal TUI Framework

```myp
import ui;

// Pure-MYP implementation rendering via ANSI escape codes
Window win = new Window(0, 0, 80, 24, "MyApp");
win.add(new Button(10, 5, 12, 3, "Click"));
win.add(new ProgressBar(10, 10, 40, 3, 0.5));
win.render();                            // render one frame

// Components: Window, Label, Button, TextBox, ProgressBar
```

---

## 12. Metaprogramming

MYP provides **compile-time code generation and evaluation**, covering four layers
(all additive; language spec v1.0 unchanged):

| Layer | Capability | Scope | Since |
|---|---|---|---|
| Generic monomorphization | one logic, many types (`Box<int>` generates code independently) | type-level | base |
| `@eval` compile-time evaluation | compute constants / derive config at compile time | value-level | v3.4 |
| `macro` declarative macros | AST fragment substitution (code templates) | syntax-level | v3.5 |
| `@macro` procedural macros | execute a macro function at compile time, generate AST algorithmically | full | v3.6 |

Division of labor: **generics** = type parameterization; **`@eval`** = compute values at
compile time; **`macro`/`@macro`** = eliminate repetitive code (template substitution vs.
programmable generation). Full design: `docs/metaprogramming.md` and `docs/design.md` §11.

### `@eval` — compile-time evaluation (v3.4)

`@eval`-annotated functions execute **at compile time** (pure-function subset); results
become compile-time constants:

```myp
@eval int fib(int n) {
    if (n < 2) return n;
    return fib(n - 1) + fib(n - 2);
}

const int FIB10 = fib(10);          // 55, computed at compile time
const int FIB20 = fib(20);          // 6765
const double HALF = 10.0 / 4;       // 2.5 (const RHS also compile-time evaluated)
const bool BIG = FIB10() > 50;      // @eval/const can call each other
const int T5 = triple(FIB10());     // 165
const long BIGL = 100000L * 10L;    // 1000000
```

- **Ordinary functions can call `@eval` functions** (compile-time constant folding):
  `int doubleFIB10() { return 2 * fib(10); }` → always 110 at runtime.
- **Timing**: after sema, before codegen (`evaluateCompileTimeConstants`); the result is
  embedded as an LLVM compile-time constant (e.g. `ret i32 55`).
- **Pure-function constraint** (compile-time execution must be side-effect-free):

| Allowed | Forbidden |
|---|---|
| scalar/array ops, recursion, conditionals/loops, `@eval`/`const` calls | `new` instance creation, I/O, external state, `@thread`, mapping/events |
| compile-time constant args | runtime-dependent args |

- **Diagnostics (compile-time error, compiler never crashes)**: non-pure construct →
  `compile-time evaluation: construct not supported in @eval context`; deep recursion →
  `compile-time evaluation: recursion depth exceeded in 'fib'`.

### `macro` — declarative macros (v3.5)

`macro` is a **top-level declaration** (parallel to class/struct/enum): a template of MYP
code with metavariables `$ident`; calls substitute actual **AST fragments** for the
metavariables and expand into a syntax tree:

```myp
macro repeat($n, $body) {
    for (int _i = 0; _i < $n; _i++) { $body }
}
macro addN($x, $n) {
    $x = $x + $n;
}
macro twice($body) {
    $body
    $body
}

int v = 0;
repeat(3, v = v + 10);   // expands to a for loop ×3 → v = 30
addN(v, 5);              // v = 35
twice(v = v + 1);        // nested/repeated expansion → v = 37
```

- Args can capture **expression / statement / assignment** AST fragments (substituted by
  call context).
- Macros can call macros and expand **iteratively** (until stable or a depth cap).
- **Timing**: after parse, before sema (`expandMacros`); pure compile-time AST transform,
  no execution.
- Debug: `--macro-expand` dumps the expanded AST (`[function main]` / `[action ...]` /
  `for` / `assign` …).

### `@macro` — procedural macros (v3.6)

`@macro`-annotated functions execute **at compile time** (loops/conditionals/algorithm-driven),
returning AST values (`Expr` / `Stmt` / `StmtList`), and use `quote { ... }` code templates to
generate code — enabling what `macro` templates can't ("generate n statements from n"):

```myp
@macro StmtList genAssign(string name, int value) {
    return quote { int $name = $value; };   // $name/$value compile-time interpolation
}

@macro StmtList makeCalls(int n) {
    StmtList out = quote {};                // empty statement list
    for (int i = 0; i < n; i++) {
        out = out + quote { Console.write($i); };   // StmtList concatenation
    }
    return out;
}

genAssign("x", 42);      // statement-position call → expands to `int x = 42;`
makeCalls(3);            // generates Console.write(0); Console.write(1); Console.write(2);
```

- **`quote { ... }`**: compile-time expression that parses the block into an AST (statement
  collection); `$x` interpolation embeds by compile-time value type:

  | `$x` compile-time value type | embedded as |
  |---|---|
  | `int`/`long`/`double`/`float`/`bool`/`string` | corresponding literal AST node |
  | `Expr` | inline that expression AST |
  | `StmtList` / `Stmt` | inline that statement (group) AST |

- **AST value types**: `Expr` / `Stmt` / `StmtList` are **compile-time-only types** (cannot
  be ordinary runtime variable/parameter types; sema-checked); `StmtList + StmtList` concatenates.
- **No runtime code emitted**: `@macro` functions are only executed at compile time; sema
  registers but never emits them.
- **Timing**: same as `macro` (after parse, before sema); expansion depth / instruction
  count have caps (guard against runaway loops).
- **Diagnostics**: interpolation type mismatch (`$x` is `int` but a statement is needed),
  return-type mismatch (declared `StmtList` but `quote` yields `Expr`) → compile-time error.

### Design principles

- **Additive**: introduced via annotations/keywords, breaks no existing syntax (language
  spec 1.0 unchanged).
- **Compile-time determinism**: compile-time-executed code must be pure — same input,
  same output.
- **Observable**: `--macro-expand` debug switch outputs the expansion result (AST dump).
- **Built on existing generics**: generic monomorphization is the most basic "type-level
  metaprogramming"; macros/compile-time evaluation build on top of it.

---

## 13. Compilation & Tools

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
| `--frontend-dump <tokens|ast|sema>` | Deterministic frontend dump (self-hosted compiler Oracle contract) |
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

#### Codegen (LLVM Backend)

MYP compilation pipeline: `lexer → parser → sema → codegen (build LLVM IR in memory) → opt (-O1+ optimization pipeline) → object file → link`.

- **IR generation**: codegen compiles every function / class method / mapping / coroutine
  into LLVM IR (`src/codegen/`): variables live in allocas, objects go through ARC
  (`retain`/`release`), events through the runtime registry. Source split:
  `codegen.cpp` (translation unit / globals) / `codegen_class.cpp` (class methods /
  constructors) / `codegen_stmt.cpp` (statements) / `codegen_expr.cpp` (expressions) /
  `codegen_gpu.cpp` (`@gpu for` NVPTX kernels) / `codegen_test.cpp` (`--test` runner).
- **Internalization (IPO)**: non-library builds internalize all function definitions
  (only `main` stays external), so LLVM cross-function IPO can constant-specialize and
  inline hot kernels.
- **`-O` optimization pipeline**: `-O0` (default) runs no optimization (all allocas,
  debug-friendly); `-O1/-O2/-O3` run the LLVM standard pipeline via `PassBuilder`
  (New Pass Manager) — `-O1` mem2reg/instcombine/GVN/DCE/basic inline/simple loops;
  `-O2` adds SROA/more aggressive inline/loop unroll & vectorization; `-O3` even more
  aggressive (may grow code size).
- **Custom pass (`--passes myp-pass`)**: `MypRedundantStorePass` encodes MYP-specific
  semantics — removes **dead stores** produced by codegen (e.g. local double-init
  `store i32 0, %x` immediately overwritten with no reads in between). Conservative
  rule: within one basic block, two stores to the same address with no
  load/store/call/atomic/fence between them → the earlier store is deleted.
- **`--emit-llvm`**: saves the IR text to `.ll` and skips linking — for checking
  whether optimization took effect (e.g. that mem2reg promoted loop variables) and
  for oracle comparison.
- **`MYP_FAST_MATH=1`**: marks all FP ops with fast-math flags (reassoc/contract/nnan…),
  letting LLVM vectorize FP reductions and contract FMAs; off by default (strict IEEE,
  matching the -O0/-O2 baseline).
- **Semantics interaction**: optimization passes must be regression-checked against
  runtime mechanisms — setjmp/longjmp exceptions, ucontext coroutines, arena
  allocation (`myp_region_alloc`), etc.; the full suite runs at both **-O0 and -O2**,
  and any semantics-breaking pass is disabled or reordered.
- Design & debugging: `docs/optimization_debugging.md`.

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

### Self-Hosted Compiler (myp_self)

MYP's compiler itself is being **fully rewritten in MYP** (T5 self-hosting project,
`tools/selfhost/`): frontend (lexer/parser/sema) + codegen (incl. GPU NVPTX emission) +
CLI driver are all implemented in MYP, delivering the self-hosted compiler `myp_self`,
with the classic two-stage bootstrap verification.

- **Build**: stage0 C++ `mypc` compiles `tools/selfhost/src/*.myp` → `build/myp_self`;
  `myp_self` then compiles itself → `build/myp_self2` (the full self-hosted binary in
  build/ today).
- **Modules** (`tools/selfhost/src/`): `token` / `lexer` / `ast` / `parser` / `diag` /
  `sema` / `ir_emit` / `codegen` / `link` / `main` (CLI driver).
- **Usage** (same shape as mypc):

```bash
./build/myp_self myapp.myp -o myapp                 # compile
./build/myp_self run myapp.myp args...              # compile+run (native, no mypc)
./build/myp_self --frontend-dump ast myapp.myp      # frontend dump (tokens|ast|sema)
./build/myp_self --emit-llvm myapp.myp              # output .ll
./build/myp_self fmt [--check] myapp.myp            # format (delegates to self-hosted myp_fmt2)
```

- **codegen strategy**: MYP cannot reuse the LLVM C++ API in-process → **emits LLVM IR
  text (.ll)**, then calls external `llc` (LLVM backend) + `gcc` to link the C runtime;
  naturally comparable with `mypc --emit-llvm`.
- **Oracle comparison**: the frontend uses `mypc --frontend-dump {tokens,ast,sema}`'s
  deterministic output as the contract, byte-compared; the backend compares IR text plus
  **generated-program run output** (primary acceptance).
- **Bootstrap (two-stage fixpoint)**: stage1 C++ compiles `myp_self` → stage2 `myp_self`
  compiles itself → stage3 re-compiles itself; two generations behaving identically
  establishes self-hosting. Measured: `self2 → self3 → self4` byte-identical
  (md5 `52c81186…`).
- **GPU**: implemented — `@gpu for` (and `@gpu tile/scatter/reduce/scan`) generates an
  NVPTX kernel (standalone .ll module → `llc -mtriple=nvptx64-nvidia-cuda` → PTX →
  embedded string global), with host-side GPU/CPU dual-path launch (`MYP_GPU=1` real-device
  launch, auto-falls-back to serial CPU with identical results); `kernel.*` context and
  vector types (float4/double2/int4) are supported. Only the C runtime `runtime.c` stays C
  as the generated program's "libc".
- **Progress** (`tools/selfhost/roadmap.md`): frontend F0–F4, backend G1–G4, bootstrap
  verification H1 all done; P phase closed (P2 generated-code performance on par with mypc —
  `-mtriple` enables TTI vectorization, matmul 2.43x→1.00; P3 dropping the C++ seed drill —
  rebuilt the whole toolchain with only `myp_self2`, no mypc).
- See `docs/self_hosting.md` and `tools/selfhost/{README,design,roadmap,format}.md`.

### Code Generation Tool (tools/codegen)

Schema-driven **code generation framework** (pure MYP, modeled on PyTorch torchgen / gRPC
IDL): declarative schema (JSON) → auto-generated MYP/C source, replacing hand-written
serialization/bridge/boilerplate (MYP has no runtime reflection; code generation is the
systematic "reflection substitute"). Design: `tools/codegen/design.md`.

```bash
# CLI: mypc run tools/codegen/main.myp <generator> <schema.json> [-o outdir] [--verify]
./build/mypc run tools/codegen/main.myp serde player.schema.json -o gen/
./build/mypc run tools/codegen/main.myp ffi c_api.schema.json -o gen/ --verify
```

**Built-in generators** (all round-trip verified):

| Generator | schema | output |
|---|---|---|
| `serde` | types | per-class `toJson()/fromJson()` (MYP) |
| `ffi` | ffi signatures / resources | `ffi` declarations + C bridge + resource RAII wrappers |
| `autodiff` | exprs | forward + gradient functions (numeric gradient vs finite difference) |
| `idl` | services/methods | client/server stubs + JSON-RPC codec (with net/json) |
| `orm` | tables | entity structs + CRUD SQL |
| `embed` | file paths | file → string constant (byte-level round-trip) |
| `dsl` | operator table | lexer + precedence-climbing parser + evaluator |
| `infer_ops` | ops | CPU/GPU dual per-element kernels |

**Flow**: JSON schema → `schema.myp` parses → `model.myp` model (typed) → `emit.myp`
emitter (indent/escape/multi-language) → writes source file; `--verify` then compiles the
output via `mypc --emit-llvm` to check it is valid compilable code with legal IR.

```json
// player.schema.json (schema fragment)
{
  "types": [
    { "name": "Vec2", "kind": "struct",
      "fields": [ {"name": "x", "type": "double"}, {"name": "y", "type": "double"} ] },
    { "name": "Player", "kind": "class",
      "fields": [
        { "name": "name", "type": "string" },
        { "name": "pos",  "type": "Vec2", "ref": "type" },
        { "name": "items","type": "int", "array": true }
      ] }
  ]
}
```

- `kind`: `struct`/`class`/`enum`/`op`/`ffi`; `ref: type` references another schema type;
  `array: true` dynamic array.
- Self-test: `bash tools/codegen/run_tests.sh` (wired into `tests/run_tests.sh`).

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

### Stress Testing (tests/stress/)

Stress tests for the runtime (ucontext coroutines, channels, async I/O, thread
pool): hammer it with loads far beyond normal tests to find **crashes, memory
leaks, deadlocks, data races, coroutine/handle leaks**. Unlike `bench/` (which
measures performance against C++/Go), stress focuses on **stability** — no
crash/leak/deadlock under pressure and correct results. Because the load is
heavy and output includes timing data (ms / worker count), it is **separate from
`run_tests.sh`** — run it on demand:

```bash
bash tests/stress/run_stress.sh                # all (-O2)
bash tests/stress/run_stress.sh coro_flood     # a specific test
TSAN=1 bash tests/stress/run_stress.sh         # ThreadSanitizer for data races
ASAN=1 bash tests/stress/run_stress.sh         # AddressSanitizer for memory errors
```

| Test | What it stresses | Verification |
|------|------------------|--------------|
| `coro_flood` | mass coroutine create/destroy (natural reclamation + `destroy` hard-kill) | `Coro.count()` returns to 0, no leak |
| `coro_switch_storm` | context-switch throughput (ucontext swapcontext) | all finish, sums exact |
| `channel_stress` | multi-producer/multi-consumer channels (cap=1 ping-pong + cap=8 batch) | no deadlock, Σrecv == expected |
| `async_io_stress` | async loopback TCP (coroutines `await recvAsync`) | all clients get correct payload |
| `parallel_stress` | `@parallel for` + Atomic high contention | Σ exact == n, ≥2 workers engaged |

> Each test prints `PASS <name>`; the runner judges by exit code + PASS; exit
> 0 = all pass, 1 = failures. This suite reproduced and fixed the intermittent
> `@parallel for` hang (counter-reset race, see CHANGELOG).

### How to Add Tests (@test suite)

Write new test cases with the **language-built-in `@test` suite** and drop them in
`tests/@test/`; `run_tests.sh` auto-discovers them (each compiled+run with `--test`,
requiring exit 0 and no `FAIL:`):

```myp
// tests/@test/my_feature.myp
import test;

@test void test_feature() {
    Test.assertEq(compute(), 42, "compute result");
    Test.assertNotNull<Node>(node);
    Test.report("test_feature", true);   // optional: PASS/FAIL marker
}
```

```bash
./tests/run_tests.sh        # auto-compiles+runs tests/@test/*.myp
```

Convention: one `.myp` file per group of related tests; mark functions with `@test`;
a failed assertion automatically makes the exit code non-zero (no manual handling).
Use `Test.assert(cond, msg)` with a message for deterministic-input checks.

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
export MYP_PACKAGE_PATH=/path/to/packages:/path/to/more   # package manager myp build extra search paths (colon-separated)
export MYP_FAST_MATH=1                                     # codegen: FP fast-math (vectorized reductions/FMA; default strict IEEE)
export MYP_FMT=./build/myp_fmt2                            # self-hosted compiler fmt subcommand formatter path (optional)
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
├── tools/               # self-hosted MYP tools: tools/pm (package manager → build/myp), tools/fmt, tools/viz, tools/selfhost (self-hosted compiler → build/myp_self), tools/codegen (schema-driven codegen)
├── CMakeLists.txt
├── include/mylang/      # compiler headers: AST/CodeGen/Sema/Parser/Lexer/Eval/
│   └── ...              #   Macro/MypPasses/Fmt/LSP/Token/Type/...
├── src/                 # compiler source
│   ├── main.cpp         # mypc driver (lexer→parser→sema→codegen→link)
│   ├── ast/ lexer/ runtime/ eval/ macro/ fmt/ lsp/ dap/   # see per-directory roles below
│   ├── parser/          # syntax analysis (parser / parser_expr / parser_stmt, split by concern)
│   ├── sema/            # semantic analysis (sema / sema_expr)
│   ├── codegen/         # LLVM codegen (codegen / codegen_class / codegen_stmt / codegen_expr / codegen_gpu + myp_passes)
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
│                        #   stress/ (coroutine/concurrency stress, run_stress.sh)
├── examples/            # complete examples (hello/fib/ad/BNCT/sdl/tui)
├── BNCTDoseEngine/      # BNCT Monte-Carlo engine (pure MYP + HDF5 cross-sections)
├── deeplearning/        # MLP + MNIST training/inference
├── vscode-myp/          # VS Code extension (syntax highlight + LSP + DAP)
├── docs/                # design/grammar/manual/manual_en/coro/exceptions/
│   └── ...              #   operators/metaprogramming/constructor/...
├── build/               # build outputs: mypc, myp, myp_fmt2, myp_viz2, myp_self, myp_self2, myp_debug, myp_lsp, myp_viz, myp_fmt
└── build-asan/          # ASAN/UBSAN build
```

## 14. Complete Example

### IoT Temperature Monitoring System

```myp
// ===== iot_monitor.myp =====
import env;
import timeline;
import math;

// === Sensor Component ===
class TempSensor {
    action:
        @constructor TempSensor() {
            t = new Timeline();
        }
        @startup void run() {
            // Read temperature every 2 seconds
            t.startInterval(2000);
        }
        double readValue() {
            // Simulate temperature reading
            return 20.0 + Math.sin(t.now() / 1000.0) * 5.0;
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

    mapping() {
        // Temperature reading → display and log (nodes use class names)
        TempSensor.temperatureRead -> Display.show, Logger.log;

        // Temperature reading → alarm check
        TempSensor.temperatureRead -> Alarm.check;

        // Alarm triggered → sound + prominent display
        Alarm.alarmTriggered -> Alarm.sound, Display.showAlert;
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
