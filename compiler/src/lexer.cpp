#include "lexer.h"

#include <cctype>
#include <cstdlib>
#include <unordered_map>

namespace kami {

const char* tok_name(Tok t) {
    switch (t) {
    case Tok::INT: return "INT";
    case Tok::FLOAT: return "FLOAT";
    case Tok::STRING: return "STRING";
    case Tok::NAME: return "NAME";
    case Tok::KW_DEF: return "def";
    case Tok::KW_RETURN: return "return";
    case Tok::KW_IF: return "if";
    case Tok::KW_ELIF: return "elif";
    case Tok::KW_ELSE: return "else";
    case Tok::KW_WHILE: return "while";
    case Tok::KW_FOR: return "for";
    case Tok::KW_IN: return "in";
    case Tok::KW_BREAK: return "break";
    case Tok::KW_CONTINUE: return "continue";
    case Tok::KW_PASS: return "pass";
    case Tok::KW_IMPORT: return "import";
    case Tok::KW_AND: return "and";
    case Tok::KW_OR: return "or";
    case Tok::KW_NOT: return "not";
    case Tok::KW_TRUE: return "True";
    case Tok::KW_FALSE: return "False";
    case Tok::KW_NONE: return "None";
    case Tok::PLUS: return "+";
    case Tok::MINUS: return "-";
    case Tok::STAR: return "*";
    case Tok::SLASH: return "/";
    case Tok::DSLASH: return "//";
    case Tok::PERCENT: return "%";
    case Tok::ASSIGN: return "=";
    case Tok::EQ: return "==";
    case Tok::NE: return "!=";
    case Tok::LT: return "<";
    case Tok::GT: return ">";
    case Tok::LE: return "<=";
    case Tok::GE: return ">=";
    case Tok::PLUSEQ: return "+=";
    case Tok::MINUSEQ: return "-=";
    case Tok::STAREQ: return "*=";
    case Tok::SLASHEQ: return "/=";
    case Tok::LPAREN: return "(";
    case Tok::RPAREN: return ")";
    case Tok::LBRACKET: return "[";
    case Tok::RBRACKET: return "]";
    case Tok::LBRACE: return "{";
    case Tok::RBRACE: return "}";
    case Tok::COLON: return ":";
    case Tok::COMMA: return ",";
    case Tok::DOT: return ".";
    case Tok::NEWLINE: return "NEWLINE";
    case Tok::INDENT: return "INDENT";
    case Tok::DEDENT: return "DEDENT";
    case Tok::END: return "END";
    }
    return "?";
}

static const std::unordered_map<std::string, Tok>& keywords() {
    static const std::unordered_map<std::string, Tok> kw = {
        {"def", Tok::KW_DEF},       {"return", Tok::KW_RETURN},
        {"if", Tok::KW_IF},         {"elif", Tok::KW_ELIF},
        {"else", Tok::KW_ELSE},     {"while", Tok::KW_WHILE},
        {"for", Tok::KW_FOR},       {"in", Tok::KW_IN},
        {"break", Tok::KW_BREAK},   {"continue", Tok::KW_CONTINUE},
        {"pass", Tok::KW_PASS},     {"import", Tok::KW_IMPORT},
        {"and", Tok::KW_AND},       {"or", Tok::KW_OR},
        {"not", Tok::KW_NOT},       {"True", Tok::KW_TRUE},
        {"False", Tok::KW_FALSE},   {"None", Tok::KW_NONE},
    };
    return kw;
}

namespace {
struct Lexer {
    const std::string& src;
    size_t pos = 0;
    int line = 1;
    int paren_depth = 0;
    std::vector<int> indents{0};
    std::vector<Token> out;

    explicit Lexer(const std::string& s) : src(s) {}

    char peek(size_t off = 0) const {
        return pos + off < src.size() ? src[pos + off] : '\0';
    }
    char advance() { return src[pos++]; }
    bool at_end() const { return pos >= src.size(); }

    void push(Tok k, std::string text = "") {
        Token t;
        t.kind = k;
        t.text = std::move(text);
        t.line = line;
        out.push_back(std::move(t));
    }

    [[noreturn]] void err(const std::string& m) { throw CompileError(line, m); }

    void handle_indent() {
        // Called at the start of a logical line. Skips blank/comment lines.
        for (;;) {
            size_t start = pos;
            int col = 0;
            while (!at_end() && (peek() == ' ' || peek() == '\t')) {
                if (peek() == '\t') err("tabs are not allowed in indentation");
                col++;
                pos++;
            }
            if (at_end()) return;
            if (peek() == '\n') { pos++; line++; continue; }        // blank line
            if (peek() == '#') {                                     // comment-only line
                while (!at_end() && peek() != '\n') pos++;
                continue;
            }
            if (peek() == '\r') { pos++; continue; }
            (void)start;
            if (col > indents.back()) {
                indents.push_back(col);
                push(Tok::INDENT);
            } else {
                while (col < indents.back()) {
                    indents.pop_back();
                    push(Tok::DEDENT);
                }
                if (col != indents.back()) err("unindent does not match any outer indentation level");
            }
            return;
        }
    }

    void lex_number() {
        size_t start = pos;
        while (isdigit((unsigned char)peek())) pos++;
        bool is_float = false;
        if (peek() == '.' && isdigit((unsigned char)peek(1))) {
            is_float = true;
            pos++;
            while (isdigit((unsigned char)peek())) pos++;
        }
        if (peek() == 'e' || peek() == 'E') {
            size_t save = pos;
            pos++;
            if (peek() == '+' || peek() == '-') pos++;
            if (isdigit((unsigned char)peek())) {
                is_float = true;
                while (isdigit((unsigned char)peek())) pos++;
            } else {
                pos = save;
            }
        }
        std::string text = src.substr(start, pos - start);
        Token t;
        t.line = line;
        t.text = text;
        if (is_float) {
            t.kind = Tok::FLOAT;
            t.fval = strtod(text.c_str(), nullptr);
        } else {
            t.kind = Tok::INT;
            t.ival = strtoll(text.c_str(), nullptr, 10);
        }
        out.push_back(std::move(t));
    }

    void lex_string(char quote) {
        std::string val;
        while (!at_end() && peek() != quote) {
            char c = advance();
            if (c == '\n') err("unterminated string literal");
            if (c == '\\') {
                if (at_end()) err("unterminated string literal");
                char e = advance();
                switch (e) {
                case 'n': val += '\n'; break;
                case 't': val += '\t'; break;
                case 'r': val += '\r'; break;
                case '\\': val += '\\'; break;
                case '\'': val += '\''; break;
                case '"': val += '"'; break;
                case '0': val += '\0'; break;
                case 'x': {
                    auto hex = [&](char h) -> int {
                        if (h >= '0' && h <= '9') return h - '0';
                        if (h >= 'a' && h <= 'f') return h - 'a' + 10;
                        if (h >= 'A' && h <= 'F') return h - 'A' + 10;
                        err("bad \\x escape");
                    };
                    int hi = hex(advance()), lo = hex(advance());
                    val += (char)(hi * 16 + lo);
                    break;
                }
                default: err(std::string("unknown escape '\\") + e + "'");
                }
            } else {
                val += c;
            }
        }
        if (at_end()) err("unterminated string literal");
        pos++; // closing quote
        push(Tok::STRING, val);
    }

    void lex_name() {
        size_t start = pos;
        while (isalnum((unsigned char)peek()) || peek() == '_') pos++;
        std::string text = src.substr(start, pos - start);
        auto it = keywords().find(text);
        if (it != keywords().end()) push(it->second, text);
        else push(Tok::NAME, text);
    }

    void run() {
        bool at_line_start = true;
        while (!at_end()) {
            if (at_line_start && paren_depth == 0) {
                handle_indent();
                at_line_start = false;
                if (at_end()) break;
            }
            char c = peek();
            if (c == '\n') {
                pos++;
                if (paren_depth == 0) {
                    if (!out.empty() && out.back().kind != Tok::NEWLINE &&
                        out.back().kind != Tok::INDENT && out.back().kind != Tok::DEDENT)
                        push(Tok::NEWLINE);
                    at_line_start = true;
                }
                line++;
                continue;
            }
            if (c == ' ' || c == '\t' || c == '\r') { pos++; continue; }
            if (c == '#') {
                while (!at_end() && peek() != '\n') pos++;
                continue;
            }
            if (isdigit((unsigned char)c)) { lex_number(); continue; }
            if (c == '"' || c == '\'') { pos++; lex_string(c); continue; }
            if (isalpha((unsigned char)c) || c == '_') { lex_name(); continue; }
            pos++;
            switch (c) {
            case '+': peek() == '=' ? (pos++, push(Tok::PLUSEQ)) : push(Tok::PLUS); break;
            case '-': peek() == '=' ? (pos++, push(Tok::MINUSEQ)) : push(Tok::MINUS); break;
            case '*': peek() == '=' ? (pos++, push(Tok::STAREQ)) : push(Tok::STAR); break;
            case '/':
                if (peek() == '/') { pos++; push(Tok::DSLASH); }
                else if (peek() == '=') { pos++; push(Tok::SLASHEQ); }
                else push(Tok::SLASH);
                break;
            case '%': push(Tok::PERCENT); break;
            case '=': peek() == '=' ? (pos++, push(Tok::EQ)) : push(Tok::ASSIGN); break;
            case '!':
                if (peek() == '=') { pos++; push(Tok::NE); }
                else err("unexpected '!'");
                break;
            case '<': peek() == '=' ? (pos++, push(Tok::LE)) : push(Tok::LT); break;
            case '>': peek() == '=' ? (pos++, push(Tok::GE)) : push(Tok::GT); break;
            case '(': paren_depth++; push(Tok::LPAREN); break;
            case ')': paren_depth--; push(Tok::RPAREN); break;
            case '[': paren_depth++; push(Tok::LBRACKET); break;
            case ']': paren_depth--; push(Tok::RBRACKET); break;
            case '{': paren_depth++; push(Tok::LBRACE); break;
            case '}': paren_depth--; push(Tok::RBRACE); break;
            case ':': push(Tok::COLON); break;
            case ',': push(Tok::COMMA); break;
            case '.': push(Tok::DOT); break;
            default: err(std::string("unexpected character '") + c + "'");
            }
        }
        if (!out.empty() && out.back().kind != Tok::NEWLINE) push(Tok::NEWLINE);
        while (indents.size() > 1) {
            indents.pop_back();
            push(Tok::DEDENT);
        }
        push(Tok::END);
    }
};
} // namespace

std::vector<Token> lex(const std::string& src) {
    Lexer lx(src);
    lx.run();
    return std::move(lx.out);
}

} // namespace kami
