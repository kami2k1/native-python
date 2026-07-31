# Build KamiPython trên Windows (Visual Studio Community)

> English version: [`docs/en/BUILD_WINDOWS.md`](../en/BUILD_WINDOWS.md)

Hướng dẫn này dùng **Visual Studio Community** (2022 trở lên — áp dụng cho cả các bản mới hơn như 2026) làm toolchain MSVC + CMake, và **LLVM/Clang for Windows** làm backend cho `kamipy`.

## 1. Cài đặt yêu cầu

### Visual Studio Community
1. Tải **Visual Studio Community** (miễn phí): https://visualstudio.microsoft.com/vs/community/
2. Trong **Visual Studio Installer**, chọn workload:
   - ✅ **Desktop development with C++**

   Workload này đã bao gồm: trình biên dịch **MSVC x64**, **CMake**, **Ninja**, và **Windows SDK** — không cần cài CMake riêng.

### LLVM (clang++)
`kamipy` dùng `clang++` làm LLVM backend + linker driver, cần có trên `PATH`:

```powershell
winget install LLVM.LLVM
```

hoặc tải installer từ https://github.com/llvm/llvm-project/releases (chọn `LLVM-*-win64.exe`, tick **"Add LLVM to the system PATH"**).

Kiểm tra (mở terminal mới sau khi cài):
```powershell
clang++ --version    # cần LLVM >= 15
```

### Git
```powershell
winget install Git.Git
```

## 2. Build bằng dòng lệnh (khuyến nghị)

Mở **"x64 Native Tools Command Prompt for VS"** (tìm trong Start Menu — quan trọng: bản **x64**), rồi:

```bat
git clone https://github.com/kami2k1/native-python.git
cd native-python

cmake -B build
cmake --build build --config Release
```

Kết quả trong `build\Release\`:
- `kamipy.exe` — compiler CLI
- `kamirt.lib` — runtime library (phải nằm cạnh `kamipy.exe` — CMake tự lo việc này)

Chạy test:
```bat
ctest --test-dir build -C Release --output-on-failure
```
(Integration test cần `bash` — có sẵn trong Git for Windows.)

## 3. Build bằng Visual Studio IDE

1. Mở Visual Studio → **Open a local folder** → chọn thư mục `native-python` (VS tự nhận `CMakeLists.txt`).
2. Chọn configuration **x64 Release** trên thanh công cụ.
3. **Build → Build All** (Ctrl+Shift+B).
4. Binary nằm trong `out\build\x64-Release\` (hoặc theo `CMakeSettings`/preset của bạn).

## 4. Sử dụng

```bat
build\Release\kamipy.exe build examples\main.py -o app
app.exe
```
Kết quả:
```
30
```

`app.exe` là **native PE executable** — copy sang máy Windows khác chạy được ngay, không cần Python, không cần Visual Studio (chỉ cần VC++ runtime hệ thống, có sẵn trên Windows hiện đại).

Các lệnh khác:
```bat
kamipy run main.py          &rem build vào .kamipy-cache rồi chạy
kamipy clean                &rem xóa cache
kamipy build main.py --emit-llvm    &rem giữ lại file .ll để xem LLVM IR
```

## 5. Biến môi trường (tùy chọn)

| Biến | Ý nghĩa |
|---|---|
| `KAMIPY_CXX` | Đường dẫn clang++ nếu không có trên PATH, ví dụ `C:\Program Files\LLVM\bin\clang++.exe` |
| `KAMIPY_RT_LIB` | Đường dẫn `kamirt.lib` nếu để ở chỗ khác |

## 6. Xử lý sự cố

| Lỗi | Cách xử lý |
|---|---|
| `linking failed` / `'clang++' not found` | Cài LLVM và mở terminal **mới**; hoặc set `KAMIPY_CXX` trỏ tới `clang++.exe` |
| `cannot find kamirt.lib` | Đảm bảo `kamirt.lib` nằm cạnh `kamipy.exe` (build lại bằng CMake), hoặc set `KAMIPY_RT_LIB` |
| `cmake` không nhận diện | Dùng đúng **x64 Native Tools Command Prompt**, hoặc cài workload C++ đầy đủ |
| Output test lệch dòng CRLF | Repo đã có `.gitattributes` ép LF; nếu clone cũ: `git config core.autocrlf false` rồi clone lại |

## 7. CI

Repo có sẵn GitHub Actions (`.github/workflows/ci.yml`) build + test tự động trên cả **ubuntu-24.04** và **windows-latest** (MSVC + LLVM) cho mỗi push/PR.
