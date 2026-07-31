# Changelog — KamiPython

Tất cả thay đổi đáng chú ý của project được ghi tại đây. / All notable changes to this project are documented here.

Format dựa trên [Keep a Changelog](https://keepachangelog.com/), phiên bản theo phase.

---

## [Phase 0] — 2026-08-01 — Architecture Analysis / Phân tích kiến trúc

### Added / Thêm mới
- `docs/vi/ARCHITECTURE.md` — Tài liệu phân tích kiến trúc chi tiết (tiếng Việt): pipeline compiler, thiết kế Lexer/Parser/AST/Semantic/KIR/Codegen/Linker, runtime object system (`Value` 16-byte tagged union), memory model (Arena cho compiler, Reference Counting cho runtime), giải thích binary generation `.py → AST → KIR → LLVM IR → Machine Code → Executable`, import system, CLI, chiến lược test, roadmap 16 phase kèm exit criteria.
- `docs/en/ARCHITECTURE.md` — Full English translation of the architecture analysis.
- `CHANGELOG.md` — file này / this file.
- `README.md` — cập nhật giới thiệu project, trạng thái phase, link tài liệu / project intro, phase status, doc links.

### Design decisions / Quyết định thiết kế
| # | Quyết định / Decision | Lý do / Rationale |
|---|---|---|
| 1 | Hand-written lexer + recursive descent/Pratt parser | Kiểm soát indentation (INDENT/DEDENT), báo lỗi đẹp / full control over indentation, better diagnostics |
| 2 | Mid-level IR riêng (KIR) trước LLVM IR | Tự tối ưu (folding/DCE/inline) trước khi boxing bị vật chất hoá / optimize before boxing is materialized |
| 3 | `Value` = 16-byte tagged union (không NaN-boxing) | Đơn giản, dễ debug; NaN-boxing hoãn đến khi có benchmark / simple, debuggable; defer NaN-boxing |
| 4 | Reference counting cho runtime GC (v0.1) | Deterministic, đơn giản; cycle detector là hạng mục sau / deterministic, simple; cycle detector later |
| 5 | Arena allocator cho toàn bộ AST/KIR của compiler | Free O(1), không leak theo thiết kế / O(1) teardown, leak-free by construction |
| 6 | Static link runtime `libkamirt.a`, link qua clang/lld | Executable tự chứa, không cần DLL / self-contained executable |
| 7 | Pin LLVM 18 | Trùng toolchain môi trường dev (clang 18.1.3) / matches dev environment toolchain |

### Build instructions / Hướng dẫn build
Phase 0 **chỉ có tài liệu, chưa có code** — không có gì để build. / Phase 0 is **documentation only, no code yet** — nothing to build.

Chuẩn bị môi trường cho Phase 1 (đã kiểm chứng trên môi trường dev): / Environment prep for Phase 1 (verified on the dev environment):

```bash
# Yêu cầu / Requirements
cmake --version        # ≥ 3.20   (verified: 3.28.3)
clang++ --version      # LLVM 18  (verified: 18.1.3)
llvm-config --version  # 18       (verified: 18.1.3)

# Lệnh build sẽ dùng từ Phase 1 / Build commands starting Phase 1
cmake -B build
cmake --build build
```

### Test report / Báo cáo test
| Hạng mục / Item | Kết quả / Result |
|---|---|
| Code build | N/A — Phase 0 không có code / no code in Phase 0 |
| Unit tests | N/A — sẽ bắt đầu từ Phase 1 / start from Phase 1 |
| Toolchain verification | ✅ PASS — cmake 3.28.3, clang++ 18.1.3, llvm-config 18.1.3, llc có sẵn / available |
| Docs review | ✅ PASS — VI/EN đồng bộ nội dung, link chéo hoạt động / VI/EN content in sync, cross-links valid |

### Exit criteria Phase 0
- [x] Phân tích compiler: Lexer, Parser, AST, Semantic, Optimization, Codegen
- [x] Thiết kế runtime: object system, dynamic type, memory model, GC
- [x] Giải thích binary generation: `.py → AST → IR → LLVM → Machine Code → Executable`
- [x] Tài liệu song ngữ VI + EN
- [x] Changelog kèm hướng dẫn build + báo cáo test
- [x] Không viết code trước khi hoàn thành thiết kế / No code written before design completion

### Next / Tiếp theo
**Phase 1 — Project Foundation:** CMake project, source/include/test structure, build ra `kamipy` executable chạy được.
