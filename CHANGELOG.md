# Changelog — KamiPython

Tất cả thay đổi đáng chú ý của project được ghi tại đây. / All notable changes to this project are documented here.

---

## [v0.6.0] — 2026-08-01 — "Chỉ dịch, không tự viết": stdlib bằng Python + C-ABI syscalls

Refactor kiến trúc lớn. Nguyên tắc: **compiler dịch mã Python ra native, không viết lại
thư viện Python bằng C++.** / A large architectural refactor around one rule: **the
compiler translates Python source into native code; it does not reimplement Python's
libraries in C++.**

### Removed — mã C++ thủ công mô phỏng module cấp cao
- **Xoá `runtime/src/netio.cpp`** (1.100 dòng): JSON parser/serializer, formatter của
  `logging`, wrapper `os`/`os.path` trên `std::filesystem`, HTTP client gọi subprocess
  `curl`, `Response`/`Match` "internal class", `percent_format`.
- **Xoá `runtime/src/kami_regex.cpp` + `kami_regex.h`** (410 dòng): regex engine tự viết.
- **Xoá `runtime/src/http_client.cpp`** (562 dòng, thêm ở v0.5.1): HTTP client C++ trên
  WinHTTP/OpenSSL. Thay bằng `stdlib/requests.py` nói HTTP/1.1 bằng Python trên
  `stdlib/socket.py` (để lại: chưa có TLS → `https://` báo lỗi rõ ràng).
- **Xoá kiểu heap `KT_SOCKET`** và toàn bộ `socket_method` trong `ops.cpp`; xoá nhánh
  hardcode `Match`/`Response` trong `kami_method`.
- Xoá 27 builtin id của `os/os.path/logging/json/socket/requests/re` và các hàm rewrite
  riêng trong sema (`rewrite_logging`, `rewrite_module_kwargs`, ca đặc biệt `os.path`).
- Xoá file rác ở gốc repo: `test.txt`, `result.csv`, `min_cost.txt`.

### Added — standard library viết bằng Python (`stdlib/*.py`, ~1.800 dòng)
Được compiler dịch ra LLVM IR y như code người dùng:

| Module | Nội dung |
|---|---|
| `stdlib/re.py` | Regex engine backtracking: parser ra cây list, matcher dùng **chuỗi continuation tường minh** (không cần closure), fast-path một ký tự, lọc quét theo atom đầu, cache pattern. `match/search/fullmatch/findall/finditer/sub/split/escape/compile`, group + backref, cờ `I/M/S`, lớp `Match`/`Pattern` |
| `stdlib/json.py` | `loads/dumps/load/dump` + `indent=`, `sort_keys=`, escape `\uXXXX` (kể cả surrogate pair) |
| `stdlib/logging.py` | Level, `basicConfig(level/format/datefmt/filename)`, `getLogger`, `Logger`, `%`-format (uỷ quyền phần số cho builtin `format()`), `%(asctime)s` |
| `stdlib/os.py`, `stdlib/os/path.py` | `getcwd/chdir/listdir/mkdir/makedirs/rmdir/remove/rename/system/getenv/getpid/read/write/close/lseek` · `join/split/basename/dirname/splitext/normpath/abspath/isabs/exists/isfile/isdir/getsize/expanduser` |
| `stdlib/socket.py` | Lớp `socket` Python trên một fd integer: `bind/listen/accept/connect/send/sendall/recv/recv_all/settimeout/fileno/close` |
| `stdlib/requests.py` | HTTP/1.1 **viết bằng Python trên `socket.py`**: `get/post/put/delete/head/request`, `Response` (`status_code/ok/text/headers/json()/raise_for_status`), chunked transfer-encoding, `params=`, `json=` |
| `stdlib/string.py` | Hằng số + `capwords` |
| `stdlib/itertools.py`, `stdlib/functools.py` | Chuyển từ `runtime/pylib/` (v0.5.1) sang `stdlib/` — nay được namespace hoá như mọi module khác |

`from X import *` cũng hoạt động đúng với module đã dịch: mọi tên cấp module được đưa vào
scope dưới đúng tên gốc (trước đây là no-op).

### Added — `_kami`: tầng C-ABI binding tới OS (`runtime/src/syscalls.cpp`)
29 primitive, mỗi cái là một forward trực tiếp tới hàm C tiêu chuẩn — không có logic module:
`fd_open/fd_read/fd_write/fd_close/fd_seek` (open/read/write/close/lseek),
`stat/filesize/listdir/mkdir/rmdir/unlink/rename/getcwd/chdir` (stat/opendir/readdir/…),
`getenv/system/getpid/errmsg/platform/localtime`,
`sock_open/sock_connect/sock_bind/sock_listen/sock_accept/sock_send/sock_recv/sock_close/sock_timeout`
(socket/connect/bind/listen/accept/send/recv/setsockopt). Các call blocking nhả GIL.

Ranh giới rõ ràng: cần struct mà ngôn ngữ không diễn đạt được (`sockaddr`, `struct stat`,
`DIR*`) → binding; còn lại → Python.

### Added — import thật sự có namespace (`compiler/src/modules.cpp`)
- Tầng import mới: tìm nguồn (`<thư mục input>` → `$KAMIPY_STDLIB` → `<kamipy>/../stdlib`),
  parse, **đổi tên mọi binding cấp module thành namespace** (`match` trong `re.py` →
  `re__match`), rồi ghép theo thứ tự dependency.
- Bộ đổi tên hiểu scope: local/tham số che tên module (không đổi), `global x` trỏ lại
  global đã namespace hoá, target của comprehension và lambda được xử lý đúng.
- Nhờ đó **user code không còn xung đột với nội bộ module**: chương trình tự định nghĩa
  `def match()`, `def get()`, `class Match` vẫn `import re`/`import requests` bình thường
  (trước đây lỗi "function redefined"). Test mới `feat_module_ns.py`.
- `from m import x as y` hoạt động với module Python; `os.path` là submodule thật.
- **Chẩn đoán**: mỗi module chiếm một khoảng số dòng riêng nên lỗi trong mã đã dịch báo
  đúng `stdlib/re.py:57: error: ...`; tên mangled được dịch ngược khi in ra
  (`re.findall() takes 3 argument(s)`).
- Thay `runtime/pylib/` + `$KAMIPY_PYLIB` (v0.5.1) bằng một cơ chế duy nhất: `stdlib/` +
  `$KAMIPY_STDLIB`, copy cạnh binary sau mỗi lần build, `install()` vào
  `lib/kamipy/stdlib/`.

### Fixed — bug rooting của GC trong runtime (do stdlib Python phơi ra)
`out->tag = KT_STR` được ghi **trước** `str_new()`: nếu chính lần cấp phát đó kích hoạt
collect, GC thấy slot mang tag con trỏ nhưng payload vẫn là giá trị cũ (thường là int) và
dereference rác → segfault/heap corruption. Sửa **24 vị trí** trong `ops.cpp`,
`builtins.cpp`, `objects.cpp`, `syscalls.cpp` theo đúng thứ tự "cấp phát trước, publish
sau" qua helper `put_str()`.
- Thêm `KAMIPY_GC_STRESS=1`: collect trước **mỗi** lần cấp phát → giá trị không được root
  hoá sẽ lỗi ngay lập tức, tái lập được. Thêm test CTest `integration.gc_stress`: chạy lại
  **toàn bộ** 50 chương trình integration ở chế độ này.
- Cùng cách đó tìm ra hai bug rooting nữa: `set("chuỗi")` truyền một slot **chưa root hoá**
  cho `kami_iter_prep` (hàm này cấp phát một object cho mỗi ký tự), và `as_list_pinned()`
  đăng ký pin **sau** khi vật chất hoá list. Cả hai nay pin trước rồi mới cấp phát.
- Engine cũ crash `malloc(): unsorted double linked list corrupted` khi
  `re.findall(r"(\w+)@(\w+)\.com", ...)` chạy lặp — bug này biến mất cùng đoạn C++.

### Added — `int(x, base)`
Builtin `int()` nhận base 2..36 (`int("ff", 16)`) như CPython.

### Tests
5 chương trình integration mới, output **so khớp từng byte với CPython** (trừ test HTTP
dùng server/client nội bộ):
`feat_re_engine.py` (regex: lazy/flags/backref/quantifier/compile/finditer/split),
`feat_stdlib_py.py` (json indent/sort_keys/unicode + os/os.path/string + file system),
`feat_logging_py.py` (level, `%`-format, `getLogger`, ghi ra file rồi đọc lại),
`feat_http_py.py` (server HTTP viết bằng KamiPython + client `requests` của KamiPython
trong cùng binary, GET/POST JSON/404/lỗi kết nối), `feat_module_ns.py` (namespace).
Tổng: 42 integration + 2 unit + 1 GC-stress pass, ASan/UBSan clean.

### Performance / kích thước
| | v0.5 | v0.6 |
|---|---|---|
| binary hello-world | 306 KB | **199 KB** (−35%) |
| `libkamirt.a` | 577 KB | **379 KB** |
| `re.findall(r"\d+")` trên 400 KB text ×50 | 63 ms (C++) | 873 ms (Python đã dịch; CPython chạy *cùng* engine: 765 ms, engine C của CPython: 20 ms) |
| compile `import re` + 1 dòng | — | 0,55 s |

Regex chậm hơn là giá phải trả trung thực của nguyên tắc "chỉ dịch"; đổi lại là ngữ nghĩa
khớp CPython, không còn bug bộ nhớ, và stdlib sửa được bằng Python.

### Breaking
- `requests` **không còn hỗ trợ `https://`** (client cũ thừa hưởng TLS từ `curl`; client
  Python cần binding TLS, chưa có) — nay báo lỗi rõ ràng thay vì âm thầm hạ cấp.
- `socket.socket()` trả về **instance class Python** thay cho kiểu heap `KT_SOCKET`;
  `s.accept()` trả `(conn, (host, port))` như CPython.
- `logging.debug/info/...` nhận tối đa **4 tham số format** (chưa có `*args`).
- Cần `stdlib/` đi kèm binary (build tree tự tìm `../stdlib`; hoặc đặt `$KAMIPY_STDLIB`).

---

## [v0.5.1] — 2026-08-01 — `requests` native 100%: bỏ hẳn curl + Windows UX

Theo đúng triết lý "dịch cả thư viện, không gọi process ngoài": `requests` được viết lại
từ tầng socket ngay trong runtime — hết mọi phụ thuộc curl.

### Added — thư viện chuẩn viết bằng Python, bundle khi import (`runtime/pylib/`)
Đúng tinh thần "dịch cả thư viện, không code ở tầng C++": các module stdlib thuần thuật
toán được viết bằng **Python thật** trong `runtime/pylib/`, ship cạnh binary `kamipy`, và
được chính pipeline compile khi chương trình `import`.
- `itertools`: count, repeat, chain, accumulate, combinations, permutations,
  combinations_with_replacement, product (a,b / repeat=), pairwise, zip_longest, starmap,
  islice — bản eager (trả list).
- `functools`: reduce.
- File `X.py` cục bộ vẫn ưu tiên hơn stdlib bundle (đúng semantics CPython). Đặt biến môi
  trường `KAMIPY_PYLIB` để trỏ tới thư mục pylib tùy chỉnh.

### Added — mở rộng stdlib native & phương thức
- **Sequence repetition**: `[0]*n`, `n*[x]`, `"ab"*n` (cả hai chiều toán hạng).
- **math**: factorial, gcd, isqrt, hypot, log(x, base), log2/log10, atan/asin/acos/atan2,
  degrees/radians, trunc, isnan/isinf; hằng `tau`, `nan`.
- **random**: randrange, choice, shuffle, uniform, sample, choices; `seed()` không tham số.
- **time**: monotonic, perf_counter.
- **os**: walk, chdir, getpid, urandom; **os.path**: expanduser, splitext, split, isabs;
  hằng `os.name/sep/linesep`, `sys.maxsize/platform`, `socket.AF_INET/...`,
  `string.printable/hexdigits/octdigits`.
- **str**: casefold, swapcase, index, rfind, ljust/rjust/center, removeprefix/removesuffix,
  isalnum/isnumeric, **`format()`** (`{}`, `{0}`, format spec `{:.2f}` / `{:05d}`).
- **list.clear**; **set**: union/intersection/difference/symmetric_difference (+ biến thể
  `_update`), isdisjoint, issubset/issuperset, copy, pop.
- **int(x, base)** và `int()/float()` chịu được khoảng trắng đầu/cuối.

### Added — cú pháp
- **for/while ... else** (chạy `else` khi vòng lặp kết thúc không qua `break`).
- **Ellipsis `...`** (type stub `tuple[int, ...]`, thân stub `def f(): ...`) — mô hình hoá None.
- **bytes literal `b"..."`** — nhận như str (runtime str lưu byte bất kỳ).
- **`__file__`** → đường dẫn tuyệt đối của file input.
- **Relative imports** `from .mod import x` / `from . import mod` → bundle file `.py` cùng thư mục;
  **`from X import *`** là no-op cho module bundle.
- **import list nhiều dòng/ngoặc** `from m import (a, b, c)`.
- `doctest.testmod(verbose=...)` nhận & bỏ qua kwargs.

### Added — `requests` native (bỏ hẳn curl)
- **HTTP client viết lại từ tầng socket trong runtime** (`runtime/src/http_client.cpp`) —
  không sinh process ngoài, đúng triết lý "dịch cả thư viện":
  - Windows: **WinHTTP** (system API; TLS qua SChannel, proxy hệ thống + redirect tự động).
  - POSIX: raw TCP socket + **OpenSSL** cho https (tùy chọn lúc build; http thuần không cần),
    tự xử lý `Transfer-Encoding: chunked`, `Content-Length`, redirect (301/302/303/307/308,
    tối đa 5 hop, POST→GET như python-requests), timeout (connect + send + recv),
    proxy qua `http_proxy`/`https_proxy` (CONNECT tunnel cho https).
- Lỗi mạng chỉ surface qua `RequestException` — không còn rác stderr
  (`curl: option -w: requires parameter`, `curl: (7) ...`).

### Added — default parameter values là biểu thức bất kỳ
- `def send(msg, thread_type=ThreadType.USER, retries=BASE + 5)` — không còn lỗi
  "default parameter values must be literals". Default được codegen **đánh giá trong
  prologue của hàm** (scope định nghĩa) khi caller bỏ qua tham số; kwargs bỏ qua tham số
  giữa chỉ được phép khi default là literal (báo lỗi rõ nếu không).

### Fixed — Windows UX
- **Console UTF-8**: `SetConsoleOutputCP(CP_UTF8)` lúc khởi động runtime — tiếng Việt
  in ra đúng thay vì `Kß╗₧I CHß║áY...`.
- **`kamipy` chạy từ thư mục bất kỳ**: dùng `GetModuleFileName` thay vì `argv[0]` để
  tìm `kamirt.lib` cạnh binary (hết lỗi "cannot find kamirt.lib" khi kamipy nằm trong PATH).

### Fixed — Windows / MSVC
- **`error C2177: constant too big`** (`sema.cpp`, `math.inf`): thay literal `1e999` bằng
  `std::numeric_limits<double>::infinity()`. Đây là lỗi chặn toàn bộ build `kamipy_core`
  trên MSVC → `kamipy.exe` không bao giờ được tạo lại.
- **`/EHsc` → `/EHs`** trong CMake: runtime ném exception C++ xuyên qua ranh giới
  `extern "C"` (`kami_raise` → `kami_try`). Với `/EHc`, MSVC coi hàm `extern "C"` là
  nothrow và **có thể xoá luôn catch handler** → `try/except` crash lúc chạy trên Windows.
  Sửa luôn 2 warning C4297 (`kami_raise`, `kami_rethrow`).

### Verification (v0.5.1)
- Integration **PASS toàn bộ** (suite mới: `feat_requests_native` — HTTP server viết bằng
  KamiPython phục vụ chính client `requests` native; `feat_default_exprs`, `feat_stdlib2`,
  `feat_syntax2`, `feat_pylib` — byte-identical CPython 3.11).
- `requests` native kiểm chứng thật: GET/POST + JSON echo + chunked + redirect qua server
  local; **HTTPS thật tới example.com** (TLS verify, status 200) và qua proxy CONNECT tunnel.
- Corpus 2.182 file GitHub: build **1059 → ~1190**, chạy **748 → ~900** (đo bằng
  `tools/corpus_survey.sh`).

---

## [v0.5.0] — 2026-08-01 — Comprehensions++, insertion-ordered dicts, unpacking, PEP 695 syntax

Nhóm cú pháp còn thiếu hay gặp trong corpus. Đo lại trên 2.182 file GitHub:

| Mốc | Build được | Chạy được |
|---|---|---|
| v0.4.0 | 1015 | 748 |
| **v0.5.0** | **1059 (49%)** | **784** |

### Added — comprehensions
- **Dict comprehension** `{k: v for ...}`, **set comprehension** `{e for ...}`.
- **Nested comprehensions**: nhiều mệnh đề `for` và nhiều `if`
  (`[x*y for x in A for y in B if cond]`, `[c for row in grid for c in row]`).
- Comprehension tổng quát hoá thành chuỗi clause (mỗi clause có targets + iter + conds),
  hỗ trợ unpack target (`{v: k for k, v in d.items()}`).

### Added — insertion-ordered dict/set (compact dict kiểu CPython)
Refactor `KamiMap`: entries lưu **theo thứ tự chèn** + bảng băm index (entry_index+1).
Duyệt/print/`keys()/values()/items()`/`for k in d` giờ theo thứ tự chèn — **khớp CPython**.
Xoá key giữ nguyên thứ tự phần còn lại; cập nhật key giữ nguyên vị trí; re-insert sau khi xoá
đi về cuối. Kiểm chứng stress (grow/delete/re-insert 1000 phần tử) byte-identical CPython, ASan clean.

### Added — unpacking & syntax
- **Starred trong list literal**: `[0, *a, 4]`, `[*quick_sort(lo), pivot, *quick_sort(hi)]`,
  `[*range(x), 99]`.
- **Tuple-target assignment**: `(a, b) = f()`, `(x, y, z) = [1, 2, 3]`.
- **Non-literal default parameters**: `def f(n=SIZE*2)` (đánh giá tại def-time trong prologue).
- **PEP 695 generics syntax**: `class Box[T]:`, `def identity[T](x):` — cú pháp `[...]` được
  bỏ qua (KamiPython là ngôn ngữ động, không dùng type params).
- **Class base là biểu thức** (`class T(unittest.TestCase)`, `class W(tk.Tk)`): parse được;
  chỉ mô hình hoá kế thừa từ class định nghĩa trong file, base ngoài được bỏ qua (tạo class rỗng).

### Verification
- Integration **37/37 PASS** (4 suite mới: `feat_comprehensions`, `feat_dict_order`,
  `feat_unpacking`, `feat_generics` — 3/4 byte-identical CPython 3.11; generics test
  không cross-check được vì CPython 3.11 chưa có PEP 695).
- ASan CLEAN trên dict-order stress + comprehensions + gc_stress; rebuild sạch 0 error/0 warning.
- Corpus: build 1015→**1059**, chạy 748→**784**.

### Known limitations
- Set duyệt theo thứ tự băm (CPython cũng không đảm bảo thứ tự set) → test set dùng `sorted`.
- Tuple-target lồng nhau `(a, (b, c)) = ...` chưa hỗ trợ (báo lỗi rõ).
- PEP 695 type params chỉ là cú pháp (bỏ qua), không kiểm tra kiểu.

---

## [v0.4.0] — 2026-08-01 — Functional Python: lambda, closures, decorators, map/filter + regex (`re`)

Bổ sung lớp "functional" của Python và module `re` — nhóm lỗi lớn tiếp theo trong corpus.
Đo lại trên 2.182 file GitHub:

| Mốc | Build được | Chạy được |
|---|---|---|
| v0.3.0 | 975 | 731 |
| **v0.4.0** | **1015 (47%)** | **748** |

### Added — ngôn ngữ
- **lambda**: `lambda x: expr`, dùng trực tiếp `sorted(xs, key=lambda p: p[1])`,
  `map(lambda x: x*x, xs)`. Biên dịch thành hàm tổng hợp + closure.
- **Nested functions / closures**: `def inner()` bên trong `def outer()`; **bắt biến
  enclosing theo giá trị** (capture-by-value) — `make_adder(n)` → `add(x): return x+n`
  hoạt động. Hỗ trợ bắt qua 1 cấp lồng, và 2 cấp nếu cấp giữa cũng bắt biến đó.
- **Decorators**: `@deco` trên hàm/lớp cấp module (`name = deco(name)`), xếp chồng nhiều
  decorator (`@a\n@b`). Lời gọi hàm đã decorate đi qua giá trị global (đúng ngữ nghĩa).
  Method decorator: nhận diện `@staticmethod/@classmethod/@property/@abstractmethod`
  (báo lỗi rõ ràng vì chưa hỗ trợ ngữ nghĩa đầy đủ).
- **First-class functions** hoàn chỉnh: builtins + hàm người dùng + closure đều là giá trị
  gọi được, truyền vào `map/filter/sorted(key=)`.
- Calling convention thêm tham số `captures` (ẩn) — hàm thường bỏ qua, closure đọc biến bắt.

### Added — module `re` (regex engine tự viết, `runtime/src/kami_regex.cpp`)
Backtracking matcher (CPS) hỗ trợ: literal, `. * + ? {m,n}` (greedy + lazy `?`),
lớp ký tự `[...]`/`[^...]` + range, neo `^ $`, nhóm `(...)`/non-capturing `(?:...)`,
alternation `|`, escapes `\d \D \w \W \s \S \b \B` + `\n\t\r`.
Hàm: `re.match/search/fullmatch/findall/sub/split`; Match object `.group([n])/.groups()/
.start([n])/.end([n])/.span()`. Byte-identical với CPython (trừ findall nhiều nhóm hiển thị
list thay vì tuple — theo quy ước tuple≈list đã ghi).

### Added — builtins
`map`, `filter` (eager, trả list); nhận iterable bất kỳ.

### Fixed
- Lời gọi trực tiếp tới hàm đã decorate bị bỏ qua wrapper → chuyển sang gọi động qua global.
- `re` matcher: sửa đệ quy template vô hạn (dùng `std::function` cho continuation).

### Verification
- Integration **33/33 PASS** (2 suite mới: `feat_closures`, `feat_regex` — **byte-identical
  CPython 3.11**).
- ASan CLEAN trên closures + regex; rebuild sạch 0 error/0 warning.
- Corpus 2.182 file: build 975→**1015**, chạy 731→**748**.

### Known limitations
- Closure bắt biến **theo giá trị lúc tạo** (snapshot) — không phản ánh thay đổi biến enclosing
  sau khi tạo closure (khác Python; đủ cho sort key/callback thông thường). Không có `nonlocal`.
- `@staticmethod/@classmethod/@property` chưa hỗ trợ (báo lỗi rõ).
- `re`: chưa có flags (IGNORECASE...), named groups `(?P<>)`, lookahead/lookbehind, backref
  trong pattern.

---

## [v0.3.0] — 2026-08-01 — stdlib thực tế: file I/O, os, json, logging, socket, requests + with/set/del

Tiếp tục từ bug report (`try: import ...`, `logging.basicConfig`), bổ sung các module và cú pháp
mà chương trình thật cần để **thực sự chạy được** (mở file, mở socket, HTTP, JSON, logging).
Đo lại trên corpus 2.182 file GitHub:

| Mốc | Build được | Chạy được |
|---|---|---|
| v0.2.0 | 914 | 704 |
| **v0.3.0** | **975 (45%)** | **731** |

### Added — I/O & mạng (viết mới `runtime/src/netio.cpp`)
- **File I/O**: `open(path, mode)`, `.read([n])`, `.readline()`, `.readlines()`, `.write()`,
  `.flush()`, `.close()`; lặp `for line in f`. GC tự đóng file rò rỉ.
- **`with ... as ...:`** (context manager) — nhiều manager một dòng; thân được outline + bảo vệ
  bằng unwinding nên `__exit__`/`close()` luôn chạy kể cả khi có exception; hỗ trợ `__enter__`/
  `__exit__` cho object người dùng, và tự đóng file.
- **`socket`**: `socket()`, `.bind/.listen/.accept/.connect/.send/.sendall/.recv/.close/
  .settimeout` (TCP/IPv4). Kiểm chứng: **server + client TCP thật qua 2 thread** (echo) —
  ASan/TSan clean. (Windows: link `ws2_32`.)
- **`requests`**: `requests.get(url, timeout=)`, `requests.post(url, json=/data=, timeout=)`
  (qua `curl`, hỗ trợ http + https); trả về `Response` với `.status_code`, `.ok`, `.text`,
  `.json()`. Kiểm chứng: **HTTP server viết bằng KamiPython phục vụ chính client requests của
  KamiPython**, parse JSON — tất cả native.
- **`json`**: `json.loads` (parser đầy đủ: object/array/string/number/bool/null, `\uXXXX`,
  UTF-8), `json.dumps`. Round-trip byte-identical với CPython.
- **`os` / `os.path`**: `getcwd listdir remove mkdir makedirs rmdir rename system getenv`;
  `exists isfile isdir join basename dirname getsize abspath`.
- **`logging`**: `basicConfig(level=, format=)` với `%(asctime)s/%(levelname)s/%(message)s/
  %(name)s`; `debug/info/warning/error/critical/exception` với %-format args
  (`logging.info("x=%s", v)`); hằng `logging.INFO/DEBUG/...`.

### Added — kiểu & cú pháp
- **`set`**: literal `{1, 2, 3}`, `set(iterable)`, `.add/.remove/.discard/.clear`, `in`,
  phép `& | ^ -` (giao/hợp/hiệu đối xứng/hiệu), lặp, `len`.
- **`del`**: `del d[k]`, `del xs[i]`, `del name`.
- **First-class builtins**: dùng builtin như giá trị (`sorted(xs, key=len)`, `map`-style),
  runtime có function-object mang `builtin_id`.
- **`sorted(key=, reverse=)`**; các builtin gom iterable (`sorted/sum/min/max/all/any/
  reversed/set`) nhận set/dict/str/file, không chỉ list.
- `for` lặp trực tiếp dict (→ keys), set, và file (→ dòng) qua `kami_iter_prep`.
- Số literal hex/oct/bin/underscore, `logging`/`os.path` submodule dispatch.

### Fixed
- Cập nhật thông báo lỗi "unknown module" liệt kê đủ module hỗ trợ.
- `%`-format C-style cho logging/`"%s" % x` (qua logging path).

### Verification
- Integration **31/31 PASS** (3 suite mới: `feat_files_sets`, `feat_stdlib`, `feat_socket`;
  files/sets/stdlib **byte-identical CPython 3.11**; socket deterministic).
- ASan CLEAN: files/sets, stdlib, socket, exceptions, classes; **TSan CLEAN**: socket+threads.
- Bug report demo (`auto_pull.py`: guarded imports + logging.basicConfig + os.path +
  requests loop + `__name__`) build & chạy. HTTP server+client demo native hoạt động.
- Rebuild sạch từ đầu: 0 error, 0 warning.

### Known limitations / Giới hạn
- `requests` cần `curl` trên PATH (runtime dependency có chủ đích cho HTTPS).
- `socket.settimeout` chấp nhận nhưng vẫn blocking; chỉ TCP/IPv4.
- `logging.getLogger(...).info(...)` (logger object) chưa hỗ trợ — dùng `logging.info(...)`.
- Chưa hỗ trợ: `re`, `numpy`, decorators, lambda, generators, nested functions, `*args/**kwargs`,
  relative imports.

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
