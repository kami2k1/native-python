#pragma once
#include "ast.h"

namespace kami {

// Lowers an analyzed Module to LLVM IR (textual .ll for LLVM 18+, opaque pointers).
std::string codegen(const Module& m, const std::string& source_name);

} // namespace kami
