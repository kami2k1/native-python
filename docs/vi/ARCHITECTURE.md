# KamiPython — Phân tích kiến trúc (PHASE 0)

> Tài liệu thiết kế chi tiết trước khi viết code. Bản tiếng Anh: [`docs/en/ARCHITECTURE.md`](../en/ARCHITECTURE.md)

---

## 1. Mục tiêu

Xây dựng compiler **AOT (Ahead-Of-Time)** hoàn chỉnh bằng **C++20**, biến một ngôn ngữ Python-like thành **native executable**, không phụ thuộc Python/CPython/PyInstaller/Nuitka.

```
Input:  main.py          (Python-like source)
Build:  kamipy build main.py -o app
Output: app / app.exe    (native binary, tự chứa runtime)
Run:    ./app  →  30
```

**Ràng buộc kỹ thuật:**

| Hạng mục          | Quyết định                          |
|-------------------|-------------------------------------|
| Ngôn ngữ          | C++20                               |
| Build system      | CMake ≥ 3.20                        |
| Backend           | LLVM IR + LLVM Backend (LLVM 18)    |
| Platform          | Linux x64, Windows x64              |
| Runtime           | Tự viết (libkamirt), static link    |
| Hello world size  | < 5 MB                              |
| Hello world RAM   | < 20 MB                             |

---

## 2. Pipeline biên dịch tổng thể

```
 main.py
   │  (Lexer)          — đọc UTF-8, sinh token stream, xử lý INDENT/DEDENT
   ▼
 Token Stream
   │  (Parser)         — recursive descent + Pratt cho expression
   ▼
 AST                    — cấp phát trên Arena, sống hết phiên compile
   │  (Semantic)       — symbol table, scope, name resolution, type inference
   ▼
 Typed AST
   │  (Optimizer)      — constant folding, DCE, inline nhỏ (mức KIR)
   ▼
 KIR (Kami IR)          — IR trung gian dạng 3-address, đơn giản, dễ tối ưu
   │  (Codegen)        — hạ KIR xuống LLVM IR, gọi runtime qua C ABI
   ▼
 LLVM IR (Module)
   │  (LLVM passes)    — mem2reg, instcombine, gvn, O2 pipeline
   ▼
 Machine Code (.o / .obj)
   │  (Linker)         — clang driver / lld, static link libkamirt.a
   ▼
 Executable (ELF / PE)
```

**Nguyên tắc:** mỗi tầng có input/output rõ ràng, test được độc lập, không tầng nào "biết" chi tiết nội bộ của tầng khác.

---

## 3. Cấu trúc thư mục

```
KamiPython/
├── compiler/
│   ├── lexer/        # Tokenizer, xử lý indentation
│   ├── parser/       # Recursive descent + Pratt
│   ├── ast/          # Node definitions, arena, printer/dumper
│   ├── semantic/     # Symbol table, scopes, type inference
│   ├── optimizer/    # Constant folding, DCE, inliner (trên KIR)
│   ├── ir/           # KIR: định nghĩa + builder + verifier
│   ├── codegen/      # KIR → LLVM IR, khai báo runtime ABI
│   └── linker/       # Driver: gọi clang/lld, static link runtime
├── runtime/
│   ├── value/        # Value tagged union, boxing/unboxing
│   ├── object/       # Object header, type table, method dispatch
│   ├── memory/       # Allocator interface, page allocator
│   ├── allocator/    # Size-class freelist allocator
│   ├── gc/           # Reference counting (+ cycle detector về sau)
│   ├── string/       # Immutable string: len + hash + data
│   ├── array/        # Dynamic array of Value
│   ├── hashmap/      # Open addressing, robin-hood
│   ├── exception/    # Error propagation (status code + panic)
│   └── io/           # runtime_print, stdin/stdout
├── stdlib/
│   ├── math/  ├── json/  ├── filesystem/  └── time/
├── cli/              # kamipy build|run|clean
├── tests/
│   ├── unit/         # GoogleTest / doctest cho từng module
│   └── integration/  # Compile → run → so sánh stdout
├── docs/
│   ├── vi/  └── en/
└── CMakeLists.txt
```

**Quy ước build:** hai artifact chính:
- `kamipy` — compiler CLI (link LLVM).
- `libkamirt.a` / `kamirt.lib` — runtime library, **không** phụ thuộc LLVM, được static link vào executable sinh ra.

---

## 4. Thiết kế Compiler Frontend

### 4.1 Lexer

Hand-written, không dùng generator (flex) — dễ debug, dễ báo lỗi đẹp, kiểm soát indentation.

**Token categories:**

| Loại        | Ví dụ                                        |
|-------------|----------------------------------------------|
| NUMBER      | `123`, `3.14` (int64 / double)               |
| STRING      | `"hello"` (escape: `\n \t \\ \" \xNN`)       |
| IDENTIFIER  | `variable`, `main`                           |
| KEYWORD     | `if else elif while for def return class import True False None and or not pass break continue` |
| OPERATOR    | `+ - * / % = == != < > <= >= ( ) [ ] { } : , .` |
| LAYOUT      | `NEWLINE`, `INDENT`, `DEDENT`, `EOF`         |

**Xử lý indentation (giống CPython):** giữ một stack độ sâu thụt lề. Đầu mỗi dòng logic, so sánh số cột:
- lớn hơn đỉnh stack → push, phát `INDENT`;
- nhỏ hơn → pop đến khi khớp, mỗi lần pop phát `DEDENT`; không khớp → lỗi `IndentationError`.
- Dòng trống/chỉ comment bị bỏ qua. Tab được chuẩn hoá = 8 cột (hoặc cấm trộn tab/space — v0.1 chọn **cấm trộn** cho an toàn).

Mỗi token mang `SourceLocation {file_id, line, col}` phục vụ báo lỗi.

### 4.2 Parser

**Recursive descent** cho statement, **Pratt (precedence climbing)** cho expression.

Bảng ưu tiên (thấp → cao):

```
or → and → not → == != < > <= >= → + - → * / % → unary - → call () / index [] / attr .
```

Grammar rút gọn v0.1 (EBNF):

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

**Error recovery:** panic-mode — khi gặp lỗi, sync đến `NEWLINE`/`DEDENT` gần nhất để báo được nhiều lỗi trong một lần chạy.

### 4.3 AST

- Node cấp phát trên **Arena** (xem §6), không dùng `unique_ptr` từng node → giải phóng O(1) khi compile xong.
- Hai họ node: `Expr` (Literal, Name, Binary, Unary, Call, Index, Attr) và `Stmt` (Assign, If, While, For, FuncDef, Return, ExprStmt, Import, ClassDef).
- Visitor pattern (`std::variant` + `std::visit` hoặc virtual accept — chọn **tagged enum + switch** vì đơn giản, cache-friendly, dễ thêm pass).
- Có `AstDumper` in cây dạng S-expression để test golden-file.

### 4.4 Semantic Analysis

**Symbol table** dạng chuỗi scope lồng nhau:

```
GlobalScope → FunctionScope → BlockScope (v0.1: function-level scoping như Python)
```

Nhiệm vụ:
1. **Name resolution** — biến dùng trước khi gán → lỗi compile (chặt hơn Python: bắt tại compile-time thay vì `NameError` runtime).
2. **Scope rules** — gán trong hàm tạo biến local; đọc fallback lên global (chưa hỗ trợ `global`/`nonlocal` ở v0.1).
3. **Type inference** — flow-based, lattice đơn giản:

```
        Unknown
       /   |    \
     Int Float String Bool None Array Map Func
       \   |    /
        Dynamic   (không thống nhất được → box thành Value)
```

Ví dụ:
```python
x = 10        # x: Int
y = x + 20    # Int + Int → y: Int  → codegen dùng i64 thuần, không boxing
s = "hi"      # s: String
z = f(x)      # f chưa biết kiểu trả về → z: Dynamic → boxed Value
```

Chiến lược: **unbox khi chứng minh được kiểu tĩnh** (fast path i64/double), còn lại rơi về `Value` động (§5). Đây là điểm ăn tiền về hiệu năng so với interpreter.

### 4.5 KIR — IR trung gian

Tại sao không đi thẳng AST → LLVM IR?
- Cần chỗ để tự làm constant folding / DCE / inline **trước** khi phát sinh boxing (LLVM khó gỡ boxing sau khi đã sinh call runtime).
- IR riêng giúp test optimizer không cần LLVM.

Thiết kế: **3-address code, CFG basic-block**, chưa cần SSA đầy đủ ở v0.1:

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

Op có biến thể typed (`add.i64`, `add.f64`) và dynamic (`add.dyn` → gọi `kami_add(Value, Value)`).

### 4.6 Codegen (LLVM 18)

- Một `llvm::Module` cho mỗi chương trình (v0.1 single-module; import gộp module — §9).
- Mapping:
  - `Int` → `i64`, `Float` → `double`, `Bool` → `i1`
  - `Value` → struct `{ i32 tag, [4 x i8] pad, i64 payload }` (16 byte) truyền qua pointer theo C ABI.
- Mọi thao tác dynamic gọi hàm runtime `extern "C"`:

```
Python:  print(a + b)
KIR:     %2 = add.i64 %0, %1 ; call @runtime_print_i64(%2)
LLVM:    %2 = add i64 %0, %1
         call void @kami_print_i64(i64 %2)
```

- Entry: codegen phát `main()` chuẩn C gọi `kami_rt_init()` → `@kamipy_main()` → `kami_rt_shutdown()`.
- Pass pipeline: `PassBuilder` O2 mặc định (`mem2reg`, `instcombine`, `gvn`, `sccp`, …).

### 4.7 Linker driver

Không tự viết linker. Compiler phát `.o` (qua `TargetMachine::addPassesToEmitFile`) rồi gọi:

- **Linux:** `clang++ main.o -L<rt> -lkamirt -static-libgcc -o app` (hoặc `ld.lld` trực tiếp).
- **Windows:** `lld-link main.obj kamirt.lib /out:app.exe /subsystem:console`.

Runtime luôn **static link** → executable tự chứa, không cần DLL/so ngoài libc/kernel32.

---

## 5. Thiết kế Runtime

### 5.1 Value — dynamic type system

```cpp
enum class Type : uint32_t {
    NONE, BOOL, INT, FLOAT, STRING, ARRAY, MAP, OBJECT, FUNCTION
};

struct Value {              // 16 byte, trivially copyable
    Type type;
    union {
        int64_t  i;         // INT / BOOL
        double   f;         // FLOAT
        void*    ptr;       // STRING/ARRAY/MAP/OBJECT/FUNCTION → heap object
    };
};
```

- Kiểu vô hướng (`INT/FLOAT/BOOL/NONE`) nằm **inline**, không cấp phát heap.
- Kiểu tham chiếu trỏ đến heap object có header (§5.2).
- *Alternative đã cân nhắc:* NaN-boxing (8 byte) — nhanh hơn nhưng phức tạp, khó debug; hoãn đến khi có benchmark chứng minh cần.

### 5.2 Heap Object

```cpp
struct ObjHeader {
    uint32_t type;      // Type tag
    uint32_t rc;        // reference count
};

struct KamiString { ObjHeader h; uint64_t len; uint64_t hash; char data[]; };   // immutable
struct KamiArray  { ObjHeader h; uint64_t len, cap; Value* items; };
struct KamiMap    { ObjHeader h; /* open addressing, robin hood */ };
```

String immutable → hash cache một lần, an toàn làm key map, chia sẻ không cần copy.

### 5.3 Memory model & GC

**Compiler side:** Arena allocator (§6) — mọi AST/KIR node sống trong arena, free O(1).

**Runtime side — quyết định: Reference Counting** (v0.1):

| Tiêu chí          | RC                         | Mark & Sweep            |
|-------------------|----------------------------|-------------------------|
| Độ phức tạp v0.1  | Thấp ✅                    | Trung bình              |
| Deterministic     | Có ✅ (free ngay)          | Không (pause)           |
| Cycle             | Leak ❌ (cần cycle detector)| Xử lý được ✅           |
| Codegen           | Chèn incref/decref         | Cần stack map/root scan |

v0.1 ngôn ngữ chưa có cấu trúc dễ tạo cycle (chưa có closure capture lẫn nhau, class field gán vòng hiếm) → RC là lựa chọn thực dụng. Cycle detector (trial deletion kiểu CPython) là hạng mục PHASE 6+ nếu cần. Codegen chèn `kami_incref/decref` tại điểm gán/ra khỏi scope; optimizer khử cặp incref/decref thừa.

### 5.4 Exception & IO

- v0.1: lỗi runtime (chia 0, index out of range, type error) → in thông báo + `exit(1)` qua `kami_panic(msg, loc)`.
- v1.x: `try/except` — lựa chọn thiết kế: propagation bằng status flag (không dùng C++ exception qua biên giới C ABI).
- IO: `kami_print_value(Value)`, cùng các fast-path `kami_print_i64/f64/str` để tránh boxing khi kiểu đã biết.

---

## 6. Arena Allocator (compiler)

```
Compile start ─▶ arena.alloc(AST nodes) ─▶ arena.alloc(KIR) ─▶ emit ─▶ arena reset (free O(1))
```

- Block 64 KiB, bump-pointer, không free lẻ; grow gấp đôi.
- RAII: `Arena` hủy → trả toàn bộ block. Không leak theo thiết kế; kiểm chứng bằng ASan/Valgrind trong CI.

---

## 7. Optimizer (PHASE 9 — thiết kế trước)

Trên KIR, chạy theo thứ tự:
1. **Constant folding** — `10 + 20` → `30` (fold cả so sánh, string concat hằng).
2. **Dead code elimination** — lệnh không side-effect + kết quả không dùng; block không reachable.
3. **Inline** — hàm ≤ N ops (N≈16), không đệ quy.
4. **Remove unused runtime** — chỉ khai báo extern hàm runtime thực sự được gọi + `--gc-sections`/`/OPT:REF` khi link → binary nhỏ.

LLVM O2 lo phần còn lại (GVN, LICM, vectorize…).

---

## 8. Binary generation — giải thích đầy đủ

```
.py ──lexer/parser──▶ AST ──semantic──▶ Typed AST ──lower──▶ KIR
KIR ──codegen──▶ LLVM IR ──llc/TargetMachine──▶ .o (x86-64 object, ELF/COFF)
.o + libkamirt.a ──linker(lld)──▶ executable (ELF trên Linux, PE trên Windows)
```

- **Machine code** do LLVM `TargetMachine` sinh trực tiếp trong tiến trình `kamipy` (không cần gọi `llc` ngoài).
- Executable chứa: code người dùng đã compile + runtime + phần libc static/dynamic tùy platform. **Không** chứa interpreter, bytecode hay libpython.
- Cross-target: LLVM sinh code cho triple bất kỳ (`x86_64-pc-windows-msvc`, `x86_64-unknown-linux-gnu`); link cần toolchain của platform đích.

---

## 9. Hệ thống import (as built)

**Import tĩnh, không import động lúc chạy.** Mọi module chương trình import đều được
compile vào cùng một executable: không `sys.modules`, không import hook, không `.pyc`.
Toàn bộ phân hệ nằm ở `compiler/src/modules.cpp`.

### 9.1 `import X` tìm ở đâu

```
import X
  1. project        X.py hoặc X/__init__.py cạnh file input
  2. native         module runtime đã cài (math, os, json, socket, re, ...)
  3. system Python  bản CPython dò được trên máy
  4. bundled pylib  runtime/pylib/, ship cạnh binary kamipy
```

File trong project **luôn** thắng, đúng semantics CPython. Module native được ưu tiên
hơn source CPython là có chủ ý: `import json` nên tiếp tục là lời gọi primitive C-ABI
chứ không kéo cả package `json/` vào. Còn lại thì đọc thẳng stdlib thật trên máy; thư
mục `pylib/` đi kèm là **fallback** — dùng khi máy không có Python, hoặc khi source
CPython của module đó dùng cú pháp compiler chưa nhận. Ứng viên nào **parse** thất bại
thì rơi xuống ứng viên tiếp theo; chỉ khi hết lựa chọn driver mới cảnh báo và bỏ qua.

Module được ghép vào chương trình chính theo thứ tự dependency-first, khối
`if __name__ == "__main__":` bị bỏ, nên `utils.helper()` trở thành lời gọi `helper()`
thông thường. Import vòng được phát hiện và báo lỗi.

### 9.2 Dò môi trường Python hệ thống

`find_system_python_stdlib()` trả về danh sách import root, tốt nhất trước, và cache
kết quả. Hàm này **không** spawn process nào — chỉ đọc biến môi trường, Registry
Windows và filesystem.

| Thứ tự | Nguồn |
|---|---|
| 1 | `$KAMIPY_PYTHON_STDLIB` — ghi đè tường minh, phân tách `:`/`;` |
| 2 | `$PYTHONHOME` → `<prefix>/Lib` (Windows) hoặc `<prefix>/lib/python3.*` |
| 3 | Từng entry của `$PYTHONPATH`, dùng nguyên trạng |
| 4 | `python3` / `python.exe` trên `$PATH` → prefix của nó (bắt được virtualenv, pyenv, conda) |
| 5 | Windows: Registry `HKCU`/`HKLM\Software\Python\PythonCore` (kể cả `Wow6432Node`) → `InstallPath`; `%LOCALAPPDATA%\Programs\Python\Python3*\Lib`; `C:\Python3*\Lib`; `C:\Program Files[ (x86)]\Python3*\Lib` |
| 5 | Unix: `/usr/lib/python3.*`, `/usr/local/lib/python3.*`, `/usr/lib64/...`, `/opt/homebrew/...`, `~/.pyenv/versions/*/lib/python3.*`, framework build macOS |

Một thư mục chỉ được nhận nếu có "dấu vân tay" stdlib (`os.py`, `json/__init__.py`
hoặc `types.py`), nhờ vậy một folder backup lạc không thể làm bẩn search path. Trong
cùng một mức ưu tiên, Python mới hơn xếp trước.

`kamipy paths` in toàn bộ danh sách; `kamipy build -v` báo từng resolve;
`--no-system-stdlib` (hoặc `KAMIPY_NO_SYSTEM_STDLIB=1`) tắt auto-discovery.

### 9.3 C extension và C ABI

Stdlib CPython chỉ có một nửa là Python — nửa còn lại là C extension module.
`compiler/src/cext.cpp` map từng thành viên của chúng sang một trong hai thứ:

* một **runtime primitive** (`_json.loads` → `KB_JSON_LOADS`), hoặc
* một **lời gọi C trực tiếp** sinh thẳng vào IR (`_math.sqrt` → `call double @sqrt(double)`).

`runtime/src/syscalls.cpp` chứa các primitive không có cách diễn đạt tự nhiên bằng
Python: I/O theo file descriptor (`open/read/write/close/lseek`), `gethostname`, và
`struct.pack`/`unpack`/`calcsize`.

`ctypes` và `cffi` được hiểu ở compile time, không phải runtime:

```python
lib = ctypes.CDLL("libm.so.6")     # → cờ link -lm, suy ra từ chính tên file
lib.sqrt.restype = ctypes.c_double # → kiểu trả về
lib.sqrt.argtypes = [ctypes.c_double]
lib.sqrt(2.25)                     # → call double @sqrt(double 2.25)
```

Chữ ký được mang dưới dạng một chuỗi gọn: ký tự đầu là kiểu trả về, phần còn lại là
tham số, và **mỗi độ rộng C có mã riêng** — `'l'` là `int` 32-bit của C, `'i'` là
`long`/`size_t`/pointer 64-bit, `'f'` là `float`, `'d'` là `double`, `'s'` là
`const char*`. Truyền i64 vào tham số `int` là undefined behaviour chứ không phải
tiện tay, nên codegen sinh đúng `trunc`/`sext`/`fptrunc`/`fpext` quanh lời gọi. Đối số
được unbox qua `kami_c_arg_i64/f64/cstr` nên sai kiểu là lỗi Python bắt được, không
phải stack hỏng. Driver thu thập các thư viện những binding này cần rồi thêm
`-l<name>` vào dòng lệnh clang.

---

## 10. CLI (PHASE 12 — thiết kế)

```
kamipy build <file.py> [-o app] [-O0|-O1|-O2] [--emit-llvm] [--emit-ast]
                       [--no-system-stdlib] [-v|--verbose]
kamipy run   <file.py>      # build vào .kamipy-cache/ rồi chạy
kamipy paths                # mọi thư mục mà `import` sẽ tìm, theo thứ tự
kamipy clean                # xoá .kamipy-cache/
```

`--emit-llvm`/`--emit-ast` phục vụ debug và golden-file test. `paths` và `-v` có
mặt để việc resolve import luôn chẩn đoán được, không phải đoán: `paths` trả lời
"sẽ tìm ở đâu?", `-v` trả lời "thực tế tìm thấy ở đâu, và link với thư viện nào?".

---

## 11. Chiến lược testing

| Tầng        | Cách test                                                        |
|-------------|------------------------------------------------------------------|
| Lexer       | Unit: source → token list mong đợi (kể cả INDENT/DEDENT, lỗi)    |
| Parser      | Unit + golden: source → AST dump S-expression                    |
| Semantic    | Unit: chương trình lỗi phải báo đúng lỗi, đúng dòng              |
| KIR/Optimizer| Golden: KIR trước/sau pass                                      |
| Codegen     | `--emit-llvm` + `FileCheck`-style assert                         |
| Integration | **compile → run → so sánh stdout** (bảng test `.py` + `.expected`)|
| Memory      | ASan + LeakSanitizer trên toàn bộ test suite                     |
| Benchmark   | hello-world size/RAM/startup vs CPython, Go, C++ (PHASE 14)      |

Framework: **doctest** (header-only, nhẹ) cho unit; script CMake/CTest cho integration.

---

## 12. Ngôn ngữ v0.1 — phạm vi cam kết

Hỗ trợ: biến, `int/float/str/bool/None`, số học + so sánh + logic, `if/elif/else`, `while`, `for i in range(...)`, `def`/`return`/gọi hàm/đệ quy, `print()`, comment `#`.

Chưa hỗ trợ (roadmap): class đầy đủ, closure, generator, `try/except`, slice, f-string, varargs, module người dùng (PHASE 10).

---

## 13. Rủi ro & đối sách

| Rủi ro                                   | Đối sách                                                  |
|------------------------------------------|-----------------------------------------------------------|
| API LLVM thay đổi giữa các version       | Pin LLVM 18; wrap sau interface `codegen/LLVMBackend`     |
| Boxing làm chậm code động                | Type inference unbox fast-path; benchmark từng phase      |
| RC leak do cycle                          | v0.1 giới hạn ngữ nghĩa; cycle detector khi cần           |
| Link Windows phức tạp (CRT, subsystem)   | Dùng lld-link + CI Windows sớm từ PHASE 8                 |
| Indentation lexer nhiều edge-case        | Test bảng lớn ngay PHASE 2, cấm trộn tab/space            |

---

## 14. Roadmap & tiêu chí hoàn thành từng phase

| Phase | Nội dung             | Exit criteria (phải pass mới sang phase sau)                  |
|-------|----------------------|---------------------------------------------------------------|
| 0     | Kiến trúc (tài liệu này) | Docs VI/EN + changelog + PR                               |
| 1     | CMake foundation     | `cmake -B build && cmake --build build` ra `kamipy` chạy được |
| 2     | Lexer                | Unit test token stream pass 100%                              |
| 3     | Parser               | AST dump golden test pass                                     |
| 4     | Semantic             | Name/scope/type test pass, báo lỗi đúng dòng                  |
| 5     | Value system         | Unit test Value/Type pass                                     |
| 6     | Memory/GC            | ASan sạch, không leak                                         |
| 7     | Runtime lib          | `kami_print` các kiểu chạy đúng                               |
| 8     | LLVM backend         | `print(10)` compile → chạy in `10`                            |
| 9     | Optimizer            | Golden KIR test; binary nhỏ hơn baseline                      |
| 10    | Import               | `import math` compile-link chạy đúng                          |
| 11    | Stdlib               | Test từng module stdlib                                       |
| 12    | CLI                  | `build/run/clean` hoạt động 2 platform                        |
| 13    | Integration tests    | Toàn bộ bảng test pass                                        |
| 14    | Benchmark            | hello < 5 MB, RAM < 20 MB, báo cáo so sánh                    |
| 15    | Binary protection    | strip + optimize, tài liệu giới hạn                           |

**Milestone cuối:** `kamipy build main.py` → `main.exe` chạy in `30`, không cần Python.

---

## 15. Ghi chú triển khai v0.1 (as-built)

Bản triển khai v0.1 (xem `CHANGELOG.md` mục v0.1.0) có các khác biệt có chủ đích so với thiết kế trên:

1. **AST → LLVM IR trực tiếp, chưa có KIR riêng.** Constant folding chạy ở tầng semantic; dead code sau `return/break/continue` bị loại ngay tại codegen. KIR vẫn là hướng mở rộng khi cần optimizer sâu hơn (unboxing xuyên hàm, inline).
2. **GC là mark & sweep (không phải reference counting).** Lý do: kết hợp với threading an toàn và đơn giản hơn — codegen tuân thủ bất biến *"object pointer không bao giờ sống trong register qua một call; mọi giá trị nằm trong frame slot đã đăng ký làm root; mọi mutation đi qua runtime call giữ global lock"*. Nhờ đó GC + đa luồng đúng đắn theo thiết kế (đã chứng minh bằng ThreadSanitizer/AddressSanitizer).
3. **Threading theo mô hình GIL** như CPython: mọi runtime call giữ một global lock; `sleep/join` nhả lock khi block. Tối ưu quan trọng: khi chương trình chưa spawn thread nào, lock được **bỏ qua hoàn toàn** (flag bật vĩnh viễn tại lần spawn đầu — chuyển trạng thái an toàn vì lúc đó chỉ có đúng một thread và nó đang giữ lock thật). Kết quả: single-thread nhanh gấp ~2 lần, đa luồng vẫn đúng.
4. **Codegen phát textual LLVM IR (`.ll`) rồi gọi `clang++`** làm backend + linker driver (qua `fork/execvp`, không qua shell). Đơn giản, không phụ thuộc phiên bản thư viện LLVM C++, vẫn là pipeline LLVM thực thụ. In-process `TargetMachine` là tối ưu tương lai.
5. **AST dùng `unique_ptr` (RAII)** thay arena — thay đổi hiệu năng compiler, không đổi ngữ nghĩa.
