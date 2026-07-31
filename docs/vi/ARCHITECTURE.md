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

## 9. Import system (PHASE 10 — thiết kế)

**Static import**, không import động runtime:

```
import math
   │ resolve: stdlib trước, rồi file cạnh main.py
   ▼ compile module (đệ quy, phát hiện cycle → lỗi)
   ▼ mỗi module → một LLVM module / object riêng, symbol prefix kami_mod_<name>_
   ▼ link tất cả vào một executable
```

Stdlib (`math`, `json`, `filesystem`, `time`, `random`) viết bằng C++ trong `libkamirt`, expose qua bảng symbol để semantic analyzer biết chữ ký hàm.

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
