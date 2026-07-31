#pragma once
#include "ast.h"

namespace kami {

// Name resolution, scope handling, builtin/module binding, arity checks and
// constant folding. Annotates the AST in place. Throws CompileError.
void analyze(Module& m);

} // namespace kami
