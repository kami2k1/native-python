#pragma once
#include "ast.h"

namespace kami {

// Parses a token stream into a Module AST. Throws CompileError on bad input.
Module parse(std::vector<Token> tokens);

} // namespace kami
