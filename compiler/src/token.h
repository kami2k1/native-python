#pragma once
#include <cstdint>
#include <stdexcept>
#include <string>
#include <vector>

namespace kami {

enum class Tok {
    // literals / names
    INT, FLOAT, STRING, NAME,
    // keywords
    KW_DEF, KW_RETURN, KW_IF, KW_ELIF, KW_ELSE, KW_WHILE, KW_FOR, KW_IN,
    KW_BREAK, KW_CONTINUE, KW_PASS, KW_IMPORT, KW_AND, KW_OR, KW_NOT,
    KW_TRUE, KW_FALSE, KW_NONE,
    KW_TRY, KW_EXCEPT, KW_FINALLY, KW_RAISE, KW_FROM, KW_AS, KW_CLASS,
    KW_ASSERT, KW_IS, KW_DEL, KW_WITH, KW_LAMBDA, KW_GLOBAL, KW_YIELD,
    KW_NONLOCAL,
    // operators / punctuation
    PLUS, MINUS, STAR, SLASH, DSLASH, PERCENT, POW,
    AMP, PIPE, CARET, TILDE, LSHIFT, RSHIFT,
    ASSIGN, EQ, NE, LT, GT, LE, GE,
    PLUSEQ, MINUSEQ, STAREQ, SLASHEQ, DSLASHEQ, PERCENTEQ, POWEQ,
    AMPEQ, PIPEEQ, CARETEQ, LSHIFTEQ, RSHIFTEQ,
    LPAREN, RPAREN, LBRACKET, RBRACKET, LBRACE, RBRACE,
    COLON, COMMA, DOT, ARROW, AT, SEMI,
    // layout
    NEWLINE, INDENT, DEDENT, END,
};

struct Token {
    Tok kind;
    std::string text;  // name / string value (unescaped)
    int64_t ival = 0;
    double fval = 0.0;
    int line = 0;
};

struct CompileError : std::runtime_error {
    int line;
    CompileError(int line_, const std::string& msg)
        : std::runtime_error(msg), line(line_) {}
};

const char* tok_name(Tok t);

} // namespace kami
