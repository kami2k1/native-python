# KamiPython (native-python)

**KamiPython** là một compiler AOT viết bằng **C++20**, biến ngôn ngữ Python-like thành **native executable** qua **LLVM** — không cần Python, CPython runtime, PyInstaller hay Nuitka.

**KamiPython** is an AOT compiler written in **C++20** that turns a Python-like language into a **native executable** via **LLVM** — no Python, CPython runtime, PyInstaller, or Nuitka required.

```python
def main():
    a = 10
    b = 20
    print(a + b)

main()
```

```bash
$ kamipy build main.py -o app
built app
$ ./app
30
$ ldd app          # no libpython — only libc/libm/libstdc++
```

## Build / Cài đặt

Requirements / Yêu cầu: CMake ≥ 3.20, a C++20 compiler, and `clang++` (LLVM ≥ 15) on `PATH` (used as the LLVM backend + linker driver).

```bash
cmake -B build
cmake --build build -j
build/kamipy build examples/main.py -o app   # or: tests/integration/milestone.py
./app
```

Run the test suite / Chạy test:

```bash
ctest --test-dir build --output-on-failure
```

## CLI

```
kamipy build <file.py> [-o out] [-O0|-O1|-O2] [--emit-llvm] [--emit-ast]
kamipy run   <file.py>      # build vào .kamipy-cache rồi chạy
kamipy clean                # xóa .kamipy-cache
```

## Language v0.1 / Ngôn ngữ v0.1

| Supported / Hỗ trợ | |
|---|---|
| Types | `int` (64-bit), `float`, `str`, `bool`, `None`, `list`, `dict`, function values |
| Operators | `+ - * / // % == != < > <= >= and or not in "not in"` , `+= -= *= /=` |
| Control flow | `if/elif/else`, `while`, `for x in range(...)`, `for x in <list/str>`, `break`, `continue` |
| Functions | `def`, `return`, recursion, functions as values (`f(g, x)`) |
| Builtins | `print len str int float abs min max ord chr type range` |
| Methods | `list.append/pop`, `str.upper/lower`, `dict.get/keys/values` |
| Modules | `math` (sqrt sin cos tan exp log pow floor ceil fabs pi e), `time` (time sleep), `random` (random randint seed), `threading` (spawn join) |
| Threading | Real OS threads (`std::thread`) with a GIL-style runtime lock; lock elided until the first `threading.spawn` |
| Memory | Mark & sweep GC, root frames registered by generated code; ASan/LSan/TSan clean |

Not yet / Chưa hỗ trợ: classes, closures/nested functions, `try/except`, slicing, f-strings, generators, user-defined module imports.

## How it works / Cách hoạt động

```
main.py → Lexer (INDENT/DEDENT) → Parser (recursive descent + Pratt) → AST
       → Semantic analysis (scopes, name resolution, arity, constant folding)
       → LLVM IR (.ll) → clang/LLVM -O2 → object code → link with libkamirt.a
       → native executable (ELF/PE)
```

Chi tiết: [docs/vi/ARCHITECTURE.md](docs/vi/ARCHITECTURE.md) · Details: [docs/en/ARCHITECTURE.md](docs/en/ARCHITECTURE.md)

Báo cáo build/test/benchmark từng phase: [CHANGELOG.md](CHANGELOG.md)

## Benchmarks (Linux x64, LLVM 18)

| Metric | KamiPython | C++ (-O2) | CPython 3.11 | Target |
|---|---|---|---|---|
| hello binary size | **70 KB** (60 KB stripped) | 16 KB | n/a (needs ~30 MB install) | < 5 MB ✅ |
| hello startup | **1.9 ms** | 1.8 ms | 12.6 ms | — |
| hello max RSS | **3.6 MB** | 3.6 MB | 7.5 MB | < 20 MB ✅ |
| fib(27) | **42 ms** | — | 44 ms | — |
| GC stress (600k allocs) | 96 ms, 4.7 MB peak | — | — | bounded ✅ |

## Status

| Component | Status |
|---|---|
| Lexer / Parser / Sema / Codegen / Linker driver | ✅ implemented + tested |
| Runtime (Value, GC, list/dict/str, threading GIL) | ✅ implemented + sanitizer-clean |
| CLI (`build/run/clean`) | ✅ |
| Tests | ✅ 2 unit suites + 15 integration programs (incl. threading, GC stress, neural-net XOR) |
| Platforms | ✅ Linux x64 (tested) · ⚠️ Windows x64 (code paths present, not yet CI-tested) |
