# KamiPython — Architecture Analysis (PHASE 0)

> Detailed design document written before any code. Vietnamese version: [`docs/vi/ARCHITECTURE.md`](../vi/ARCHITECTURE.md)

---

## 1. Goals

Build a complete **AOT (Ahead-Of-Time)** compiler in **C++20** that turns a Python-like language into a **native executable**, with no dependency on Python/CPython/PyInstaller/Nuitka.

```
Input:  main.py          (Python-like source)
Build:  kamipy build main.py -o app
Output: app / app.exe    (native binary, self-contained runtime)
Run:    ./app  →  30
```

**Technical constraints:**

| Item              | Decision                            |
|-------------------|-------------------------------------|
| Language          | C++20                               |
| Build system      | CMake ≥ 3.20                        |
| Backend           | LLVM IR + LLVM Backend (LLVM 18)    |
| Platform          | Linux x64, Windows x64              |
| Runtime           | Hand-written (libkamirt), static-linked |
| Hello world size  | < 5 MB                              |
| Hello world RAM   | < 20 MB                             |

---

## 2. End-to-end compilation pipeline

```
 main.py
   │  (Lexer)          — read UTF-8, emit token stream, handle INDENT/DEDENT
   ▼
 Token Stream
   │  (Parser)         — recursive descent + Pratt for expressions
   ▼
 AST                    — arena-allocated, lives for the whole compile session
   │  (Semantic)       — symbol tables, scopes, name resolution, type inference
   ▼
 Typed AST
   │  (Optimizer)      — constant folding, DCE, small-function inlining (on KIR)
   ▼
 KIR (Kami IR)          — simple 3-address mid-level IR, easy to optimize
   │  (Codegen)        — lower KIR to LLVM IR, call runtime via C ABI
   ▼
 LLVM IR (Module)
   │  (LLVM passes)    — mem2reg, instcombine, gvn, O2 pipeline
   ▼
 Machine Code (.o / .obj)
   │  (Linker)         — clang driver / lld, static link libkamirt.a
   ▼
 Executable (ELF / PE)
```

**Principle:** every layer has well-defined input/output, is independently testable, and never reaches into another layer's internals.

---

## 3. Directory layout

```
KamiPython/
├── compiler/
│   ├── lexer/        # Tokenizer, indentation handling
│   ├── parser/       # Recursive descent + Pratt
│   ├── ast/          # Node definitions, arena, printer/dumper
│   ├── semantic/     # Symbol table, scopes, type inference
│   ├── optimizer/    # Constant folding, DCE, inliner (on KIR)
│   ├── ir/           # KIR: definitions + builder + verifier
│   ├── codegen/      # KIR → LLVM IR, runtime ABI declarations
│   └── linker/       # Driver: invoke clang/lld, static link runtime
├── runtime/
│   ├── value/        # Value tagged union, boxing/unboxing
│   ├── object/       # Object header, type table, method dispatch
│   ├── memory/       # Allocator interface, page allocator
│   ├── allocator/    # Size-class freelist allocator
│   ├── gc/           # Reference counting (+ cycle detector later)
│   ├── string/       # Immutable string: len + hash + data
│   ├── array/        # Dynamic array of Value
│   ├── hashmap/      # Open addressing, robin-hood
│   ├── exception/    # Error propagation (status code + panic)
│   └── io/           # runtime_print, stdin/stdout
├── stdlib/
│   ├── math/  ├── json/  ├── filesystem/  └── time/
├── cli/              # kamipy build|run|clean
├── tests/
│   ├── unit/         # doctest for each module
│   └── integration/  # compile → run → compare stdout
├── docs/
│   ├── vi/  └── en/
└── CMakeLists.txt
```

**Build convention:** two main artifacts:
- `kamipy` — the compiler CLI (links against LLVM).
- `libkamirt.a` / `kamirt.lib` — runtime library with **no** LLVM dependency, statically linked into every produced executable.

---

## 4. Compiler frontend design

### 4.1 Lexer

Hand-written, no generator (flex) — easier to debug, produces nicer diagnostics, and gives full control over indentation.

**Token categories:**

| Category    | Examples                                     |
|-------------|----------------------------------------------|
| NUMBER      | `123`, `3.14` (int64 / double)               |
| STRING      | `"hello"` (escapes: `\n \t \\ \" \xNN`)      |
| IDENTIFIER  | `variable`, `main`                           |
| KEYWORD     | `if else elif while for def return class import True False None and or not pass break continue` |
| OPERATOR    | `+ - * / % = == != < > <= >= ( ) [ ] { } : , .` |
| LAYOUT      | `NEWLINE`, `INDENT`, `DEDENT`, `EOF`         |

**Indentation handling (CPython-style):** keep a stack of indent depths. At the start of each logical line, compare the column count:
- greater than the stack top → push, emit `INDENT`;
- smaller → pop until it matches, emitting one `DEDENT` per pop; no match → `IndentationError`.
- Blank/comment-only lines are skipped. Tabs are either normalized to 8 columns or tab/space mixing is forbidden — v0.1 chooses to **forbid mixing** for safety.

Every token carries a `SourceLocation {file_id, line, col}` for diagnostics.

### 4.2 Parser

**Recursive descent** for statements, **Pratt (precedence climbing)** for expressions.

Precedence table (low → high):

```
or → and → not → == != < > <= >= → + - → * / % → unary - → call () / index [] / attr .
```

Abridged v0.1 grammar (EBNF):

```
program    := (stmt NEWLINE?)* EOF
stmt       := simple_stmt | compound_stmt
simple_stmt:= assign | expr_stmt | return_stmt | pass | break | continue | import_stmt
assign     := IDENT "=" expr
compound   := if_stmt | while_stmt | for_stmt | func_def | class_def
if_stmt    := "if" expr ":" block ("elif" expr ":" block)* ("else" ":" block)?
while_stmt := "while" expr ":" block
func_def   := "def" IDENT "(" params? ")" ":" block
block      := NEWLINE INDENT stmt+ DEDENT
```

**Error recovery:** panic-mode — on error, synchronize to the nearest `NEWLINE`/`DEDENT` so multiple errors can be reported in one run.

### 4.3 AST

- Nodes are allocated on an **Arena** (§6), not per-node `unique_ptr` → O(1) teardown at the end of compilation.
- Two node families: `Expr` (Literal, Name, Binary, Unary, Call, Index, Attr) and `Stmt` (Assign, If, While, For, FuncDef, Return, ExprStmt, Import, ClassDef).
- Visitor strategy: `std::variant` + `std::visit` vs virtual accept — we choose a **tagged enum + switch**: simple, cache-friendly, easy to add passes.
- An `AstDumper` prints the tree as S-expressions for golden-file tests.

### 4.4 Semantic analysis

**Symbol table** as a chain of nested scopes:

```
GlobalScope → FunctionScope → BlockScope (v0.1: function-level scoping, like Python)
```

Responsibilities:
1. **Name resolution** — using a variable before assignment is a compile error (stricter than Python: caught at compile time instead of a runtime `NameError`).
2. **Scope rules** — assignment inside a function creates a local; reads fall back to globals (`global`/`nonlocal` not supported in v0.1).
3. **Type inference** — flow-based over a simple lattice:

```
        Unknown
       /   |    \
     Int Float String Bool None Array Map Func
       \   |    /
        Dynamic   (cannot unify → boxed as Value)
```

Example:
```python
x = 10        # x: Int
y = x + 20    # Int + Int → y: Int  → codegen uses raw i64, no boxing
s = "hi"      # s: String
z = f(x)      # f's return type unknown → z: Dynamic → boxed Value
```

Strategy: **unbox whenever the type is statically provable** (i64/double fast path), otherwise fall back to the dynamic `Value` (§5). This is the key performance lever over an interpreter.

### 4.5 KIR — the mid-level IR

Why not go straight from AST to LLVM IR?
- We need a place for our own constant folding / DCE / inlining **before** boxing is materialized (LLVM struggles to remove boxing once runtime calls are emitted).
- A private IR lets us test the optimizer without LLVM.

Design: **3-address code with a basic-block CFG**; full SSA is not required for v0.1:

```
func @main() {
bb0:
  %0 = const.i64 10
  %1 = const.i64 20
  %2 = add.i64 %0, %1
  call @runtime_print_i64(%2)
  ret
}
```

Ops come in typed variants (`add.i64`, `add.f64`) and a dynamic variant (`add.dyn` → calls `kami_add(Value, Value)`).

### 4.6 Codegen (LLVM 18)

- One `llvm::Module` per program (v0.1 is single-module; imports merge modules — §9).
- Type mapping:
  - `Int` → `i64`, `Float` → `double`, `Bool` → `i1`
  - `Value` → struct `{ i32 tag, [4 x i8] pad, i64 payload }` (16 bytes), passed by pointer under the C ABI.
- Every dynamic operation calls an `extern "C"` runtime function:

```
Python:  print(a + b)
KIR:     %2 = add.i64 %0, %1 ; call @runtime_print_i64(%2)
LLVM:    %2 = add i64 %0, %1
         call void @kami_print_i64(i64 %2)
```

- Entry point: codegen emits a standard C `main()` that calls `kami_rt_init()` → `@kamipy_main()` → `kami_rt_shutdown()`.
- Pass pipeline: default `PassBuilder` O2 (`mem2reg`, `instcombine`, `gvn`, `sccp`, …).

### 4.7 Linker driver

We do not write a linker. The compiler emits `.o` (via `TargetMachine::addPassesToEmitFile`) and then invokes:

- **Linux:** `clang++ main.o -L<rt> -lkamirt -static-libgcc -o app` (or `ld.lld` directly).
- **Windows:** `lld-link main.obj kamirt.lib /out:app.exe /subsystem:console`.

The runtime is always **statically linked** → the executable is self-contained, needing nothing beyond libc/kernel32.

---

## 5. Runtime design

### 5.1 Value — the dynamic type system

```cpp
enum class Type : uint32_t {
    NONE, BOOL, INT, FLOAT, STRING, ARRAY, MAP, OBJECT, FUNCTION
};

struct Value {              // 16 bytes, trivially copyable
    Type type;
    union {
        int64_t  i;         // INT / BOOL
        double   f;         // FLOAT
        void*    ptr;       // STRING/ARRAY/MAP/OBJECT/FUNCTION → heap object
    };
};
```

- Scalar types (`INT/FLOAT/BOOL/NONE`) are stored **inline** — no heap allocation.
- Reference types point to heap objects with a header (§5.2).
- *Alternative considered:* NaN-boxing (8 bytes) — faster but complex and hard to debug; deferred until benchmarks prove the need.

### 5.2 Heap objects

```cpp
struct ObjHeader {
    uint32_t type;      // Type tag
    uint32_t rc;        // reference count
};

struct KamiString { ObjHeader h; uint64_t len; uint64_t hash; char data[]; };   // immutable
struct KamiArray  { ObjHeader h; uint64_t len, cap; Value* items; };
struct KamiMap    { ObjHeader h; /* open addressing, robin hood */ };
```

Strings are immutable → the hash is cached once, they are safe as map keys, and they can be shared without copying.

### 5.3 Memory model & GC

**Compiler side:** Arena allocator (§6) — all AST/KIR nodes live in the arena, freed in O(1).

**Runtime side — decision: Reference Counting** (v0.1):

| Criterion         | RC                          | Mark & Sweep            |
|-------------------|-----------------------------|-------------------------|
| v0.1 complexity   | Low ✅                      | Medium                  |
| Deterministic     | Yes ✅ (immediate free)     | No (pauses)             |
| Cycles            | Leaks ❌ (needs detector)   | Handled ✅              |
| Codegen impact    | Insert incref/decref        | Needs stack maps/root scan |

The v0.1 language has no constructs that easily create cycles (no mutually-capturing closures; cyclic class fields are rare) → RC is the pragmatic choice. A cycle detector (CPython-style trial deletion) is a PHASE 6+ item if needed. Codegen inserts `kami_incref/decref` at assignment/scope-exit points; the optimizer elides redundant incref/decref pairs.

### 5.4 Exceptions & IO

- v0.1: runtime errors (division by zero, index out of range, type errors) → print a message and `exit(1)` via `kami_panic(msg, loc)`.
- v1.x: `try/except` — design choice: propagation via a status flag (no C++ exceptions across the C ABI boundary).
- IO: `kami_print_value(Value)`, plus fast paths `kami_print_i64/f64/str` to avoid boxing when the type is known.

---

## 6. Arena allocator (compiler)

```
Compile start ─▶ arena.alloc(AST nodes) ─▶ arena.alloc(KIR) ─▶ emit ─▶ arena reset (O(1) free)
```

- 64 KiB blocks, bump-pointer, no individual frees; doubling growth.
- RAII: destroying the `Arena` returns all blocks. Leak-free by construction; verified with ASan/Valgrind in CI.

---

## 7. Optimizer (PHASE 9 — designed up front)

Runs on KIR, in order:
1. **Constant folding** — `10 + 20` → `30` (also folds comparisons and constant string concatenation).
2. **Dead code elimination** — side-effect-free instructions with unused results; unreachable blocks.
3. **Inlining** — functions ≤ N ops (N≈16), non-recursive.
4. **Remove unused runtime** — only declare externs for runtime functions actually called, plus `--gc-sections`/`/OPT:REF` at link time → small binaries.

LLVM O2 handles the rest (GVN, LICM, vectorization, …).

---

## 8. Binary generation — full explanation

```
.py ──lexer/parser──▶ AST ──semantic──▶ Typed AST ──lower──▶ KIR
KIR ──codegen──▶ LLVM IR ──llc/TargetMachine──▶ .o (x86-64 object, ELF/COFF)
.o + libkamirt.a ──linker(lld)──▶ executable (ELF on Linux, PE on Windows)
```

- **Machine code** is produced by LLVM's `TargetMachine` inside the `kamipy` process (no external `llc` invocation).
- The executable contains: compiled user code + the runtime + platform-appropriate static/dynamic libc pieces. It contains **no** interpreter, bytecode, or libpython.
- Cross-targeting: LLVM can emit code for any triple (`x86_64-pc-windows-msvc`, `x86_64-unknown-linux-gnu`); linking requires the target platform's toolchain.

---

## 9. Import system (as built)

**Static imports, no dynamic runtime importing.** Everything a program imports is
compiled into the same executable; there is no `sys.modules`, no import hook, no
`.pyc`. The whole subsystem lives in `compiler/src/modules.cpp`.

### 9.1 Where `import X` looks

```
import X
  1. project      X.py or X/__init__.py next to the input file
  2. native       a module the runtime implements (math, os, json, socket, re, ...)
  3. system Python the CPython installation discovered on this machine
  4. bundled pylib runtime/pylib/, shipped next to the kamipy binary
```

A project file always wins, as in CPython. A native module beats CPython's source
on purpose: `import json` should stay a C-ABI primitive call rather than drag in the
whole `json/` package. Everything else is read from the machine's real stdlib, and
the shipped `pylib/` is the *fallback* — used when no Python is installed, or when
CPython's source for that module uses syntax this compiler does not accept yet. A
candidate that fails to **parse** falls through to the next one, which is what makes
that ordering safe; only when no candidate is left does the driver warn and skip.

Modules are spliced into the main program dependency-first, with `if __name__ ==
"__main__":` blocks dropped, so `utils.helper()` resolves to a plain call to
`helper()`. Import cycles are detected and reported.

### 9.2 Discovering the system Python

`find_system_python_stdlib()` returns import roots, best first, and caches the
result. It never runs a subprocess — it only reads environment variables, the
Windows registry and the filesystem.

| Order | Source |
|---|---|
| 1 | `$KAMIPY_PYTHON_STDLIB` — explicit override, `:`/`;` separated |
| 2 | `$PYTHONHOME` → `<prefix>/Lib` (Windows) or `<prefix>/lib/python3.*` |
| 3 | `$PYTHONPATH` entries, used as-is |
| 4 | `python3` / `python.exe` on `$PATH` → its prefix (catches virtualenv, pyenv, conda) |
| 5 | Windows: registry `HKCU`/`HKLM\Software\Python\PythonCore` (and `Wow6432Node`) → `InstallPath`; `%LOCALAPPDATA%\Programs\Python\Python3*\Lib`; `C:\Python3*\Lib`; `C:\Program Files[ (x86)]\Python3*\Lib` |
| 5 | Unix: `/usr/lib/python3.*`, `/usr/local/lib/python3.*`, `/usr/lib64/...`, `/opt/homebrew/...`, `~/.pyenv/versions/*/lib/python3.*`, macOS framework builds |

A directory is only accepted if it contains a stdlib fingerprint (`os.py`,
`json/__init__.py` or `types.py`), so a stray backup folder cannot poison the
search path. Within one priority level the newest Python version sorts first.

`kamipy paths` prints the whole list; `kamipy build -v` reports every resolution;
`--no-system-stdlib` (or `KAMIPY_NO_SYSTEM_STDLIB=1`) turns discovery off.

### 9.3 C extensions and the C ABI

CPython's standard library is only half Python — the interesting half is C
extension modules. `compiler/src/cext.cpp` maps their members onto one of two
things:

* a **runtime primitive** (`_json.loads` → `KB_JSON_LOADS`), or
* a **direct C call** emitted into the IR (`_math.sqrt` → `call double @sqrt(double)`).

`runtime/src/syscalls.cpp` holds the primitives that have no natural Python
expression: descriptor I/O (`open/read/write/close/lseek`), `gethostname`, and
`struct.pack`/`unpack`/`calcsize`.

`ctypes` and `cffi` are understood at compile time, not runtime:

```python
lib = ctypes.CDLL("libm.so.6")     # → link flag -lm, derived from the name
lib.sqrt.restype = ctypes.c_double # → return type
lib.sqrt.argtypes = [ctypes.c_double]
lib.sqrt(2.25)                     # → call double @sqrt(double 2.25)
```

Signatures are carried as a compact string: the first character is the return
type, the rest are parameters, and **each C width has its own code** — `'l'` is a
32-bit C `int`, `'i'` is a 64-bit `long`/`size_t`/pointer, `'f'` is `float`, `'d'`
is `double`, `'s'` is `const char*`. Handing an i64 to a function whose parameter
is a C `int` is undefined behaviour, not a nicety, so codegen emits the
`trunc`/`sext`/`fptrunc`/`fpext` conversions around the call. Arguments are
unboxed through `kami_c_arg_i64/f64/cstr`, so a type mismatch is a catchable
Python-level error rather than a corrupted stack. The driver collects the
libraries these bindings need and appends `-l<name>` to the clang command.

---

## 10. CLI (as built)

```
kamipy build <file.py> [-o app] [-O0|-O1|-O2] [--emit-llvm] [--emit-ast]
                       [--no-system-stdlib] [-v|--verbose]
kamipy run   <file.py>      # build into .kamipy-cache/, then exec
kamipy paths                # every directory `import` searches, in order
kamipy clean                # remove .kamipy-cache/
```

`--emit-llvm`/`--emit-ast` support debugging and golden-file tests. `paths` and
`-v` exist to make import resolution diagnosable without guesswork: `paths`
answers "where would it look?", `-v` answers "where did it actually find it, and
what did it link against?".

---

## 11. Testing strategy

| Layer        | How it is tested                                                  |
|--------------|-------------------------------------------------------------------|
| Lexer        | Unit: source → expected token list (incl. INDENT/DEDENT, errors)  |
| Parser       | Unit + golden: source → S-expression AST dump                     |
| Semantic     | Unit: invalid programs must report the right error on the right line |
| KIR/Optimizer| Golden: KIR before/after each pass                                |
| Codegen      | `--emit-llvm` + FileCheck-style assertions                        |
| Integration  | **compile → run → compare stdout** (a table of `.py` + `.expected`)|
| Memory       | ASan + LeakSanitizer across the whole test suite                  |
| Benchmark    | hello-world size/RAM/startup vs CPython, Go, C++ (PHASE 14)       |

Framework: **doctest** (header-only, lightweight) for unit tests; CMake/CTest scripts for integration.

---

## 12. Language v0.1 — committed scope

Supported: variables, `int/float/str/bool/None`, arithmetic + comparison + logic, `if/elif/else`, `while`, `for i in range(...)`, `def`/`return`/calls/recursion, `print()`, `#` comments.

Not yet (roadmap): full classes, closures, generators, `try/except`, slicing, f-strings, varargs, user modules (PHASE 10).

---

## 13. Risks & mitigations

| Risk                                       | Mitigation                                                |
|--------------------------------------------|------------------------------------------------------------|
| LLVM API churn between versions            | Pin LLVM 18; wrap behind a `codegen/LLVMBackend` interface |
| Boxing slows down dynamic code             | Type inference unboxes fast paths; benchmark every phase   |
| RC leaks from cycles                       | v0.1 restricts semantics; cycle detector when needed       |
| Windows linking complexity (CRT, subsystem)| Use lld-link + Windows CI early, from PHASE 8              |
| Indentation lexer edge cases               | Large test table in PHASE 2; forbid tab/space mixing       |

---

## 14. Roadmap & per-phase exit criteria

| Phase | Content              | Exit criteria (must pass before moving on)                    |
|-------|----------------------|---------------------------------------------------------------|
| 0     | Architecture (this doc) | VI/EN docs + changelog + PR                               |
| 1     | CMake foundation     | `cmake -B build && cmake --build build` produces a working `kamipy` |
| 2     | Lexer                | Token-stream unit tests pass 100%                             |
| 3     | Parser               | AST-dump golden tests pass                                    |
| 4     | Semantic             | Name/scope/type tests pass, correct error lines               |
| 5     | Value system         | Value/Type unit tests pass                                    |
| 6     | Memory/GC            | ASan clean, no leaks                                          |
| 7     | Runtime lib          | `kami_print` works for all types                              |
| 8     | LLVM backend         | `print(10)` compiles → runs → prints `10`                     |
| 9     | Optimizer            | Golden KIR tests; binary smaller than baseline                |
| 10    | Import               | `import math` compiles, links, runs correctly                 |
| 11    | Stdlib               | Per-module stdlib tests                                       |
| 12    | CLI                  | `build/run/clean` work on both platforms                      |
| 13    | Integration tests    | Full test table passes                                        |
| 14    | Benchmark            | hello < 5 MB, RAM < 20 MB, comparison report                  |
| 15    | Binary protection    | strip + optimize, documented limitations                      |

**Final milestone:** `kamipy build main.py` → `main.exe` runs and prints `30`, with no Python installed.

---

## 15. v0.1 implementation notes (as-built)

The v0.1 implementation (see `CHANGELOG.md`, v0.1.0) intentionally deviates from the design above in a few places:

1. **AST → LLVM IR directly; no separate KIR yet.** Constant folding runs in the semantic pass; code after `return/break/continue` is dropped at codegen time. KIR remains the extension point for deeper optimizations (cross-function unboxing, inlining).
2. **The GC is mark & sweep (not reference counting).** Rationale: it composes more safely and simply with threading — codegen maintains the invariant that *object pointers never live in registers across a call; every value lives in a frame slot registered as a GC root; every mutation goes through a runtime call that holds the global lock*. This makes GC + threads correct by construction (validated with ThreadSanitizer/AddressSanitizer).
3. **Threading uses a GIL model** like CPython: every runtime call holds one global lock; `sleep/join` release it while blocked. Key optimization: while a program has not spawned any thread, the lock is **skipped entirely** (the flag flips permanently at the first spawn — a race-free transition because exactly one thread exists at that moment and it holds the real lock). Result: ~2× faster single-threaded code, still-correct multithreading.
4. **Codegen emits textual LLVM IR (`.ll`) and invokes `clang++`** as the backend + linker driver (via `fork/execvp`, never through a shell). Simple, robust across LLVM versions, and still a genuine LLVM pipeline. In-process `TargetMachine` is a future optimization.
5. **The AST uses `unique_ptr` (RAII)** instead of an arena — a compiler-performance detail with no semantic impact.
