# Changelog — KamiPython

Tất cả thay đổi đáng chú ý của project được ghi tại đây. / All notable changes to this project are documented here.

---

## [v0.6.1] — 2026-08-01 — "Chỉ dịch, không tự viết": stdlib chuyển sang Python thật

Đợt refactor lớn nhất từ đầu project. Nguyên tắc: **KamiPython là trình biên dịch, không
phải là một bản cài lại thư viện chuẩn bằng C++.** Mọi thứ có logic/nghiệp vụ (regex, JSON,
logging, HTTP) được viết bằng **Python thật** trong `runtime/pylib/` và được chính pipeline
`lex → parse → sema → codegen → LLVM IR` dịch ra mã máy. C++ runtime chỉ còn giữ đúng ba
việc: **Value model, GC, và binding C-ABI xuống syscall của OS.**

### Removed — 1.400+ dòng C++ thủ công bị xoá
| File bị xoá | Dòng | Thay bằng |
|---|---|---|
| `runtime/src/kami_regex.cpp` + `.h` | 411 | `runtime/pylib/re.py` (engine Python thuần) |
| `runtime/src/http_client.cpp` | 562 | `runtime/pylib/requests.py` + `socket.wrap_tls()` |
| `runtime/src/netio.cpp` (JSON + logging + HTTP shaping) | ~480 | `runtime/pylib/json.py`, `runtime/pylib/logging.py` |
Phần còn lại của `netio.cpp` (os / os.path) tách thành `runtime/src/oslayer.cpp`; socket +
TLS tách thành `runtime/src/netsock.cpp`. Các class C++ "nội bộ" `Match` và `Response` — trước
đây được dựng tay trong runtime và dispatch method bằng `strcmp` — đã biến mất hoàn toàn.

### Added — stdlib bằng Python (`runtime/pylib/`)
- **`re`**: engine backtracking hai tầng (parser → VM). Hỗ trợ `. ^ $ [] () (?:) (?P<n>)
  (?i) | * + ? {m,n}` (kèm biến thể non-greedy), `\d \D \w \W \s \S \b \B \A \Z`,
  backreference `\1`, cờ `I/M/S/X`; API `compile/match/search/fullmatch/findall/finditer/
  sub/subn/split/escape/purge`, `Match.group/groups/groupdict/start/end/span/expand`.
  VM dùng **trail stack tường minh** nên không đệ quy — khớp được input dài (đã test 20 000
  ký tự) và pattern bệnh lý (`(a*)*b`, `(|a)*b`) vẫn dừng.
- **`json`**: `loads/load/dumps/dump` (+ `indent`, `sort_keys`), unicode escape & surrogate
  pair, thông báo lỗi có vị trí ký tự.
- **`logging`**: `basicConfig` (level/format/datefmt/stream/filename), `getLogger`, `Logger`,
  `debug/info/warning/error/critical/exception/log`, `%(asctime)s %(levelname)s %(name)s
  %(message)s %(levelno)s %(process)s`, ghi ra stderr như CPython.
- **`requests`**: HTTP/1.1 thuần Python trên socket — parse URL, dựng request, đọc header,
  giải mã `Transfer-Encoding: chunked`, theo redirect, `params=/data=/json=/headers=/auth=/
  timeout=`, `Response.status_code/ok/text/content/headers/reason/url/history/json()/
  raise_for_status()`, `Session`, `urlparse/quote/urlencode`.

### Added — TLS là binding OS, không phải HTTP client tự chế
- `socket.wrap_tls(host)`: đưa socket đã connect cho **TLS stack của hệ điều hành** —
  OpenSSL trên POSIX, **SChannel** trên Windows (`secur32`/`crypt32`, không còn WinHTTP).
  Đây chính là việc của module `ssl` trong CPython.
- `socket.settimeout()` giờ **thật sự** đặt `SO_RCVTIMEO/SO_SNDTIMEO` (trước đây bị bỏ qua);
  thêm `gettimeout`, `fileno`, `tls_available`.

### Added — Name mangling cho module stdlib
Module trong `pylib/` được bundle vào chương trình, nên tên top-level của nó từng dùng chung
namespace toàn cục với code người dùng — `logging.py` định nghĩa `error`, `info`, `root`, và
một biến `error = ...` của user sẽ ghi đè thư viện. Nay mỗi binding top-level được đổi tên
theo module (`re.search` → `std_re_search`, `logging.info` → `std_logging_info`); sema map
`mod.attr` và `from mod import x as y` về đúng symbol đã mangle. Tên method **không** bị đổi
(`m.group()` vẫn là `group`).

### Added — cú pháp & ngữ nghĩa
- **Nối chuỗi ngầm qua f-string**: `f"{a}\n" "tail"` (lỗi `expected ')'` khi viết block
  chuỗi nhiều dòng — hay gặp nhất trong code thật — đã hết).
- **`*args`** trong `def`/method (kèm `min_arity` đúng, sema kiểm tra số tham số).
- **Toán tử `%`** trên str: `"%s=%d" % (a, b)`, `"%(k)s" % {...}`, cờ/width/precision.
- **`__str__` / `__repr__`** được `print()`, `str()`, `repr()`, f-string gọi đến.
- **So sánh dict/set theo giá trị** (`{"a": 1} == {"a": 1}` trước đây trả `False`).
- **`threading.Lock()` / `RLock()`**: mutex OS thật, `acquire(blocking, timeout)`, `release`,
  `locked`, dùng được với `with`.
- **`repr()`**, **`callable()`**, **`sys.stdout` / `sys.stderr`**, **`time.localtime` /
  `gmtime` / `strftime`** (binding libc), **`dict.setdefault`**, **`list(dict)`**,
  **`str.strip/lstrip/rstrip(chars)`**, **`splitlines`**, **`partition/rpartition`**,
  **`find/index/rfind/rindex`** với cửa sổ start/end.
- **Package cục bộ**: `import pkg` tìm cả `pkg/__init__.py`.

### Fixed
- **`with` bên trong vòng lặp/hàm không gọi `__exit__`.** Slot frame "persistent" của
  `with` được cấp phát tại `temps_base + max_temps`, nhưng `max_temps` còn tăng sau đó nên
  biến tạm ghi đè lên context manager. Nghĩa là `with open(...)` trong `while` **không bao
  giờ đóng file**. Nay index slot được emit dưới dạng placeholder và patch ở `finish()`.
- `kami_class_add_method` không khởi tạo `builtin_id`, nên gọi một method qua
  `kami_call_value` nhảy vào bảng builtin với id rác (crash).
- `sys.stdout`/`sys.stderr` không bị `close()` bởi GC hay `with`.
- Thông báo lỗi import: liệt kê đúng module đang có (kể cả `pylib/`) và nói rõ phải làm gì
  với package bên thứ ba (`pyautogui`, `numpy`, `flask` — cần C extension của CPython).

### Notes
- `re`/`json`/`logging`/`requests` giờ cần thư mục `pylib/` cạnh binary `kamipy`
  (CMake copy tự động), hoặc source thật của CPython trên máy — xem cơ chế resolve
  `project → native → system Python → pylib` của v0.6.0.
- Không có OpenSSL khi build POSIX → `https://` báo `SSLError` rõ ràng, `http://` vẫn chạy.
- v0.6.0 để `json`/`re`/`logging`/`requests` ở đường **native**; bản này bỏ các entry đó khỏi
  bảng native, nên chúng resolve xuống `pylib/` (hoặc system Python) và được **compile từ
  source Python**. Đồng thời v0.6.0 xoá `netio.cpp`/`http_client.cpp` nhưng `ops.cpp` vẫn tham
  chiếu `socket_method`/`json_loads` khiến **mọi** chương trình fail ở bước link — bản này khôi
  phục các primitive đó trong `oslayer.cpp`/`netsock.cpp` và sửa `ops.cpp`.

---

## [v0.6.0] — 2026-08-01 — Tự động tìm Python hệ thống + gắn kết C-ABI (C extensions, ctypes/cffi)

Trước bản này, `import X` chỉ tìm được `X.py` cạnh file input hoặc trong `runtime/pylib/`
ship kèm binary — muốn dùng thêm module stdlib là phải copy tay. Bản này bỏ hẳn ràng buộc đó:
compiler tự dò môi trường Python trên máy và **compile chính source stdlib thật của CPython**.

### Added — Auto-discovery môi trường Python hệ thống (`compiler/src/modules.cpp`)
Hàm `find_system_python_stdlib()` trả về danh sách import root, tốt nhất trước, dò theo thứ tự:
1. `KAMIPY_PYTHON_STDLIB` (ghi đè tường minh, phân tách `:` / `;`).
2. `PYTHONHOME` → `<prefix>/Lib` (Windows) hoặc `<prefix>/lib/python3.*`.
3. Từng entry của `PYTHONPATH`.
4. Trình thông dịch `python3`/`python.exe` trên `PATH` — nhờ đó virtualenv, pyenv, conda đều được nhận.
5. Quét theo nền tảng:
   - **Windows**: Registry `HKCU`/`HKLM\Software\Python\PythonCore` (kể cả `Wow6432Node`) →
     `InstallPath`; `%LOCALAPPDATA%\Programs\Python\Python3*\Lib`; `C:\Python3*\Lib`;
     `C:\Program Files\Python3*\Lib` và `Program Files (x86)`.
   - **Linux/macOS**: `/usr/lib/python3.*`, `/usr/local/lib/python3.*`, `/usr/lib64/...`,
     `/opt/homebrew/...`, `~/.pyenv/versions/*/lib/python3.*`, framework build của macOS.

Mỗi thư mục phải chứa "dấu vân tay" stdlib (`os.py` / `json/__init__.py` / `types.py`) mới được
nhận, sắp xếp nguồn tường minh trước nguồn quét, Python mới trước Python cũ. Kết quả được cache.

### Changed — thứ tự resolve import (`resolve_module`)
`project → native → system Python → pylib đi kèm`:
- File `X.py` cạnh input **luôn** thắng (đúng semantics CPython).
- Module runtime đã có primitive C-ABI (`math`, `os`, `json`, `socket`, `re`, …) giữ nguyên đường
  native — nhanh hơn và đã được kiểm chứng, không kéo cả package CPython vào.
- Còn lại: nạp `.py` **thật** từ Python hệ thống. Hỗ trợ cả package (`X/__init__.py`) và `pkg.mod`.
- `pylib` đi kèm trở thành **fallback thật**: nếu máy không có Python, hoặc source CPython dùng
  cú pháp compiler chưa nhận (`*args/**kwargs`, nested class, …), quá trình tự rơi xuống bản
  `runtime/pylib/`. Chỉ khi hết lựa chọn mới cảnh báo và bỏ qua module đó.

### Added — Tích hợp C extension & C-ABI (`compiler/src/cext.cpp`, `runtime/src/syscalls.cpp`)
Nửa "C" của stdlib CPython nay có chỗ đáp: bảng map từng thành viên C extension sang
**runtime primitive** hoặc **lời gọi C trực tiếp** trong LLVM IR (`call double @sqrt(double)`).
- `_math` → libm trực tiếp (sqrt, sin, cos, tan, exp, log, log2/log10, atan/asin/acos, sinh/cosh/tanh,
  cbrt, expm1, log1p, pow, atan2, fmod, hypot, copysign, remainder) + primitive cho factorial/gcd/isqrt.
- `_os` / `posix` / `nt` → `os.*` primitive, cộng I/O file descriptor **thật**:
  `open/read/write/close/lseek` (syscall), hằng `O_RDONLY/O_WRONLY/O_CREAT/O_TRUNC/O_APPEND`,
  `SEEK_*` — cờ được runtime dịch lại nên cùng một source chạy được cả Windows.
- `_socket` → primitive `socket()`, `gethostname()`, và `htons/ntohs/htonl/ntohl` gọi thẳng
  libc / `ws2_32.dll`; hằng `AF_INET`, `SOCK_STREAM`, …
- `_struct` → `pack`/`unpack`/`calcsize` cài trong `runtime/src/syscalls.cpp`
  (`b B h H i I l L q Q f d x`, tiền tố `< > ! = @`, có dấu/không dấu, đệm `x`).
- `_json` → `json.loads/dumps` primitive; `_time`, `_random` → primitive tương ứng.
- `from _math import sqrt` hoạt động y như `_math.sqrt`.

### Added — `ctypes` / `cffi` → native call + tự động sinh cờ link
- `lib = ctypes.CDLL("libm.so.6")` (và `WinDLL`/`OleDLL`/`PyDLL`/`cdll.LoadLibrary`,
  `CDLL(None)` = chính process) được xử lý **ở compile time**: tên thư viện suy ra cờ linker
  (`libm.so.6` → `-lm`, `ws2_32.dll` → `-lws2_32`, `libc.so.6`/`msvcrt.dll` → không cần gì).
- `lib.f.restype = ctypes.c_double` / `lib.f.argtypes = [...]` dựng chữ ký; thiếu `argtypes` thì
  suy ra từ đối số thực tế (float → `double`, str → `char*`, còn lại → integer).
- `cffi`: `ffi = cffi.FFI()`, `ffi.cdef("double cbrt(double);")`, `ffi = ffi.dlopen(...)` —
  prototype trong `cdef()` được parse để lấy chữ ký chính xác.
- **Độ rộng kiểu được tôn trọng**: C `int` là 32-bit (`trunc`/`sext` quanh lời gọi), `long` theo
  LP64/LLP64, `float` khác `double`. Truyền i64 vào tham số `int` là UB, không phải tiện tay.
- Đối số được unbox qua runtime (`kami_c_arg_i64/f64/cstr`) nên sai kiểu là lỗi Python bắt được,
  không phải hỏng stack.

### Added — CLI & chẩn đoán
- `kamipy paths` — in mọi thư mục mà `import` sẽ tìm, theo thứ tự (project → pylib → Python hệ thống).
- `kamipy build ... -v/--verbose` — in từng module resolve về đâu và cờ link C-ABI nào được thêm.
- `kamipy build ... --no-system-stdlib` (hoặc `KAMIPY_NO_SYSTEM_STDLIB=1`) — tắt auto-discovery.

### Verification
- `import stat` và `import colorsys` compile từ **`/usr/lib/python3.13/*.py` thật**, output
  **trùng từng byte** với CPython (`tests/integration/feat_system_stdlib.py`).
- `import json / math / socket / os` vẫn đi đường native primitive như trước.
- 3 integration test mới (`feat_cext_cabi`, `feat_ctypes_ffi_posix`, `feat_system_stdlib`) và
  1 unit suite mới (`tests/unit/test_cabi.cpp`: suy luận cờ link, bảng kiểu ctypes, parser `cdef()`,
  bảng map C extension, tính bất biến của discovery). Tổng: 3 unit + 48 integration, all green.

### Known limits
Phần lớn stdlib CPython vẫn chưa compile được vì dùng `*args/**kwargs`, nested class, generator —
compiler báo chính xác file:dòng:lý do rồi rơi xuống fallback thay vì làm hỏng cả build.

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
