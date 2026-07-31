# Building KamiPython on Windows (Visual Studio Community)

> Bản tiếng Việt: [`docs/vi/BUILD_WINDOWS.md`](../vi/BUILD_WINDOWS.md)

This guide uses **Visual Studio Community** (2022 or newer — the same steps apply to later releases such as 2026) as the MSVC + CMake toolchain, and **LLVM/Clang for Windows** as the backend for `kamipy`.

## 1. Prerequisites

### Visual Studio Community
1. Download **Visual Studio Community** (free): https://visualstudio.microsoft.com/vs/community/
2. In the **Visual Studio Installer**, select the workload:
   - ✅ **Desktop development with C++**

   This workload already includes the **MSVC x64** compiler, **CMake**, **Ninja**, and the **Windows SDK** — no separate CMake install needed.

### LLVM (clang++)
`kamipy` uses `clang++` as its LLVM backend + linker driver; it must be on `PATH`:

```powershell
winget install LLVM.LLVM
```

or grab the installer from https://github.com/llvm/llvm-project/releases (pick `LLVM-*-win64.exe` and tick **"Add LLVM to the system PATH"**).

Verify (open a new terminal after installing):
```powershell
clang++ --version    # LLVM >= 15 required
```

### Git
```powershell
winget install Git.Git
```

## 2. Command-line build (recommended)

Open the **"x64 Native Tools Command Prompt for VS"** (find it in the Start Menu — make sure it is the **x64** one), then:

```bat
git clone https://github.com/kami2k1/native-python.git
cd native-python

cmake -B build
cmake --build build --config Release
```

Outputs in `build\Release\`:
- `kamipy.exe` — the compiler CLI
- `kamirt.lib` — the runtime library (must sit next to `kamipy.exe` — CMake takes care of this)

Run the tests:
```bat
ctest --test-dir build -C Release --output-on-failure
```
(The integration suite needs `bash`, which ships with Git for Windows.)

## 3. Building from the Visual Studio IDE

1. Open Visual Studio → **Open a local folder** → pick the `native-python` folder (VS auto-detects `CMakeLists.txt`).
2. Select the **x64 Release** configuration in the toolbar.
3. **Build → Build All** (Ctrl+Shift+B).
4. Binaries land in `out\build\x64-Release\` (or wherever your `CMakeSettings`/preset points).

## 4. Usage

```bat
build\Release\kamipy.exe build examples\main.py -o app
app.exe
```
Result:
```
30
```

`app.exe` is a **native PE executable** — copy it to another Windows machine and it runs as-is: no Python, no Visual Studio required (only the system VC++ runtime, present on modern Windows).

Other commands:
```bat
kamipy run main.py          &rem build into .kamipy-cache, then run
kamipy clean                &rem remove the cache
kamipy build main.py --emit-llvm    &rem keep the .ll file to inspect LLVM IR
```

## 5. Environment variables (optional)

| Variable | Meaning |
|---|---|
| `KAMIPY_CXX` | Path to clang++ if it is not on PATH, e.g. `C:\Program Files\LLVM\bin\clang++.exe` |
| `KAMIPY_RT_LIB` | Path to `kamirt.lib` if you moved it elsewhere |

## 6. Troubleshooting

| Problem | Fix |
|---|---|
| `linking failed` / `'clang++' not found` | Install LLVM and open a **new** terminal; or set `KAMIPY_CXX` to the full `clang++.exe` path |
| `cannot find kamirt.lib` | Make sure `kamirt.lib` sits next to `kamipy.exe` (rebuild with CMake), or set `KAMIPY_RT_LIB` |
| `cmake` not recognized | Use the **x64 Native Tools Command Prompt**, or install the full C++ workload |
| Test outputs differ by CRLF | The repo ships a `.gitattributes` forcing LF; for old clones: `git config core.autocrlf false` and re-clone |

## 7. CI

The repo ships GitHub Actions (`.github/workflows/ci.yml`) that build + test automatically on both **ubuntu-24.04** and **windows-latest** (MSVC + LLVM) for every push/PR.
