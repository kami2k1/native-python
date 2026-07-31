# KamiPython (native-python)

**KamiPython** là một compiler AOT viết bằng **C++20 + LLVM**, biến ngôn ngữ Python-like thành **native executable** — không cần Python, CPython runtime, PyInstaller hay Nuitka.

**KamiPython** is an AOT compiler written in **C++20 + LLVM** that turns a Python-like language into a **native executable** — no Python, CPython runtime, PyInstaller, or Nuitka required.

```python
def main():
    a = 10
    b = 20
    print(a + b)

main()
```

```bash
kamipy build main.py -o app
./app
# → 30
```

## Documentation / Tài liệu

| Tiếng Việt | English |
|---|---|
| [Phân tích kiến trúc](docs/vi/ARCHITECTURE.md) | [Architecture Analysis](docs/en/ARCHITECTURE.md) |

Hướng dẫn build và báo cáo test của từng phase: xem [CHANGELOG.md](CHANGELOG.md).
Build instructions and per-phase test reports: see [CHANGELOG.md](CHANGELOG.md).

## Status / Trạng thái

| Phase | Nội dung / Content | Status |
|---|---|---|
| 0 | Architecture analysis / Phân tích kiến trúc | ✅ Done |
| 1 | CMake project foundation | ⏳ Next |
| 2–15 | Lexer → … → Benchmark → Binary protection | 🔜 Planned |

## Toolchain

- C++20, CMake ≥ 3.20
- LLVM 18 (codegen backend)
- Targets: Linux x64, Windows x64
