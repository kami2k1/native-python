// Lexer unit tests. Exit code 0 = pass.
#include "../../compiler/src/lexer.h"

#include <cstdio>

using namespace kami;

static int failures = 0;

#define CHECK(cond)                                                                     \
    do {                                                                                \
        if (!(cond)) {                                                                  \
            fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);             \
            failures++;                                                                 \
        }                                                                               \
    } while (0)

static std::vector<Tok> kinds(const std::string& src) {
    std::vector<Tok> out;
    for (auto& t : lex(src)) out.push_back(t.kind);
    return out;
}

int main() {
    // numbers, strings, names, operators
    {
        auto ts = lex("x = 123 + 3.14");
        CHECK(ts.size() == 7); // NAME = INT + FLOAT NEWLINE END
        CHECK(ts[0].kind == Tok::NAME && ts[0].text == "x");
        CHECK(ts[1].kind == Tok::ASSIGN);
        CHECK(ts[2].kind == Tok::INT && ts[2].ival == 123);
        CHECK(ts[3].kind == Tok::PLUS);
        CHECK(ts[4].kind == Tok::FLOAT && ts[4].fval > 3.13 && ts[4].fval < 3.15);
        CHECK(ts[5].kind == Tok::NEWLINE);
        CHECK(ts[6].kind == Tok::END);
    }
    {
        auto ts = lex("s = \"he\\\"llo\\n\"");
        CHECK(ts[2].kind == Tok::STRING && ts[2].text == "he\"llo\n");
    }
    // keywords vs identifiers
    {
        auto ts = lex("if ifx else def defx");
        CHECK(ts[0].kind == Tok::KW_IF);
        CHECK(ts[1].kind == Tok::NAME && ts[1].text == "ifx");
        CHECK(ts[2].kind == Tok::KW_ELSE);
        CHECK(ts[3].kind == Tok::KW_DEF);
        CHECK(ts[4].kind == Tok::NAME);
    }
    // two-char operators
    {
        auto k = kinds("a == b != c <= d >= e // f += g");
        CHECK(k[1] == Tok::EQ);
        CHECK(k[3] == Tok::NE);
        CHECK(k[5] == Tok::LE);
        CHECK(k[7] == Tok::GE);
        CHECK(k[9] == Tok::DSLASH);
        CHECK(k[11] == Tok::PLUSEQ);
    }
    // INDENT / DEDENT
    {
        auto k = kinds("if x:\n    y = 1\n    z = 2\nw = 3\n");
        std::vector<Tok> expect = {
            Tok::KW_IF,  Tok::NAME,   Tok::COLON,  Tok::NEWLINE, Tok::INDENT,
            Tok::NAME,   Tok::ASSIGN, Tok::INT,    Tok::NEWLINE, Tok::NAME,
            Tok::ASSIGN, Tok::INT,    Tok::NEWLINE, Tok::DEDENT, Tok::NAME,
            Tok::ASSIGN, Tok::INT,    Tok::NEWLINE, Tok::END};
        CHECK(k == expect);
    }
    // nested blocks + dedent-to-zero at EOF
    {
        auto k = kinds("while a:\n  if b:\n    c = 1\n");
        int indents = 0, dedents = 0;
        for (auto t : k) {
            if (t == Tok::INDENT) indents++;
            if (t == Tok::DEDENT) dedents++;
        }
        CHECK(indents == 2 && dedents == 2);
    }
    // blank lines and comments are skipped
    {
        auto k = kinds("a = 1\n\n# comment\n   # indented comment\nb = 2\n");
        std::vector<Tok> expect = {Tok::NAME, Tok::ASSIGN, Tok::INT,    Tok::NEWLINE,
                                   Tok::NAME, Tok::ASSIGN, Tok::INT,    Tok::NEWLINE,
                                   Tok::END};
        CHECK(k == expect);
    }
    // implicit line continuation inside brackets
    {
        auto k = kinds("x = [1,\n 2,\n 3]\n");
        int newlines = 0;
        for (auto t : k)
            if (t == Tok::NEWLINE) newlines++;
        CHECK(newlines == 1);
    }
    // errors
    {
        bool threw = false;
        try {
            lex("if x:\n\ty = 1\n");
        } catch (const CompileError&) {
            threw = true;
        }
        CHECK(threw); // tab in indentation
    }
    {
        bool threw = false;
        try {
            lex("s = \"unterminated");
        } catch (const CompileError&) {
            threw = true;
        }
        CHECK(threw);
    }
    {
        bool threw = false;
        try {
            lex("if x:\n    y = 1\n  z = 2\n");
        } catch (const CompileError&) {
            threw = true;
        }
        CHECK(threw); // bad dedent level
    }

    if (failures) {
        fprintf(stderr, "%d lexer check(s) failed\n", failures);
        return 1;
    }
    printf("lexer: all checks passed\n");
    return 0;
}
