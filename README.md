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

| Supported / Hỗ trợ (v0.2) | |
|---|---|
| Types | `int` (64-bit), `float`, `str`, `bool`, `None`, `list`, `dict`, tuples (≈list), function values, **classes/objects** |
| Exceptions | **`try/except/else/finally`, `raise`, `assert`** — mọi runtime error bắt được |
| Classes | `class C(Base):`, `__init__`, methods, `self.x`, class attrs, single inheritance, `isinstance`, **`__str__`/`__repr__`/`__enter__`/`__exit__`** |
| Strings | **f-strings** (`{x:.2f}`, `{x = }`), **implicit concatenation** (`f"{a}" "tail"`), **`%` formatting** (`"%s=%d" % (a, b)`), triple-quoted docstrings, raw strings, slicing `s[::-1]`, 25+ methods |
| Operators | số học + so sánh chuỗi hoá (`0 <= i < n`) + logic + **bitwise `& \| ^ ~ << >>`** + `**` + augmented |
| Control flow | `if/elif/else`, `while`, `for` (đa target unpack), `break/continue`, ternary, inline body |
| Functions | `def` với **kwargs + default params + `*args`**, recursion, **lambda, nested functions/closures, decorators**, functions as values, `global` |
| Sugar | **list/dict/set + nested comprehensions**, genexp, lambda, decorators, tuple/starred unpacking (`(a,b)=`, `[*a,b]`), `x = y = 0`, slices, PEP 695 `[T]` syntax, annotations (ignored) |
| Builtins | `print(sep=,end=) len str int float bool abs min max sum sorted reversed enumerate zip round ord chr type range all any bin hex oct list dict tuple isinstance format divmod input pow exit` |
| I/O | **`open()`** file read/write/iterate, **`with`** context managers, **`socket`** (TCP server+client, **`wrap_tls()`** → OpenSSL/SChannel, `settimeout`), **`requests`** (pure-Python HTTP/1.1: params/data/json/headers/auth, redirects, chunked, `Response.json()/raise_for_status()`, `Session`), **`json`**, **`re`**, **`logging`** — tất cả là Python trong `runtime/pylib/` |
| Collections | **insertion-ordered dict** (CPython-parity), **`set`** (`{1,2}`, `&\|^-`), **`del`**, comprehensions, first-class builtins |
| Modules | C-ABI bindings: `math time random threading sys os os.path socket string doctest`; **Python stdlib compiled from source** (`runtime/pylib/`): `re json logging requests itertools functools`; **auto-discovered system Python stdlib** (compile source thật của CPython trên máy); no-op `typing`/`__future__` |
| System Python | **`find_system_python_stdlib()`**: dò `KAMIPY_PYTHON_STDLIB` → `PYTHONHOME` → `PYTHONPATH` → interpreter trên `PATH` → Registry Windows (`HKCU`/`HKLM\Software\Python\PythonCore`) + `%LOCALAPPDATA%\Programs\Python\Python3*\Lib` + `C:\Python3*\Lib` + `C:\Program Files\Python3*\Lib` / `/usr/lib/python3.*` + `/usr/local/lib/python3.*` + pyenv + Homebrew. Thứ tự resolve: **project → native → system Python → pylib fallback**. `kamipy paths` để xem, `-v` để trace, `--no-system-stdlib` để tắt |
| C extensions | `_math` `_os`/`posix`/`nt` `_socket` `_struct` `_json` `_time` `_random` → **C-ABI primitive** (`runtime/src/syscalls.cpp`) hoặc **lời gọi C trực tiếp** trong LLVM IR (`call double @sqrt(double)`); raw fd I/O `open/read/write/close/lseek`; `struct.pack/unpack/calcsize` |
| ctypes / cffi | `ctypes.CDLL("libm.so.6")` + `restype`/`argtypes`, `cffi.FFI()` + `cdef()` + `dlopen()` → **native call** + **tự sinh cờ link** (`-lm`, `-lws2_32`, …); độ rộng kiểu C đúng chuẩn (`int` 32-bit, `long` theo LP64/LLP64, `float` ≠ `double`) |
| Local modules | **`import mymodule` bundles `mymodule.py`** (cạnh file input, đệ quy theo dependency, `pkg.mod` → `pkg/mod.py`, relative `from .mod import x`); `__main__` guard của module bundle không chạy; `try: import cv2 / except ImportError:` works |
| Functional | **`map` `filter`** + first-class functions/lambdas/closures passed to `sorted(key=)` etc. |
| Threading | Real OS threads with GIL-style lock (elided when single-threaded); **`threading.Lock`/`RLock`** = native mutexes usable with `with`; socket accept/recv and lock waits release the GIL |
| Memory | Mark & sweep GC (exception-unwind + file/socket safe); ASan/LSan/TSan clean |

**Real-world compatibility:** đo trên **2.182 file Python thật** từ GitHub (TheAlgorithms, geekcomputers): **1059 build (49%), 784 chạy** — so với 269/261 ở v0.1.1 (xem `tools/corpus_survey.sh` + CHANGELOG).

**Native networking demo:** một HTTP server viết bằng KamiPython phục vụ chính client `requests` của KamiPython, parse JSON — tất cả là native binary. TCP socket server+client qua thread. Xem `docs`/CHANGELOG.

Not yet / Chưa hỗ trợ (lỗi thông báo rõ): generators/`yield`, `**kwargs`, walrus `:=`, `@staticmethod/@property`, `nonlocal`, exception subclasses (`class E(Exception)`), và third-party package cần C extension của CPython (`numpy`, `pyautogui`, `PIL`, `flask`).

### "Translate, don't rewrite" / "Chỉ dịch — không tự viết"

The compiler's job is to *translate* Python, not to reimplement the standard library in C++.
So anything with algorithmic content lives in `runtime/pylib/` as **real Python**, and the same
`lex → parse → sema → codegen` pipeline turns it into machine code:

```
runtime/pylib/re.py         a backtracking regex VM   (replaced 411 lines of C++)
runtime/pylib/json.py       encoder + decoder         (replaced ~240 lines of C++)
runtime/pylib/logging.py    handlers + formatting     (replaced ~60 lines of C++)
runtime/pylib/requests.py   HTTP/1.1 over a socket    (replaced 562 lines of C++)
```

The C++ runtime keeps only what Python cannot express about itself: the value model, the GC,
and C-ABI bindings down to the OS (`runtime/src/oslayer.cpp` for files/processes,
`runtime/src/netsock.cpp` for sockets and the platform TLS stack).

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
| Tests | ✅ 3 unit suites + 55 integration programs (threading + locks, GC stress, neural-net XOR, real-world projects, exceptions/classes, files/sets, stdlib, sockets + TLS, closures/lambda/decorators, regex, JSON, logging, requests, comprehensions, dict-order, unpacking, C-ABI/ctypes/cffi, system-Python imports) |
| CI | ✅ GitHub Actions: ubuntu-24.04 + windows-latest (MSVC + LLVM) |
| Platforms | ✅ Linux x64 (tested locally + CI) · Windows x64 (MSVC-ready, tested via CI) |
