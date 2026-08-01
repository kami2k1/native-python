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
    case Tok::KW_TRY: return "try";
    case Tok::KW_EXCEPT: return "except";
    case Tok::KW_FINALLY: return "finally";
    case Tok::KW_RAISE: return "raise";
    case Tok::KW_FROM: return "from";
    case Tok::KW_AS: return "as";
    case Tok::KW_CLASS: return "class";
    case Tok::KW_ASSERT: return "assert";
    case Tok::KW_IS: return "is";
    case Tok::KW_DEL: return "del";
    case Tok::KW_WITH: return "with";
    case Tok::KW_LAMBDA: return "lambda";
    case Tok::KW_GLOBAL: return "global";
    case Tok::KW_YIELD: return "yield";
    case Tok::KW_NONLOCAL: return "nonlocal";
    case Tok::PLUS: return "+";
    case Tok::MINUS: return "-";
    case Tok::STAR: return "*";
    case Tok::SLASH: return "/";
    case Tok::DSLASH: return "//";
    case Tok::PERCENT: return "%";
    case Tok::POW: return "**";
    case Tok::AMP: return "&";
    case Tok::PIPE: return "|";
    case Tok::CARET: return "^";
    case Tok::TILDE: return "~";
    case Tok::LSHIFT: return "<<";
    case Tok::RSHIFT: return ">>";
    case Tok::AMPEQ: return "&=";
    case Tok::PIPEEQ: return "|=";
    case Tok::CARETEQ: return "^=";
    case Tok::LSHIFTEQ: return "<<=";
    case Tok::RSHIFTEQ: return ">>=";
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
    case Tok::DSLASHEQ: return "//=";
    case Tok::PERCENTEQ: return "%=";
    case Tok::POWEQ: return "**=";
    case Tok::LPAREN: return "(";
    case Tok::RPAREN: return ")";
    case Tok::LBRACKET: return "[";
    case Tok::RBRACKET: return "]";
    case Tok::LBRACE: return "{";
    case Tok::RBRACE: return "}";
    case Tok::COLON: return ":";
    case Tok::COMMA: return ",";
    case Tok::DOT: return ".";
    case Tok::ELLIPSIS: return "...";
    case Tok::ARROW: return "->";
    case Tok::AT: return "@";
    case Tok::SEMI: return ";";
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
        {"try", Tok::KW_TRY},       {"except", Tok::KW_EXCEPT},
        {"finally", Tok::KW_FINALLY}, {"raise", Tok::KW_RAISE},
        {"from", Tok::KW_FROM},     {"as", Tok::KW_AS},
        {"class", Tok::KW_CLASS},   {"assert", Tok::KW_ASSERT},
        {"is", Tok::KW_IS},         {"del", Tok::KW_DEL},
        {"with", Tok::KW_WITH},     {"lambda", Tok::KW_LAMBDA},
        {"global", Tok::KW_GLOBAL}, {"yield", Tok::KW_YIELD},
        {"nonlocal", Tok::KW_NONLOCAL},
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
        for (;;) {
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
        // hex / octal / binary
        if (peek() == '0' && (peek(1) == 'x' || peek(1) == 'X' || peek(1) == 'o' ||
                              peek(1) == 'O' || peek(1) == 'b' || peek(1) == 'B')) {
            char kind = (char)tolower((unsigned char)peek(1));
            pos += 2;
            std::string digits;
            auto ok = [&](char c) {
                if (kind == 'x') return isxdigit((unsigned char)c) != 0;
                if (kind == 'o') return c >= '0' && c <= '7';
                return c == '0' || c == '1';
            };
            while (ok(peek()) || peek() == '_') {
                if (peek() != '_') digits += peek();
                pos++;
            }
            if (digits.empty()) err("invalid numeric literal");
            Token t;
            t.line = line;
            t.kind = Tok::INT;
            t.ival = strtoll(digits.c_str(), nullptr, kind == 'x' ? 16 : kind == 'o' ? 8 : 2);
            out.push_back(std::move(t));
            return;
        }
        std::string text;
        bool is_float = false;
        while (isdigit((unsigned char)peek()) || peek() == '_') {
            if (peek() != '_') text += peek();
            pos++;
        }
        if (peek() == '.' && peek(1) != '.') { // "1." and "1.5" (but not "1..")
            is_float = true;
            text += '.';
            pos++;
            while (isdigit((unsigned char)peek()) || peek() == '_') {
                if (peek() != '_') text += peek();
                pos++;
            }
        }
        if (peek() == 'e' || peek() == 'E') {
            size_t save = pos;
            std::string ex;
            ex += peek();
            pos++;
            if (peek() == '+' || peek() == '-') { ex += peek(); pos++; }
            if (isdigit((unsigned char)peek())) {
                is_float = true;
                while (isdigit((unsigned char)peek()) || peek() == '_') {
                    if (peek() != '_') ex += peek();
                    pos++;
                }
                text += ex;
            } else {
                pos = save;
            }
        }
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

    // Reads string content after the opening quote(s). Returns the raw value.
    std::string read_string_body(char quote, bool triple, bool raw) {
        std::string val;
        for (;;) {
            if (at_end()) err("unterminated string literal");
            char c = peek();
            if (triple) {
                if (c == quote && peek(1) == quote && peek(2) == quote) {
                    pos += 3;
                    return val;
                }
                if (c == '\n') {
                    val += '\n';
                    pos++;
                    line++;
                    continue;
                }
            } else {
                if (c == quote) {
                    pos++;
                    return val;
                }
                if (c == '\n') err("unterminated string literal");
            }
            pos++;
            if (c == '\\' && !raw) {
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
                case '\n': line++; break; // escaped newline: line continuation
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
                default: // Python-style: unknown escapes stay literal
                    val += '\\';
                    val += e;
                    break;
                }
            } else if (c == '\\' && raw) {
                val += '\\';
                if (!at_end()) {
                    char e = advance();
                    if (e == '\n') line++;
                    val += e;
                }
            } else {
                val += c;
            }
        }
    }

    // f-string: expand to  ( "lit" + str ( expr ) + ... )  token sequence.
    void lex_fstring(char quote, bool triple, bool raw) {
        std::string body = read_string_body(quote, triple, raw);
        // split into parts
        struct Part { bool is_expr; std::string text; std::string spec; };
        std::vector<Part> parts;
        std::string lit;
        size_t i = 0;
        while (i < body.size()) {
            char c = body[i];
            if (c == '{' && i + 1 < body.size() && body[i + 1] == '{') {
                lit += '{';
                i += 2;
                continue;
            }
            if (c == '}' && i + 1 < body.size() && body[i + 1] == '}') {
                lit += '}';
                i += 2;
                continue;
            }
            if (c == '}') err("single '}' is not allowed in f-string");
            if (c != '{') {
                lit += c;
                i++;
                continue;
            }
            // expression
            i++;
            int depth = 0;
            std::string expr;
            for (;;) {
                if (i >= body.size()) err("unterminated '{' in f-string");
                char e = body[i];
                if (depth == 0 && (e == '}' || e == ':' || e == '!')) break;
                if (e == '(' || e == '[' || e == '{') depth++;
                if (e == ')' || e == ']' || e == '}') depth--;
                expr += e;
                i++;
            }
            std::string spec;
            if (body[i] == '!') { // conversion !r/!s: treat as str()
                i++;
                if (i < body.size() && (body[i] == 'r' || body[i] == 's')) i++;
            }
            if (i < body.size() && body[i] == ':') { // format spec
                i++;
                while (i < body.size() && body[i] != '}') {
                    if (body[i] == '{') err("nested '{' in f-string format spec");
                    spec += body[i];
                    i++;
                }
            }
            if (i >= body.size() || body[i] != '}') err("unterminated '{' in f-string");
            i++;
            // f-string debug form: {expr = } / {expr=}
            std::string dbg_prefix;
            {
                size_t e3 = expr.find_last_not_of(" \t");
                if (e3 != std::string::npos && expr[e3] == '=' &&
                    (e3 == 0 || expr[e3 - 1] != '=' )) {
                    dbg_prefix = expr; // includes the '='
                    expr = expr.substr(0, e3);
                }
            }
            if (!dbg_prefix.empty()) lit += dbg_prefix;
            if (!lit.empty() || parts.empty()) parts.push_back({false, lit, ""});
            lit.clear();
            // trim expr
            size_t b = expr.find_first_not_of(" \t");
            size_t e2 = expr.find_last_not_of(" \t");
            if (b == std::string::npos) err("empty expression in f-string");
            parts.push_back({true, expr.substr(b, e2 - b + 1), spec});
        }
        if (!lit.empty() || parts.empty()) parts.push_back({false, lit, ""});

        push(Tok::LPAREN);
        bool first = true;
        for (auto& p : parts) {
            if (!first) push(Tok::PLUS);
            first = false;
            if (!p.is_expr) {
                push(Tok::STRING, p.text);
            } else {
                push(Tok::NAME, p.spec.empty() ? "str" : "format");
                push(Tok::LPAREN);
                Lexer sub(p.text);
                sub.line = line;
                sub.run();
                for (auto& t : sub.out) {
                    if (t.kind == Tok::NEWLINE || t.kind == Tok::INDENT ||
                        t.kind == Tok::DEDENT || t.kind == Tok::END)
                        continue;
                    t.line = line;
                    out.push_back(t);
                }
                if (!p.spec.empty()) {
                    push(Tok::COMMA);
                    push(Tok::STRING, p.spec);
                }
                push(Tok::RPAREN);
            }
        }
        push(Tok::RPAREN);
    }

    // Looks ahead past whitespace / comments / line continuations for another
    // string literal — Python's implicit concatenation (`"a" f"b" "c"`).
    // Returns 0 (nothing), 1 (plain literal) or 2 (f-string). Consumes nothing.
    int peek_string_literal() const {
        size_t p = pos;
        for (;;) {
            if (p >= src.size()) return 0;
            char c = src[p];
            if (c == ' ' || c == '\t' || c == '\r') { p++; continue; }
            if (c == '#') {
                while (p < src.size() && src[p] != '\n') p++;
                continue;
            }
            if (c == '\\') { // explicit line continuation
                size_t q = p + 1;
                if (q < src.size() && src[q] == '\r') q++;
                if (q < src.size() && src[q] == '\n') { p = q + 1; continue; }
                return 0;
            }
            if (c == '\n') {
                if (paren_depth == 0) return 0; // the statement ends here
                p++;
                continue;
            }
            break;
        }
        size_t q = p;
        bool fstr = false;
        while (q < src.size() && q - p < 2) { // optional r/f/b prefix
            char l = (char)tolower((unsigned char)src[q]);
            if (l == 'r' || l == 'b') { q++; continue; }
            if (l == 'f') { fstr = true; q++; continue; }
            break;
        }
        if (q >= src.size() || (src[q] != '"' && src[q] != '\'')) return 0;
        return fstr ? 2 : 1;
    }

    // Adjacent plain literals are merged by the parser, but an f-string has
    // already been expanded into a token group here, so the pieces have to be
    // joined with an explicit '+' instead.
    void maybe_implicit_concat(bool was_fstring) {
        int nxt = peek_string_literal();
        if (nxt != 0 && (was_fstring || nxt == 2)) push(Tok::PLUS);
    }

    // Handles a name; also detects string prefixes (r, f, b combinations).
    void lex_name() {
        size_t start = pos;
        while (isalnum((unsigned char)peek()) || peek() == '_') pos++;
        std::string text = src.substr(start, pos - start);
        // string prefix?
        if ((peek() == '"' || peek() == '\'') && text.size() <= 2) {
            bool raw = false, fstr = false, okpref = true;
            for (char c : text) {
                char l = (char)tolower((unsigned char)c);
                if (l == 'r') raw = true;
                else if (l == 'f') fstr = true;
                else if (l == 'b') { /* bytes → str */ }
                else okpref = false;
            }
            if (okpref) {
                // b"..." is modeled as a plain str: the runtime has no separate
                // bytes type (str already stores arbitrary bytes).
                char quote = advance();
                bool triple = peek() == quote && peek(1) == quote;
                if (triple) pos += 2;
                if (fstr) lex_fstring(quote, triple, raw);
                else push(Tok::STRING, read_string_body(quote, triple, raw));
                maybe_implicit_concat(fstr);
                return;
            }
        }
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
            if (c == '\\') { // explicit line continuation
                size_t save = pos;
                pos++;
                if (peek() == '\r') pos++;
                if (peek() == '\n') {
                    pos++;
                    line++;
                    continue;
                }
                pos = save;
                err("unexpected character '\\'");
            }
            if (c == ' ' || c == '\t' || c == '\r') { pos++; continue; }
            if (c == '#') {
                while (!at_end() && peek() != '\n') pos++;
                continue;
            }
            if (isdigit((unsigned char)c) ||
                (c == '.' && isdigit((unsigned char)peek(1)))) {
                if (c == '.') { // ".5" float
                    std::string text = "0";
                    text += advance();
                    while (isdigit((unsigned char)peek()) || peek() == '_') {
                        if (peek() != '_') text += peek();
                        pos++;
                    }
                    Token t;
                    t.line = line;
                    t.kind = Tok::FLOAT;
                    t.fval = strtod(text.c_str(), nullptr);
                    out.push_back(std::move(t));
                    continue;
                }
                lex_number();
                continue;
            }
            if (c == '"' || c == '\'') {
                pos++;
                bool triple = peek() == c && peek(1) == c;
                if (triple) pos += 2;
                push(Tok::STRING, read_string_body(c, triple, false));
                maybe_implicit_concat(false);
                continue;
            }
            if (isalpha((unsigned char)c) || c == '_') { lex_name(); continue; }
            pos++;
            switch (c) {
            case '+': peek() == '=' ? (pos++, push(Tok::PLUSEQ)) : push(Tok::PLUS); break;
            case '-':
                if (peek() == '=') { pos++; push(Tok::MINUSEQ); }
                else if (peek() == '>') { pos++; push(Tok::ARROW); }
                else push(Tok::MINUS);
                break;
            case '*':
                if (peek() == '*') {
                    pos++;
                    if (peek() == '=') { pos++; push(Tok::POWEQ); }
                    else push(Tok::POW);
                } else if (peek() == '=') { pos++; push(Tok::STAREQ); }
                else push(Tok::STAR);
                break;
            case '/':
                if (peek() == '/') {
                    pos++;
                    if (peek() == '=') { pos++; push(Tok::DSLASHEQ); }
                    else push(Tok::DSLASH);
                } else if (peek() == '=') { pos++; push(Tok::SLASHEQ); }
                else push(Tok::SLASH);
                break;
            case '%': peek() == '=' ? (pos++, push(Tok::PERCENTEQ)) : push(Tok::PERCENT); break;
            case '&': peek() == '=' ? (pos++, push(Tok::AMPEQ)) : push(Tok::AMP); break;
            case '|': peek() == '=' ? (pos++, push(Tok::PIPEEQ)) : push(Tok::PIPE); break;
            case '^': peek() == '=' ? (pos++, push(Tok::CARETEQ)) : push(Tok::CARET); break;
            case '~': push(Tok::TILDE); break;
            case '=': peek() == '=' ? (pos++, push(Tok::EQ)) : push(Tok::ASSIGN); break;
            case '!':
                if (peek() == '=') { pos++; push(Tok::NE); }
                else err("unexpected '!'");
                break;
            case '<':
                if (peek() == '=') { pos++; push(Tok::LE); }
                else if (peek() == '<') {
                    pos++;
                    if (peek() == '=') { pos++; push(Tok::LSHIFTEQ); }
                    else push(Tok::LSHIFT);
                } else push(Tok::LT);
                break;
            case '>':
                if (peek() == '=') { pos++; push(Tok::GE); }
                else if (peek() == '>') {
                    pos++;
                    if (peek() == '=') { pos++; push(Tok::RSHIFTEQ); }
                    else push(Tok::RSHIFT);
                } else push(Tok::GT);
                break;
            case '(': paren_depth++; push(Tok::LPAREN); break;
            case ')': paren_depth--; push(Tok::RPAREN); break;
            case '[': paren_depth++; push(Tok::LBRACKET); break;
            case ']': paren_depth--; push(Tok::RBRACKET); break;
            case '{': paren_depth++; push(Tok::LBRACE); break;
            case '}': paren_depth--; push(Tok::RBRACE); break;
            case ':': push(Tok::COLON); break;
            case ',': push(Tok::COMMA); break;
            case '.':
                // `...` is the Ellipsis literal (used in type stubs and as a
                // no-op body); emit a single ELLIPSIS token.
                if (peek() == '.' && peek(1) == '.') {
                    pos += 2;
                    push(Tok::ELLIPSIS);
                } else {
                    push(Tok::DOT);
                }
                break;
            case '@': push(Tok::AT); break;
            case ';': push(Tok::SEMI); break;
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
    // Skip a UTF-8 BOM (EF BB BF) — Windows Notepad prepends one by default,
    // which would otherwise surface as "unexpected character" on line 1.
    if (src.size() >= 3 && (unsigned char)src[0] == 0xEF && (unsigned char)src[1] == 0xBB &&
        (unsigned char)src[2] == 0xBF)
        lx.pos = 3;
    lx.run();
    return std::move(lx.out);
}

} // namespace kami
