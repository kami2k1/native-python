#pragma once
// Embedded native code generation (Part III): LLVM TargetMachine + LLD.
//
// When kamipy is built with KAMI_EMBED_LLVM, the driver compiles the textual
// LLVM IR produced by codegen straight to a native object file in memory and
// links the final executable with the in-process LLD library — no external
// clang/gcc/ld process, no toolchain requirement on the user's machine.
#include <string>
#include <vector>

namespace kami {

// True when this kamipy binary carries the embedded LLVM/LLD backend.
bool native_backend_available();

// Parses + optimizes `ir` (textual LLVM IR) and writes a native object file.
// Throws std::runtime_error with a readable message on invalid IR.
void compile_ir_to_object(const std::string& ir, const std::string& obj_path,
                          int opt_level);

// Links object files and libraries into an executable using the embedded LLD
// (ELF on Linux, COFF on Windows). `libs` are bare names ("m", "pthread").
// Throws std::runtime_error when the link fails.
void link_executable(const std::vector<std::string>& objects,
                     const std::string& runtime_lib,
                     const std::vector<std::string>& libs,
                     const std::string& output);

} // namespace kami
