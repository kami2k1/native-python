#include "parser.h"

#include "../../runtime/include/kami_runtime.h"

namespace kami {

namespace {

struct Parser {
    std::vector<Token> toks;
    size_t pos = 0;
    int func_depth = 0;
    int loop_depth = 0;

    explicit Parser(std::vector<Token> t) : toks(std::move(t)) {}

    const Token& peek(size_t off = 0) const {
        size_t i = pos + off;
        return i < toks.size() ? toks[i] : toks.back();
    }
    bool check(Tok k) const { return peek().kind == k; }
    bool match(Tok k) {
        if (check(k)) { pos++; return true; }
        return false;
    }
    const Token& advance() { return toks[pos++]; }
    const Token& expect(Tok k, const char* what) {
        if (!check(k))
            throw CompileError(peek().line, std::string("expected ") + what + " but got '" +
                                                (peek().text.empty() ? tok_name(peek().kind)
                                                                     : peek().text) + "'");
        return toks[pos++];
    }
    [[noreturn]] void err(const std::string& m) { throw CompileError(peek().line, m); }

    ExprPtr mk(ExprKind k) {
        auto e = std::make_unique<Expr>();
        e->kind = k;
        e->line = peek().line;
        return e;
    }

    // ---------------- expressions ----------------
    ExprPtr parse_expr() { return parse_or(); }

    ExprPtr parse_or() {
        ExprPtr left = parse_and();
        while (check(Tok::KW_OR)) {
            int line = advance().line;
            auto e = std::make_unique<Expr>();
            e->kind = ExprKind::BoolOp;
            e->line = line;
            e->op = 1;
            e->a = std::move(left);
            e->b = parse_and();
            left = std::move(e);
        }
        return left;
    }

    ExprPtr parse_and() {
        ExprPtr left = parse_not();
        while (check(Tok::KW_AND)) {
            int line = advance().line;
            auto e = std::make_unique<Expr>();
            e->kind = ExprKind::BoolOp;
            e->line = line;
            e->op = 0;
            e->a = std::move(left);
            e->b = parse_not();
            left = std::move(e);
        }
        return left;
    }

    ExprPtr parse_not() {
        if (check(Tok::KW_NOT)) {
            int line = advance().line;
            auto e = std::make_unique<Expr>();
            e->kind = ExprKind::Unary;
            e->line = line;
            e->op = KUOP_NOT;
            e->a = parse_not();
            return e;
        }
        return parse_comparison();
    }

    ExprPtr parse_comparison() {
        ExprPtr left = parse_arith();
        int op = -1;
        bool negate = false;
        switch (peek().kind) {
        case Tok::EQ: op = KOP_EQ; break;
        case Tok::NE: op = KOP_NE; break;
        case Tok::LT: op = KOP_LT; break;
        case Tok::GT: op = KOP_GT; break;
        case Tok::LE: op = KOP_LE; break;
        case Tok::GE: op = KOP_GE; break;
        case Tok::KW_IN: op = KOP_IN; break;
        case Tok::KW_NOT: // "not in"
            if (peek(1).kind == Tok::KW_IN) { op = KOP_IN; negate = true; pos++; }
            break;
        default: break;
        }
        if (op < 0) return left;
        int line = advance().line;
        auto e = std::make_unique<Expr>();
        e->kind = ExprKind::Binary;
        e->line = line;
        e->op = op;
        e->a = std::move(left);
        e->b = parse_arith();
        if (!negate) return e;
        auto n = std::make_unique<Expr>();
        n->kind = ExprKind::Unary;
        n->line = line;
        n->op = KUOP_NOT;
        n->a = std::move(e);
        return n;
    }

    ExprPtr parse_arith() {
        ExprPtr left = parse_term();
        for (;;) {
            int op;
            if (check(Tok::PLUS)) op = KOP_ADD;
            else if (check(Tok::MINUS)) op = KOP_SUB;
            else break;
            int line = advance().line;
            auto e = std::make_unique<Expr>();
            e->kind = ExprKind::Binary;
            e->line = line;
            e->op = op;
            e->a = std::move(left);
            e->b = parse_term();
            left = std::move(e);
        }
        return left;
    }

    ExprPtr parse_term() {
        ExprPtr left = parse_factor();
        for (;;) {
            int op;
            if (check(Tok::STAR)) op = KOP_MUL;
            else if (check(Tok::SLASH)) op = KOP_DIV;
            else if (check(Tok::DSLASH)) op = KOP_FLOORDIV;
            else if (check(Tok::PERCENT)) op = KOP_MOD;
            else break;
            int line = advance().line;
            auto e = std::make_unique<Expr>();
            e->kind = ExprKind::Binary;
            e->line = line;
            e->op = op;
            e->a = std::move(left);
            e->b = parse_factor();
            left = std::move(e);
        }
        return left;
    }

    ExprPtr parse_factor() {
        if (check(Tok::MINUS)) {
            int line = advance().line;
            auto e = std::make_unique<Expr>();
            e->kind = ExprKind::Unary;
            e->line = line;
            e->op = KUOP_NEG;
            e->a = parse_factor();
            return e;
        }
        if (check(Tok::PLUS)) { advance(); return parse_factor(); }
        return parse_postfix();
    }

    ExprPtr parse_postfix() {
        ExprPtr e = parse_atom();
        for (;;) {
            if (check(Tok::LPAREN)) {
                int line = advance().line;
                std::vector<ExprPtr> args;
                if (!check(Tok::RPAREN)) {
                    do { args.push_back(parse_expr()); } while (match(Tok::COMMA));
                }
                expect(Tok::RPAREN, "')'");
                if (args.size() > 16) throw CompileError(line, "too many arguments (max 16)");
                if (e->kind == ExprKind::Attr) {
                    auto c = std::make_unique<Expr>();
                    c->kind = ExprKind::MethodCall;
                    c->line = line;
                    c->sval = e->sval;   // method / module-attr name
                    c->a = std::move(e->a);
                    c->args = std::move(args);
                    e = std::move(c);
                } else {
                    auto c = std::make_unique<Expr>();
                    c->kind = ExprKind::Call;
                    c->line = line;
                    c->a = std::move(e);
                    c->args = std::move(args);
                    e = std::move(c);
                }
            } else if (check(Tok::LBRACKET)) {
                int line = advance().line;
                auto c = std::make_unique<Expr>();
                c->kind = ExprKind::Index;
                c->line = line;
                c->a = std::move(e);
                c->b = parse_expr();
                expect(Tok::RBRACKET, "']'");
                e = std::move(c);
            } else if (check(Tok::DOT)) {
                int line = advance().line;
                const Token& n = expect(Tok::NAME, "attribute name");
                auto c = std::make_unique<Expr>();
                c->kind = ExprKind::Attr;
                c->line = line;
                c->sval = n.text;
                c->a = std::move(e);
                e = std::move(c);
            } else {
                break;
            }
        }
        return e;
    }

    ExprPtr parse_atom() {
        const Token& t = peek();
        switch (t.kind) {
        case Tok::INT: {
            auto e = mk(ExprKind::IntLit);
            e->ival = t.ival;
            advance();
            return e;
        }
        case Tok::FLOAT: {
            auto e = mk(ExprKind::FloatLit);
            e->fval = t.fval;
            advance();
            return e;
        }
        case Tok::STRING: {
            auto e = mk(ExprKind::StrLit);
            e->sval = t.text;
            advance();
            return e;
        }
        case Tok::KW_TRUE:
        case Tok::KW_FALSE: {
            auto e = mk(ExprKind::BoolLit);
            e->ival = t.kind == Tok::KW_TRUE ? 1 : 0;
            advance();
            return e;
        }
        case Tok::KW_NONE: {
            auto e = mk(ExprKind::NoneLit);
            advance();
            return e;
        }
        case Tok::NAME: {
            auto e = mk(ExprKind::Name);
            e->sval = t.text;
            advance();
            return e;
        }
        case Tok::LPAREN: {
            advance();
            ExprPtr e = parse_expr();
            expect(Tok::RPAREN, "')'");
            return e;
        }
        case Tok::LBRACKET: {
            auto e = mk(ExprKind::ListLit);
            advance();
            if (!check(Tok::RBRACKET)) {
                do {
                    if (check(Tok::RBRACKET)) break; // trailing comma
                    e->args.push_back(parse_expr());
                } while (match(Tok::COMMA));
            }
            expect(Tok::RBRACKET, "']'");
            if (e->args.size() > 16) err("list literal too long (max 16 items; use append)");
            return e;
        }
        case Tok::LBRACE: {
            auto e = mk(ExprKind::MapLit);
            advance();
            if (!check(Tok::RBRACE)) {
                do {
                    if (check(Tok::RBRACE)) break;
                    ExprPtr k = parse_expr();
                    expect(Tok::COLON, "':'");
                    ExprPtr v = parse_expr();
                    e->pairs.emplace_back(std::move(k), std::move(v));
                } while (match(Tok::COMMA));
            }
            expect(Tok::RBRACE, "'}'");
            return e;
        }
        default:
            err(std::string("unexpected '") +
                (t.text.empty() ? tok_name(t.kind) : t.text) + "' in expression");
        }
    }

    // ---------------- statements ----------------
    StmtPtr mks(StmtKind k, int line) {
        auto s = std::make_unique<Stmt>();
        s->kind = k;
        s->line = line;
        return s;
    }

    std::vector<StmtPtr> parse_block() {
        expect(Tok::NEWLINE, "newline");
        expect(Tok::INDENT, "an indented block");
        std::vector<StmtPtr> body;
        while (!check(Tok::DEDENT) && !check(Tok::END)) body.push_back(parse_stmt());
        expect(Tok::DEDENT, "dedent");
        return body;
    }

    StmtPtr parse_stmt() {
        const Token& t = peek();
        switch (t.kind) {
        case Tok::KW_IF: return parse_if();
        case Tok::KW_WHILE: return parse_while();
        case Tok::KW_FOR: return parse_for();
        case Tok::KW_DEF: return parse_def();
        case Tok::KW_RETURN: {
            if (func_depth == 0) err("'return' outside function");
            int line = advance().line;
            auto s = mks(StmtKind::Return, line);
            if (!check(Tok::NEWLINE)) s->e1 = parse_expr();
            expect(Tok::NEWLINE, "newline");
            return s;
        }
        case Tok::KW_BREAK: {
            if (loop_depth == 0) err("'break' outside loop");
            int line = advance().line;
            expect(Tok::NEWLINE, "newline");
            return mks(StmtKind::Break, line);
        }
        case Tok::KW_CONTINUE: {
            if (loop_depth == 0) err("'continue' outside loop");
            int line = advance().line;
            expect(Tok::NEWLINE, "newline");
            return mks(StmtKind::Continue, line);
        }
        case Tok::KW_PASS: {
            int line = advance().line;
            expect(Tok::NEWLINE, "newline");
            return mks(StmtKind::Pass, line);
        }
        case Tok::KW_IMPORT: {
            int line = advance().line;
            const Token& n = expect(Tok::NAME, "module name");
            expect(Tok::NEWLINE, "newline");
            auto s = mks(StmtKind::Import, line);
            s->name = n.text;
            return s;
        }
        default: return parse_simple();
        }
    }

    StmtPtr parse_if() {
        int line = advance().line; // 'if' / 'elif'
        auto s = mks(StmtKind::If, line);
        s->e1 = parse_expr();
        expect(Tok::COLON, "':'");
        s->body = parse_block();
        if (check(Tok::KW_ELIF)) {
            s->orelse.push_back(parse_if()); // desugar elif → nested if
        } else if (match(Tok::KW_ELSE)) {
            expect(Tok::COLON, "':'");
            s->orelse = parse_block();
        }
        return s;
    }

    StmtPtr parse_while() {
        int line = advance().line;
        auto s = mks(StmtKind::While, line);
        s->e1 = parse_expr();
        expect(Tok::COLON, "':'");
        loop_depth++;
        s->body = parse_block();
        loop_depth--;
        return s;
    }

    StmtPtr parse_for() {
        int line = advance().line;
        auto s = mks(StmtKind::For, line);
        s->name = expect(Tok::NAME, "loop variable").text;
        expect(Tok::KW_IN, "'in'");
        s->e1 = parse_expr();
        expect(Tok::COLON, "':'");
        loop_depth++;
        s->body = parse_block();
        loop_depth--;
        return s;
    }

    StmtPtr parse_def() {
        int line = advance().line;
        if (func_depth > 0) throw CompileError(line, "nested functions are not supported");
        auto s = mks(StmtKind::FuncDef, line);
        s->name = expect(Tok::NAME, "function name").text;
        expect(Tok::LPAREN, "'('");
        if (!check(Tok::RPAREN)) {
            do { s->params.push_back(expect(Tok::NAME, "parameter name").text); }
            while (match(Tok::COMMA));
        }
        expect(Tok::RPAREN, "')'");
        if (s->params.size() > 16) throw CompileError(line, "too many parameters (max 16)");
        expect(Tok::COLON, "':'");
        func_depth++;
        int save_loops = loop_depth;
        loop_depth = 0;
        s->body = parse_block();
        loop_depth = save_loops;
        func_depth--;
        return s;
    }

    // assignment / expression statement
    StmtPtr parse_simple() {
        int line = peek().line;
        ExprPtr e = parse_expr();
        Tok k = peek().kind;
        if (k == Tok::ASSIGN || k == Tok::PLUSEQ || k == Tok::MINUSEQ || k == Tok::STAREQ ||
            k == Tok::SLASHEQ) {
            advance();
            ExprPtr value = parse_expr();
            expect(Tok::NEWLINE, "newline");
            if (k != Tok::ASSIGN) { // desugar augmented assignment: x += v → x = x + v
                int op = k == Tok::PLUSEQ    ? KOP_ADD
                         : k == Tok::MINUSEQ ? KOP_SUB
                         : k == Tok::STAREQ  ? KOP_MUL
                                             : KOP_DIV;
                auto lhs_copy = clone_expr(e.get());
                auto bin = std::make_unique<Expr>();
                bin->kind = ExprKind::Binary;
                bin->line = line;
                bin->op = op;
                bin->a = std::move(lhs_copy);
                bin->b = std::move(value);
                value = std::move(bin);
            }
            if (e->kind == ExprKind::Name) {
                auto s = mks(StmtKind::Assign, line);
                s->name = e->sval;
                s->e1 = std::move(value);
                return s;
            }
            if (e->kind == ExprKind::Index) {
                auto s = mks(StmtKind::IndexAssign, line);
                s->e1 = std::move(e->a);
                s->e2 = std::move(e->b);
                s->e3 = std::move(value);
                return s;
            }
            throw CompileError(line, "invalid assignment target");
        }
        expect(Tok::NEWLINE, "newline");
        auto s = mks(StmtKind::ExprStmt, line);
        s->e1 = std::move(e);
        return s;
    }

    static ExprPtr clone_expr(const Expr* e) {
        auto c = std::make_unique<Expr>();
        c->kind = e->kind;
        c->line = e->line;
        c->ival = e->ival;
        c->fval = e->fval;
        c->sval = e->sval;
        c->op = e->op;
        if (e->a) c->a = clone_expr(e->a.get());
        if (e->b) c->b = clone_expr(e->b.get());
        for (auto& a : e->args) c->args.push_back(clone_expr(a.get()));
        for (auto& p : e->pairs)
            c->pairs.emplace_back(clone_expr(p.first.get()), clone_expr(p.second.get()));
        return c;
    }

    Module run() {
        Module m;
        while (!check(Tok::END)) {
            // tolerate stray layout tokens at top level
            if (match(Tok::NEWLINE)) continue;
            m.body.push_back(parse_stmt());
        }
        return m;
    }
};

} // namespace

Module parse(std::vector<Token> tokens) {
    Parser p(std::move(tokens));
    return p.run();
}

// ---------------- AST dump (for tests) ----------------
static const char* binop_name(int op) {
    switch (op) {
    case KOP_ADD: return "+";
    case KOP_SUB: return "-";
    case KOP_MUL: return "*";
    case KOP_DIV: return "/";
    case KOP_FLOORDIV: return "//";
    case KOP_MOD: return "%";
    case KOP_EQ: return "==";
    case KOP_NE: return "!=";
    case KOP_LT: return "<";
    case KOP_GT: return ">";
    case KOP_LE: return "<=";
    case KOP_GE: return ">=";
    case KOP_IN: return "in";
    default: return "?";
    }
}

std::string dump_expr(const Expr* e) {
    switch (e->kind) {
    case ExprKind::IntLit: return std::to_string(e->ival);
    case ExprKind::FloatLit: {
        std::string s = std::to_string(e->fval);
        return s;
    }
    case ExprKind::StrLit: return "\"" + e->sval + "\"";
    case ExprKind::BoolLit: return e->ival ? "True" : "False";
    case ExprKind::NoneLit: return "None";
    case ExprKind::Name: return e->sval;
    case ExprKind::Binary:
        return std::string("(") + binop_name(e->op) + " " + dump_expr(e->a.get()) + " " +
               dump_expr(e->b.get()) + ")";
    case ExprKind::Unary:
        return std::string("(") + (e->op == KUOP_NEG ? "neg " : "not ") + dump_expr(e->a.get()) +
               ")";
    case ExprKind::BoolOp:
        return std::string("(") + (e->op ? "or " : "and ") + dump_expr(e->a.get()) + " " +
               dump_expr(e->b.get()) + ")";
    case ExprKind::Call: {
        std::string s = "(call " + dump_expr(e->a.get());
        for (auto& a : e->args) s += " " + dump_expr(a.get());
        return s + ")";
    }
    case ExprKind::MethodCall: {
        std::string s = "(method " + dump_expr(e->a.get()) + " ." + e->sval;
        for (auto& a : e->args) s += " " + dump_expr(a.get());
        return s + ")";
    }
    case ExprKind::Index:
        return "(index " + dump_expr(e->a.get()) + " " + dump_expr(e->b.get()) + ")";
    case ExprKind::Attr: return "(attr " + dump_expr(e->a.get()) + " ." + e->sval + ")";
    case ExprKind::ListLit: {
        std::string s = "(list";
        for (auto& a : e->args) s += " " + dump_expr(a.get());
        return s + ")";
    }
    case ExprKind::MapLit: {
        std::string s = "(map";
        for (auto& p : e->pairs)
            s += " (" + dump_expr(p.first.get()) + " " + dump_expr(p.second.get()) + ")";
        return s + ")";
    }
    }
    return "?";
}

std::string dump_stmt(const Stmt* s, int indent) {
    std::string pad(indent * 2, ' ');
    auto block = [&](const std::vector<StmtPtr>& b) {
        std::string r;
        for (auto& st : b) r += dump_stmt(st.get(), indent + 1);
        return r;
    };
    switch (s->kind) {
    case StmtKind::ExprStmt: return pad + "(expr " + dump_expr(s->e1.get()) + ")\n";
    case StmtKind::Assign:
        return pad + "(= " + s->name + " " + dump_expr(s->e1.get()) + ")\n";
    case StmtKind::IndexAssign:
        return pad + "(setindex " + dump_expr(s->e1.get()) + " " + dump_expr(s->e2.get()) + " " +
               dump_expr(s->e3.get()) + ")\n";
    case StmtKind::If: {
        std::string r = pad + "(if " + dump_expr(s->e1.get()) + "\n" + block(s->body);
        if (!s->orelse.empty()) r += pad + " else\n" + block(s->orelse);
        return r + pad + ")\n";
    }
    case StmtKind::While:
        return pad + "(while " + dump_expr(s->e1.get()) + "\n" + block(s->body) + pad + ")\n";
    case StmtKind::For:
        return pad + "(for " + s->name + " " + dump_expr(s->e1.get()) + "\n" + block(s->body) +
               pad + ")\n";
    case StmtKind::FuncDef: {
        std::string r = pad + "(def " + s->name + " (";
        for (size_t i = 0; i < s->params.size(); i++)
            r += (i ? " " : "") + s->params[i];
        return r + ")\n" + block(s->body) + pad + ")\n";
    }
    case StmtKind::Return:
        return pad + "(return" + (s->e1 ? " " + dump_expr(s->e1.get()) : "") + ")\n";
    case StmtKind::Break: return pad + "(break)\n";
    case StmtKind::Continue: return pad + "(continue)\n";
    case StmtKind::Pass: return pad + "(pass)\n";
    case StmtKind::Import: return pad + "(import " + s->name + ")\n";
    }
    return pad + "?\n";
}

std::string dump_module(const Module& m) {
    std::string r;
    for (auto& s : m.body) r += dump_stmt(s.get());
    return r;
}

} // namespace kami
