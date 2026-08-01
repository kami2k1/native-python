#pragma once
#include "ast.h"

namespace kami {

// Lowers an analyzed Module to LLVM IR (textual .ll for LLVM 18+, opaque pointers).
// `target` is an optional LLVM target triple recorded in the module header.
std::string codegen(const Module& m, const std::string& source_name,
                    const std::string& target = "");

} // namespace kami
