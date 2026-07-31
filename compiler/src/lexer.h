#pragma once
#include "token.h"

namespace kami {

// Tokenizes KamiPython source, producing INDENT/DEDENT layout tokens.
std::vector<Token> lex(const std::string& src);

} // namespace kami
