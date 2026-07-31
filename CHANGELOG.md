# Changelog — KamiPython

Tất cả thay đổi đáng chú ý của project được ghi tại đây. / All notable changes to this project are documented here.

---

## [v0.2.0] — 2026-08-01 — Real-world compatibility: exceptions, classes, f-strings + 100s dự án GitHub thật

Xuất phát từ bug report thực tế (`try:` → parse error), toàn bộ frontend + runtime được nâng cấp
theo quy trình **đo trên corpus thật → fix theo tần suất lỗi → đo lại**, dùng 2.182 file Python
thật từ GitHub (TheAlgorithms/Python, geekcomputers/Python).

### Kết quả corpus / Corpus results
| Vòng | Build được / Built | Chạy được / Ran |
|---|---|---|
| Baseline v0.1.1 | 269/2182 (12%) | 261 |
| **v0.2.0** | **914/2182 (42%)** | **704** |

(Phần lớn số còn lại cần: third-party modules như numpy/cv2/re/os, `with`, decorators, lambda,
generators, nested functions, set literals, relative imports — xem Roadmap.)

### Added — ngôn ngữ / Language
- **Exceptions**: `try/except [E [as e]]/else/finally`, `raise Error("msg")`, bare `raise`,
  `assert cond, msg`. Runtime error nào cũng bắt được (ZeroDivision, IndexError, KeyError, ...).
  Cơ chế: try-body được outline thành hàm riêng, bảo vệ bằng C++ unwinding (`kami_try`),
  GC frame-stack được tự sửa khi unwind; `return/break/continue` xuyên qua try hoạt động đúng
  qua mã code (0/1/2/3). ASan clean.
- **Classes**: `class C(Base):`, `__init__`, methods, instance fields (`self.x`), class
  attributes, single inheritance, unbound call `Base.__init__(self, ...)`,
  `isinstance(x, C)` / `isinstance(x, (int, str))`.
- **f-strings**: `f"{expr}"`, format spec `{x:.2f}` `{n:04d}` `{s:>6}` `{p:.0%}`, debug `{x = }`,
  raw strings `r"..."`, triple-quoted strings + docstrings, implicit string concat.
- **Slices**: `a[1:3]`, `a[:n]`, `a[::-1]` cho list + str.
- **Comprehensions**: `[f(x) for x in xs if cond]`, generator-expression trong call
  (`sum(x for x in xs)`), unpack targets (`for k, v in d.items()`).
- **Tuple assignment**: `a, b = 1, 2`, swap `a, b = b, a`, unpack từ hàm, `x = y = 0`,
  `return a, b`; tuple literals ≈ list (hashable khi dùng làm dict key: `d[i, j]`).
- **Bitwise**: `& | ^ ~ << >>` + augmented (`|=`, `<<=`, ...); `**`/`**=`; `//=`, `%=`.
- Chained comparisons (`0 <= i < n`), ternary (`a if c else b`), `is`/`is not`,
  `not in`, `while True`, inline body (`if x: return y`), dấu `;`, `\` nối dòng,
  hex/octal/binary/underscore số (`0xFF`, `1_000_000`), số mũ, `.5`, `1.`.
- **Kwargs + defaults**: `def f(a, b=1)`, gọi `f(a, b=2)` cho hàm/constructor/unbound method
  trong file; runtime hỗ trợ gọi động với defaults (calling convention mang `nargs`).
- `global`, type annotations (params, return `->`, biến, `self.x: T = v`) — parse và bỏ qua.
- Imports: `import a, b`, `import x as y`, `from math import sqrt, pi`,
  `import` trong `try/except ImportError` với module không có sẵn → bỏ qua (đúng pattern
  bug report), `__name__ == "__main__"`, no-op modules (`typing`, `__future__`, `abc`,
  `dataclasses`), stub `doctest.testmod()`, `sys.argv`/`sys.exit`, `string.ascii_lowercase`...

### Added — builtins & methods
`sum sorted reversed enumerate zip bool round input pow all any bin hex oct list dict tuple
isinstance format divmod exit quit`; str: `split join strip lstrip rstrip replace startswith
endswith find count isdigit isalpha isupper islower isspace capitalize title zfill`;
list: `insert remove index extend sort reverse count copy pop(i)`; dict: `items`;
`min(xs)`/`max(xs)` dạng iterable; `print(sep=, end=)`.

### Fixed
- **Bug codegen nghiêm trọng**: slot kết quả cấp phát sau khi giải phóng temp args → out
  alias arg slot; constructor ghi instance đè lên argument. (Phát hiện nhờ test class.)
- Unknown escape trong string giờ giữ nguyên như Python (`"\d"`), thay vì lỗi.
- Trailing comma trong tham số `def f(a, b,)`, positional-only marker `/`.

### Verification / Kiểm chứng
- Integration **28/28 PASS** (23 cũ + 5 mới: `feat_exceptions`, `feat_classes`,
  `feat_modern_syntax`, `feat_builtins`, `feat_guarded_imports` — 4/5 cross-check CPython
  từng byte; riêng exceptions dùng expected riêng vì format message khác chủ ý).
- ASan CLEAN trên exception-unwinding + classes; unit + integration cũ không hỏng.
- Tool khảo sát corpus: `tools/corpus_survey.sh` (tái lập được kết quả).

### Known deviations / Khác biệt chủ ý so với CPython
- Tuple hiển thị như list (`(1, 2)` → `[1, 2]`); tuple là list (mutable).
- `str(e)` của exception kèm prefix loại lỗi (`"ValueError: msg"`).
- `except SomeError` bắt mọi lỗi (chưa phân loại theo type); nhiều mệnh đề except:
  mệnh đề đầu bắt tất.
- `round()` half-away-from-zero (không banker's); enumerate/zip/range eager (trả list).
- Comprehension variable leak ra scope (như Python 2); `a[i] += v` đánh giá `a`, `i` 2 lần.

### Roadmap (chưa hỗ trợ, lỗi rõ ràng)
`with`, decorators, lambda, generators/`yield`, nested functions/closures, set literals,
`del`, walrus `:=`, relative imports, `*args/**kwargs`, f-string spec lồng nhau,
match, PEP 695 generics, third-party modules (numpy, cv2, requests...).

---

## [v0.1.1] — 2026-08-01 — Windows build support + real-world validation / Hỗ trợ build Windows + kiểm chứng dự án thực tế

### Added / Thêm mới
- **Tài liệu build Windows (Visual Studio Community)** song ngữ: [`docs/vi/BUILD_WINDOWS.md`](docs/vi/BUILD_WINDOWS.md) · [`docs/en/BUILD_WINDOWS.md`](docs/en/BUILD_WINDOWS.md) — cài VS Community (workload *Desktop development with C++*), LLVM/Clang for Windows, build bằng dòng lệnh hoặc IDE, sử dụng, biến môi trường, troubleshooting.
- **GitHub Actions CI** (`.github/workflows/ci.yml`): build + full test trên **ubuntu-24.04** và **windows-latest** (MSVC + LLVM) cho mỗi push/PR.
- **8 dự án thực tế** compile thành native + chạy trong CI (`tests/integration/proj_*.py`):
  | Dự án | Nội dung |
  |---|---|
  | `proj_calculator` | interpreter biểu thức: tokenizer + recursive-descent parser + evaluator |
  | `proj_sudoku` | giải sudoku backtracking (đệ quy sâu, list 2 chiều) — ra đúng nghiệm chuẩn |
  | `proj_text_analytics` | tách từ thủ công, dict tần suất, top-k deterministic |
  | `proj_linear_regression` | machine learning: gradient descent fit y=2x+1 (hội tụ, MSE < 1e-4) |
  | `proj_primes` | sàng Eratosthenes tới 100.000 (list 100k phần tử) — 9592 số nguyên tố |
  | `proj_maze_bfs` | BFS đường đi ngắn nhất trong mê cung (queue + visited dict) |
  | `proj_quicksort` | quicksort in-place 20.000 số + LCG + checksum |
  | `proj_bank_threads` | 6 OS threads cập nhật list chia sẻ đồng thời |

### Fixed / Sửa (Windows-portability)
- `CMakeLists.txt`: tách flags MSVC (`/O2 /utf-8 /EHsc`) khỏi GCC/Clang (`-O2 -fno-exceptions`) — trước đây build MSVC sẽ fail.
- `driver.cpp`: quote arguments chứa khoảng trắng khi `_spawnvp` (đường dẫn `C:\Program Files\LLVM\...`).
- `gc.cpp`: `_setmode(stdout, _O_BINARY)` trên Windows để output `\n` đồng nhất giữa các platform.
- `.gitattributes`: ép LF cho `*.py`, `*.expected`, `*.sh` — tránh CRLF phá integration test khi clone trên Windows.
- Thêm `<cctype>/<cstdlib>` còn thiếu trong `lexer.cpp`, `ops.cpp` (portability MSVC).

### Build instructions / Hướng dẫn build
- **Linux**: như v0.1.0 (`cmake -B build && cmake --build build -j && ctest --test-dir build`).
- **Windows (VS Community)**: xem `docs/vi/BUILD_WINDOWS.md` — tóm tắt: cài VS Community + workload C++, `winget install LLVM.LLVM`, mở *x64 Native Tools Command Prompt*, `cmake -B build && cmake --build build --config Release`, `ctest --test-dir build -C Release`.

### Test report / Báo cáo test (Linux x64, clang/LLVM 18.1.3)
| Kiểm chứng / Check | Kết quả / Result |
|---|---|
| Unit (lexer, parser) | ✅ PASS |
| Integration **23/23** (15 cũ + 8 dự án thực tế mới) | ✅ PASS |
| **Cross-check CPython**: 7 dự án Python-compatible, output so sánh từng byte với `python3` chạy cùng source | ✅ GIỐNG HỆT 7/7 |
| ASan + LSan trên `proj_bank_threads`, `proj_sudoku` | ✅ CLEAN |
| TSan trên `proj_bank_threads` (6 threads + GC) | ✅ CLEAN |
| Determinism: `proj_bank_threads` chạy 20 lần | ✅ 20/20 output giống hệt |
| Clean rebuild từ đầu | ✅ 0 error, 0 warning |
| Windows CI (MSVC + LLVM, windows-latest) | 🔄 chạy tự động trên GitHub Actions sau khi push |

### Benchmarks — dự án thực tế (Linux x64)
| Program | KamiPython | CPython 3.11 |
|---|---|---|
| prime sieve 100k | **19 ms / 5.3 MB** | 55 ms / 8.5 MB |
| quicksort 20k | **32 ms / 4.1 MB** | 38 ms / 8.5 MB |
| sudoku solver | **18 ms / 3.6 MB** | 29 ms / 7.7 MB |
| calculator interpreter | **2.1 ms / 3.8 MB** | 12.1 ms / 7.9 MB |

---

## [v0.1.0] — 2026-08-01 — Full compiler implementation / Compiler hoàn chỉnh

Compiler hoạt động end-to-end: `.py → native executable`, không cần Python. / Working end-to-end compiler: `.py → native executable`, no Python required.

### Added / Thêm mới
- **Compiler** (`compiler/src/`, C++20):
  - `lexer.cpp` — tokenizer với INDENT/DEDENT kiểu CPython, string escapes, số int/float/exponent, cấm trộn tab trong indentation.
  - `parser.cpp` — recursive descent + precedence climbing; desugar `elif`, `+= -= *= /=`, `not in`; AST dump S-expression cho golden test.
  - `sema.cpp` — symbol table (global/function scope), name resolution lúc compile (biến chưa định nghĩa → lỗi compile), arity check, module binding (`math/time/random/threading`), **constant folding** (int/float/bool/string).
  - `codegen.cpp` — phát **LLVM IR** (textual, opaque pointers, LLVM ≥ 15); frame slots đăng ký làm GC roots; đặc biệt hoá `for i in range(...)` thành counting loop (không materialize list); short-circuit `and/or`; dead code sau `return/break/continue` bị loại bỏ.
  - `driver.cpp` — pipeline + gọi `clang++` (LLVM backend + linker) qua `fork/execvp` (không qua shell → không command injection); tự tìm `libkamirt.a` cạnh binary.
- **Runtime** (`runtime/`, C++20, static lib `libkamirt.a`, không phụ thuộc LLVM):
  - `Value` 16-byte tagged union (NONE/BOOL/INT/FLOAT/STR/LIST/MAP/FUNC/THREAD).
  - **GC mark & sweep** không di chuyển, kích hoạt theo ngưỡng cấp phát; roots = globals + frame stacks của mọi thread + pins.
  - **Threading GIL-style**: mọi runtime call giữ global lock; lock **được bỏ qua hoàn toàn khi chương trình single-threaded** (bật vĩnh viễn tại lần `threading.spawn` đầu tiên — chuyển trạng thái race-free vì diễn ra khi chỉ có 1 thread và đang giữ lock thật). `time.sleep`/`threading.join` nhả lock khi block.
  - list (dynamic array), dict (open addressing, Python-style hash/eq), string immutable (FNV-1a hash cache), số học semantics giống Python (`//` floor, `%` sign, `/` → float).
  - Builtins: `print len str int float abs min max ord chr type range` + `math.* time.* random.* threading.*` + methods `append pop upper lower get keys values`.
  - Lỗi runtime → thông báo rõ ràng + exit code 1 (`division by zero`, `list index out of range`, `KeyError`, type errors...).
- **CLI** (`cli/main.cpp`): `kamipy build|run|clean`, `-o`, `-O0/-O1/-O2`, `--emit-llvm`, `--emit-ast`.
- **Tests**: 2 unit suites (lexer 10 nhóm assert, parser 19 golden + 7 error case) + **15 integration programs** (compile → run → diff stdout), đăng ký qua CTest.
- `examples/main.py` — chương trình milestone.

### Build instructions / Hướng dẫn build
```bash
# Requirements: CMake ≥ 3.20, C++20 compiler, clang++ (LLVM ≥ 15) on PATH
cmake -B build
cmake --build build -j          # → build/kamipy + build/libkamirt.a
ctest --test-dir build --output-on-failure

# Use / Sử dụng
build/kamipy build examples/main.py -o app
./app                            # → 30
```
Đã kiểm chứng build sạch từ đầu (xóa `build/` build lại): **0 error, 0 warning**. / Verified from a pristine tree: **0 errors, 0 warnings**.

### Test report / Báo cáo test (Linux x64, clang/LLVM 18.1.3, cmake 3.28.3)
| Suite | Kết quả / Result |
|---|---|
| `unit.lexer` | ✅ PASS (tokens, keywords, 2-char ops, INDENT/DEDENT, blank/comment lines, bracket continuation, 3 error cases) |
| `unit.parser` | ✅ PASS (precedence, desugaring, functions, loops, collections, 7 error cases) |
| `integration` — 15 programs | ✅ 15/15 PASS |

Integration programs / Chương trình integration:
`milestone` (đúng đề bài → in `30`), `hello`, `arith`, `control_flow`, `functions` (đệ quy fib(20), hàm truyền như giá trị), `strings`, `lists`, `dicts`, `bool_logic` (short-circuit có side-effect), `for_loops`, `math_module`, `threading_test` (4 OS threads + shared list + join), `gc_stress` (600k+ object churn), `ai_xor` (**neural network 2-3-1 viết thuần KamiPython, backprop 6000 epochs, học XOR thành công: 0/1/1/0**), `complex_app` (insertion sort + word-freq dict + higher-order map + stddev + 4-thread Leibniz π).

### Robustness validation / Kiểm chứng độ bền (yêu cầu: threading, chương trình phức tạp, runtime)
| Kiểm chứng / Check | Kết quả / Result |
|---|---|
| Native binary, không cần Python | ✅ `ldd` chỉ thấy libc/libm/libstdc++/libgcc — không libpython |
| Thread-race hunt: 6 threads cấp phát rác đồng thời (GC chạy giữa chừng), lặp 30 lần | ✅ 30/30 output giống hệt nhau |
| **AddressSanitizer + LeakSanitizer** trên gc_stress & thrash đa luồng | ✅ CLEAN — không heap error, không leak |
| **ThreadSanitizer** trên thrash đa luồng | ✅ CLEAN — không data race |
| GC memory bound: 600k allocations | ✅ peak RSS 4.7 MB (phẳng) |
| Error handling: `1/0`, index OOB, `"a"+1`, sai arity, undefined var, thiếu indent | ✅ message rõ ràng, exit 1, lỗi compile chỉ đúng dòng |
| `kamipy run` / `kamipy clean` / `--emit-llvm` / `--emit-ast` | ✅ hoạt động |

### Benchmarks (Linux x64)
| Metric | KamiPython | C++ -O2 | CPython 3.11 | Target |
|---|---|---|---|---|
| hello binary | **70 KB** (60 KB stripped) | 16 KB | — | < 5 MB ✅ |
| hello startup | **1.9 ms** | 1.8 ms | 12.6 ms | — |
| hello max RSS | **3.6 MB** | 3.6 MB | 7.5 MB | < 20 MB ✅ |
| fib(27) | **42 ms** | — | 44 ms | — |
| threading_test (4×10k) | 20 ms / 4.1 MB | — | — | — |

(Sau tối ưu "GIL elision" cho chương trình single-thread: fib(27) từ 89 ms → 42 ms.)

### Design deviations from Phase-0 doc / Khác biệt so với thiết kế Phase 0
1. **Không có KIR riêng ở v0.1** — AST hạ thẳng xuống LLVM IR; constant folding làm ở sema, trivially-dead-code (sau return/break) làm ở codegen. KIR vẫn là hướng mở rộng khi cần optimizer sâu hơn.
2. **Mark & sweep (không phải RC)** — chọn M&S vì kết hợp threading an toàn hơn: generated code không bao giờ giữ object pointer trong register qua call, mọi mutation dưới lock → GC đúng theo thiết kế, đã chứng minh bằng TSan/ASan.
3. **Phát textual LLVM IR + clang driver** thay vì link thư viện LLVM C++ vào kamipy — đơn giản, bền theo version (chỉ cần clang trên PATH), vẫn là LLVM backend thực thụ.
4. AST dùng `unique_ptr` (RAII) thay arena — arena là tối ưu tương lai, không đổi ngữ nghĩa.

### Known limitations / Giới hạn đã biết
- Windows x64: code path có (`_spawnvp`, `.exe`, `kamirt.lib`) nhưng **chưa test trên CI Windows**.
- Chưa hỗ trợ: class, closure, `try/except`, slicing, f-string, import module người dùng.
- Gọi trực tiếp theo tên hàm bind tại compile-time; nếu gán đè tên hàm bằng giá trị khác ở module level, call trực tiếp vẫn trỏ hàm gốc.
- `a[i] += v` đánh giá `a` và `i` hai lần.
- Threading dùng GIL → đúng đắn tuyệt đối nhưng CPU-bound code không scale tuyến tính theo số core (giống CPython).

---

## [Phase 0] — 2026-08-01 — Architecture Analysis / Phân tích kiến trúc

### Added / Thêm mới
- `docs/vi/ARCHITECTURE.md`, `docs/en/ARCHITECTURE.md` — phân tích kiến trúc chi tiết song ngữ: pipeline compiler, runtime object system, memory model, GC, binary generation, roadmap 16 phase.
- `CHANGELOG.md`, `README.md`.

### Test report
- Toolchain verification: ✅ cmake 3.28.3, clang++ 18.1.3, llvm-config 18.1.3.
- Code: N/A (docs only).
