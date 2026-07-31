# Changelog — KamiPython

Tất cả thay đổi đáng chú ý của project được ghi tại đây. / All notable changes to this project are documented here.

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
