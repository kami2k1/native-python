#pragma once
#include "ast.h"

namespace kami {

// Name resolution, scope handling, builtin/module binding, arity checks and
// constant folding. Annotates the AST in place. Throws CompileError.
void analyze(Module& m);

// True for modules handled natively by the compiler/runtime (math, os, json,
// requests, ...) or accepted as no-ops (typing, __future__, ...). The driver
// uses this to decide whether "import X" should look for a local X.py to
// bundle instead.
bool known_builtin_module(const std::string& name);

} // namespace kami
