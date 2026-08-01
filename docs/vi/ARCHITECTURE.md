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

## 9. Import system và standard library

**Static import**, không import động runtime — và từ v0.6.0 thêm một nguyên tắc bất di bất dịch:

> **Chỉ dịch, không tự viết lại.** Việc của compiler là *dịch* mã Python thành mã native,
> không phải viết lại thư viện Python bằng C++.

```
import re
   │ resolve: <thư mục input>/re.py, rồi $KAMIPY_STDLIB hoặc <kamipy>/../stdlib/re.py
   ▼ lex + parse chính file Python của module
   ▼ đệ quy vào import của nó (dependency trước; cycle → lỗi)
   ▼ đổi tên các binding cấp module thành namespace:  match → re__match
   ▼ ghép statement vào trước thân chương trình → một AST → một LLVM module
```

Nhờ vậy `re`, `json`, `logging`, `os`, `os.path`, `socket`, `requests`, `string` đều là
**file Python trong `stdlib/`**, được biên dịch y như code người dùng: chương trình
`import re` có regex engine native tĩnh vì *mã Python của engine đã được dịch*, chứ
không phải vì link thêm code C++.

### 9.1 Namespace mà không cần module object

Ngôn ngữ không có module object lúc runtime, nên bundler tạo namespace ở compile-time
bằng cách đổi tên các binding cấp module:

| Trong `stdlib/re.py` | Sau khi bundle | Gọi bằng |
|---|---|---|
| `def match(...)` | `def re__match(...)` | `re.match(...)` |
| `class Pattern` | `class re__Pattern` | `re.Pattern` |
| `_cache = {}` | `re___cache = {}` | (riêng của module) |
| trong `stdlib/os/path.py`: `def join` | `def os_path__join` | `os.path.join(...)` |

Bộ đổi tên hiểu scope: tên local (hoặc tham số) của hàm che tên cấp module và **không**
bị đổi, còn `global x` thì trỏ lại global đã namespace hoá. Vì `.` không thể xuất hiện
trong identifier Python, tên của chương trình không bao giờ đụng nội bộ module — user
viết `def match()` vẫn dùng được `re.match`. Sema resolve `re.match` /
`from re import match as m` qua cùng một bảng, và thông báo lỗi được dịch ngược
(`re.findall() takes 3 argument(s)`, không bao giờ hiện `re__findall`). Số dòng AST của
mỗi module được dịch sang một khoảng riêng nên lỗi trong mã đã dịch báo đúng
`stdlib/re.py:57: error: ...`.

### 9.2 Phần nào còn là C++, và vì sao

`libkamirt` chỉ còn hai loại C++:

1. **Value model + GC** — bản thân ngôn ngữ: tagged value, string, list, dict, set,
   object, toán tử, mark & sweep (§5, §6).
2. **C-ABI binding tới OS** (`runtime/src/syscalls.cpp`, module `_kami`) — forward một
   dòng tới libc: `open/read/write/close/lseek`, `stat/mkdir/rmdir/unlink/rename/
   getcwd/chdir`, `opendir/readdir`, `socket/connect/bind/listen/accept/send/recv/
   setsockopt`, `getenv/system/getpid/localtime`.

Ranh giới: *nếu không viết được bằng Python vì cần một struct ngôn ngữ không diễn đạt
được (`sockaddr`, `struct stat`, `DIR*`) thì đó là binding; còn lại là Python.*
`socket.py` là class Python giữ một fd integer; `requests.py` nói HTTP/1.1 qua class đó;
`logging.py` render record rồi ghi bằng `_kami.fd_write`. `math`, `time`, `random`,
`threading` vẫn native vì bản thân chúng *là* binding trực tiếp tới libm / libc /
`std::thread`.

---

## 10. CLI (PHASE 12 — thiết kế)

```
kamipy build main.py [-o app] [-O0|-O2] [--emit-llvm] [--emit-kir]
kamipy run   main.py        # build vào thư mục tạm rồi exec
kamipy clean                # xóa .kamipy-cache/
```

`--emit-llvm`/`--emit-kir` phục vụ debug và test golden-file.

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

---

## 16. v0.6.0: "chỉ dịch, không tự viết" (as-built)

v0.3–v0.5.1 đã dựng một bản stdlib bằng C++ ngay trong runtime: `runtime/src/netio.cpp`
chứa JSON parser, formatter cho logging, wrapper `os`/`os.path` trên `std::filesystem`,
model socket và một HTTP client gọi `curl`; `runtime/src/kami_regex.cpp` chứa regex
engine tự viết; `runtime/src/http_client.cpp` thay lời gọi `curl` bằng 562 dòng
WinHTTP/OpenSSL. Cả ba file đã bị xoá.

| | trước (v0.5) | sau (v0.6) |
|---|---|---|
| stdlib | ~2.100 dòng C++ trong runtime | ~2.100 dòng Python trong `stdlib/` |
| `re` | engine C++ tự viết | `stdlib/re.py`, backtracking bằng chuỗi continuation tường minh |
| `requests` | subprocess `curl` (v0.3–v0.5), rồi 562 dòng C++ trên WinHTTP/OpenSSL (v0.5.1) | `stdlib/requests.py` nói HTTP/1.1 qua `stdlib/socket.py` |
| `socket` | heap type `KT_SOCKET` + method C++ | class Python trên một fd integer |
| `json`, `logging`, `os`, `os.path`, `string` | builtin id trong C++ | module Python |
| namespace module | splice phẳng (`utils.f` → `f`) | namespace đổi tên (`re.match` → `re__match`) |
| tag runtime | `KT_SOCKET` | đã bỏ |
| binary hello-world | 365 KB | 225 KB |

Những hệ quả cần biết:

- **Đúng đắn hơn.** Hai bug biến mất cùng đoạn C++ chứa chúng: heap corruption trong
  GC-rooting của `re.findall` cũ, và việc engine cũ lệch ngữ nghĩa so với CPython.
  Engine Python được so khớp từng byte với CPython trong CI.
- **Tìm và sửa được một bug GC.** Mã Python cấp phát nhiều đã phơi ra một vi phạm thứ tự
  rooting trong runtime: `out->tag = KT_STR` được ghi *trước* `str_new()`, nên một lần
  collect do chính lần cấp phát đó kích hoạt sẽ đi theo payload cũ của slot như một con
  trỏ. Mọi vị trí như vậy giờ cấp phát trước rồi mới publish (`put_str`), và
  `KAMIPY_GC_STRESS=1` (collect trước mỗi lần cấp phát) chạy lại toàn bộ suite integration
  trong CI. Cũng nhờ công tắc đó mà tìm ra hai bug rooting nữa: `set("chuỗi")` truyền một
  slot chưa root hoá cho `kami_iter_prep` (hàm này cấp phát một object mỗi ký tự), và
  `as_list_pinned()` đăng ký pin *sau* khi vật chất hoá list.
- **Regex chậm hơn.** `re.findall(r"\d+", ...)` trên chuỗi 8 KB, lặp 50 lần, mất ~0,69 s
  so với ~0,04 s của engine C++ đã xoá — giá phải trả trung thực khi chạy cùng một thuật toán
  theo cùng cách CPython làm. Bản native vẫn ngang tầm engine C của CPython với input
  nhỏ, và matcher có fast-path một ký tự cùng bộ lọc quét theo atom đầu.
- **`requests` chưa hỗ trợ HTTPS.** Client cũ thừa hưởng TLS từ `curl`; client Python cần
  binding tới một thư viện TLS, hiện chưa có, nên `https://` báo lỗi rõ ràng thay vì
  âm thầm hạ cấp.

---

## 17. v0.7.0: calling convention variadic, FFI, thread, cross-target

### 17.1 Calling convention của Python, đã biên dịch

`*args`, `**kwargs` và tham số keyword-only được **dịch**, không còn bị parser từ
chối. Quyết định thiết kế đáng nói: **dict keyword đi bằng một kênh riêng**, không
phải làm tham số cuối.

```
typedef void (*KamiFn)(KamiValue* ret, KamiValue** argv, int64_t nargs,
                       KamiValue* captures, KamiValue* kwargs);
```

Lý do: khi có `*args`, một dict ở cuối không thể phân biệt với một tham số vị trí
nữa — với `def f(*a, **k)` thì `f(x, y)` và `f(x)` + dict là cùng một hình dạng.
Kênh riêng cũng giữ cho việc kiểm tra arity chỉ liên quan tới tham số vị trí.

| Tính năng | Nơi thực hiện |
|---|---|
| `def f(a, *rest)` | prologue gọi `kami_pack_args(slot, argv, nargs, nfixed)` |
| `def f(**opts)` | prologue copy `%kwargs`, hoặc tạo dict rỗng khi null |
| `def f(*, mode="x")` | prologue gọi `kami_kwarg_take(slot, %kwargs, "mode")`, không có thì dùng default |
| `f(1, k=2)` (biết callee) | sema điền tham số theo vị trí; phần dư thành dict literal |
| `f(*seq)` / `f(**map)` | `kami_call_spread` / `kami_method_spread` dựng argv lúc chạy |
| `a, *mid, b = seq` | `kami_unpack_star` cắt phần giữa ra |

Một mapping lúc chạy vẫn phải tới được tham số **có tên** — `f(**{"b": 10})` — nên
mỗi hàm đã biên dịch mang theo bảng tên tham số vị trí của nó, và
`kami_call_value_kw` bind theo tên trước khi đưa phần còn lại cho `**kwargs`.
Nhờ vậy idiom decorator hoạt động trọn vẹn:

```python
def deco(fn):
    def inner(*a, **k):
        return fn(*a, **k) * 2      # chuyển tiếp mọi hình dạng tham số
    return inner

@deco
def scaled(a, b=3): return a * b

scaled(5, b=10)                     # 100
```

<callout>
Trước thay đổi này, đúng chương trình trên vẫn biên dịch và **âm thầm** trả 30:
keyword truyền cho một hàm có decorator bị bỏ mất.
</callout>

### 17.2 ctypes là cấu trúc của compile-time

`ctypes.CDLL` không tạo object lúc chạy. Handle chỉ tồn tại trong compiler,
`argtypes`/`restype` là khai báo, và lời gọi trở thành lời gọi C trực tiếp:

```python
lib = ctypes.CDLL("libm.so.6")
lib.sqrt.argtypes = [ctypes.c_double]
lib.sqrt.restype = ctypes.c_double
lib.sqrt(144.0)
```

```llvm
%1 = call double @kami_to_f64(ptr %slot)   ; marshal
%2 = call double @sqrt(double %1)          ; lời gọi C thật
call void @kami_make_float(ptr %ret, double %2)
```

`declare double @sqrt(double)` được phát kèm, và thư viện nêu trong `CDLL(...)`
được dịch thành cờ linker (`libm.so.6` → `-lm`, `./build/libfoo.so` →
`-L./build -lfoo`). Đó là toàn bộ mẹo, và cũng là giới hạn: đây là **biên dịch**,
không phải `dlopen`. Thư viện phải tồn tại lúc link, và một đường dẫn tính lúc
chạy thì không dùng được. Có thể truyền thêm `.c`, `.cpp`, `.o`, `.a`, `.lib`
(và cờ `-l`/`-L`) trực tiếp cho `kamipy build`, nên một file C cạnh chương trình
nằm luôn trong cùng một executable.

### 17.3 Thread

`stdlib/threading.py` là Python: `Thread(target=, args=, kwargs=)` với
`start/join/is_alive`, `Lock`/`RLock` với `acquire/release/__enter__`. Phần native
chỉ còn bốn primitive — spawn, join, alive và một bảng mutex — mỗi cái là wrapper
mỏng của `std::thread` / `std::recursive_mutex`.

`Thread.start()` spawn một **closure**, đó là cách mọi hình dạng tham số tới được
một primitive chỉ nhận một giá trị:

```python
def _runner(target, args, kwargs):
    def go():
        target(*args, **kwargs)
    return go
```

### 17.4 Cross-compilation

`--target <triple>` ghi triple vào module và yêu cầu clang dùng backend đó (kèm
`lld`); `--sysroot` trỏ tới header/thư viện của platform đích. Điều cần nói thẳng,
và thông báo lỗi giờ cũng nói: **runtime cũng phải được build cho target đó**.
`libkamirt.a` là C++, nên cross-compile một chương trình nghĩa là phải cross-build
runtime rồi trỏ `KAMIPY_RT_LIB` vào nó. Không có bước đó, `--target` chỉ đi được
tới bước link.

### 17.5 Đã đánh giá và chủ động hoãn

Hai mục trong roadmap được **đo** thay vì làm nửa vời.

**`invoke`/`landingpad` tường minh (giai đoạn 6.1).** Exception hiện *đã* unwind
qua platform unwinder: `kami_raise` throw một `KamiError` C++, `kami_try` bắt nó,
và compiler outline mỗi thân `try` thành một hàm riêng. Chuyển codegen sang
`invoke` + `landingpad` sẽ bỏ được phần outline và một lời gọi gián tiếp mỗi
`try`, nhưng **không thêm năng lực mới**, và sẽ đẩy việc sửa lại registry root của
GC (`kami_try` cắt bớt registry mà các `kami_frame_pop` bị bỏ qua để lại) vào mã
sinh ra, nơi mọi landing pad đều phải làm đúng việc đó. Chi phí đo được hiện nay
là một lời gọi mỗi lần vào `try`, nên đây là một refactor có rủi ro thật mà người
dùng không thấy lợi ích. Thay vào đó đã **sửa một bug thật** của cơ chế outline:
thân đã outline không bao giờ nhận con trỏ `captures` của closure bao ngoài, nên
`try` bên trong một closure có đọc biến captured thì **không biên dịch được**.

**Escape analysis / cấp phát trên stack (giai đoạn 6.2).** GC quét các frame slot
đã đăng ký, và `gc_collect` sweep một danh sách object toàn cục duy nhất. Một
object cấp phát trên stack vì thế phải bị loại khỏi danh sách đó nhưng vẫn được
trace như một root — khả thi, nhưng nó tương tác với mọi đường có thể lưu con trỏ
vào một object sống lâu hơn (`list.append`, `attr_set`, `map_set`, hay trả về nó),
nên cần một escape analysis thực sự, không phải heuristic. Đo trước: với live set
20.000 object và tốc độ cấp phát cao, mark & sweep tốn ~24 ms so với ~13 ms của
CPython cho cùng chương trình — khoảng cách 2×, và phần chi phối là **mark lại
toàn bộ live set** ở mỗi chu kỳ. GC theo thế hệ (phần lớn thời gian chỉ trace
object mới) đánh trực diện vào con số đó và không cần compiler; escape analysis
đánh vào một phần nhỏ hơn và lại cần compiler. Thứ tự đề nghị: GC thế hệ trước,
escape analysis sau; worklist khi mark đã được tái dùng giữa các chu kỳ như bước
đầu tiên.

**Concurrency không GIL (giai đoạn 7.2).** Hiện mọi runtime call giữ một recursive
mutex, được bỏ qua hoàn toàn khi chương trình còn đơn luồng, và được nhả quanh các
thao tác blocking. Bỏ nó đi đòi hỏi: arena cấp phát riêng cho từng thread,
safepoint của GC kèm phối hợp stop-the-world (không thể collect một thread đang
giữ con trỏ thô giữa hai runtime call), mutation nguyên tử hoặc phân mảnh cho
list/dict dùng chung, và một memory model cho data race ở mức người dùng. Mỗi mục
đó tự thân đã lớn hơn mọi thứ trong tài liệu này, và một bản làm nửa vời **không
lỗi to tiếng** — nó thỉnh thoảng làm hỏng bộ nhớ khi tải cao. Mô hình biên dịch
khiến việc này **khả thi** (không có interpreter loop để tuần tự hoá), và trình tự
trung thực là: safepoint → cấp phát theo thread → đăng ký root lock-free → bỏ
global lock, với ThreadSanitizer trong CI ở từng bước.
