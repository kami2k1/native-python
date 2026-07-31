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

**Windows (Visual Studio Community):** xem hướng dẫn chi tiết / see the full guide — [docs/vi/BUILD_WINDOWS.md](docs/vi/BUILD_WINDOWS.md) · [docs/en/BUILD_WINDOWS.md](docs/en/BUILD_WINDOWS.md)

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
| prime sieve 100k | **19 ms / 5.3 MB** | — | 55 ms / 8.5 MB | — |
| quicksort 20k | **32 ms / 4.1 MB** | — | 38 ms / 8.5 MB | — |
| sudoku solver | **18 ms / 3.6 MB** | — | 29 ms / 7.7 MB | — |
| GC stress (600k allocs) | 96 ms, 4.7 MB peak | — | — | bounded ✅ |

## Real-world validation / Kiểm chứng dự án thực tế

8 real programs compiled to native and tested in CI — 7 of them are plain-Python-compatible and their outputs are **byte-identical to CPython** (`tests/integration/proj_*.py`):

| Project | Exercises |
|---|---|
| `proj_calculator` | expression interpreter: tokenizer + recursive-descent parser + evaluator |
| `proj_sudoku` | backtracking search, deep recursion, 2D lists |
| `proj_text_analytics` | manual string parsing, frequency dicts, deterministic top-k |
| `proj_linear_regression` | gradient descent (ML), float math |
| `proj_primes` | Sieve of Eratosthenes to 100k (100k-element list, tight loops) |
| `proj_maze_bfs` | BFS shortest path: queue, visited dict |
| `proj_quicksort` | in-place quicksort of 20k ints + LCG + checksum |
| `proj_bank_threads` | 6 concurrent OS threads updating a shared list |

## Status

| Component | Status |
|---|---|
| Lexer / Parser / Sema / Codegen / Linker driver | ✅ implemented + tested |
| Runtime (Value, GC, list/dict/str, threading GIL) | ✅ implemented + sanitizer-clean |
| CLI (`build/run/clean`) | ✅ |
| Tests | ✅ 2 unit suites + 23 integration programs (threading, GC stress, neural-net XOR, 8 real-world projects) |
| CI | ✅ GitHub Actions: ubuntu-24.04 + windows-latest (MSVC + LLVM) |
| Platforms | ✅ Linux x64 (tested locally + CI) · Windows x64 (MSVC-ready, tested via CI) |
