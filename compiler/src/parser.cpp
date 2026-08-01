#include "parser.h"

#include "../../runtime/include/kami_runtime.h"

namespace kami {

namespace {

struct Parser {
    std::vector<Token> toks;
    size_t pos = 0;
    int func_depth = 0;
    int loop_depth = 0;
    int class_depth = 0;

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
    ExprPtr mkbin(int op, int line, ExprPtr a, ExprPtr b) {
        auto e = std::make_unique<Expr>();
        e->kind = ExprKind::Binary;
        e->line = line;
        e->op = op;
        e->a = std::move(a);
        e->b = std::move(b);
        return e;
    }

    static ExprPtr clone_expr(const Expr* e) {
        auto c = std::make_unique<Expr>();
        c->kind = e->kind;
        c->line = e->line;
        c->ival = e->ival;
        c->fval = e->fval;
        c->sval = e->sval;
        c->op = e->op;
        c->params = e->params;
        if (e->a) c->a = clone_expr(e->a.get());
        if (e->b) c->b = clone_expr(e->b.get());
        if (e->c) c->c = clone_expr(e->c.get());
        for (auto& a : e->args) c->args.push_back(a ? clone_expr(a.get()) : nullptr);
        for (auto& k : e->kwargs) c->kwargs.emplace_back(k.first, clone_expr(k.second.get()));
        for (auto& p : e->pairs)
            c->pairs.emplace_back(clone_expr(p.first.get()), clone_expr(p.second.get()));
        return c;
    }

    // ---------------- expressions ----------------
    ExprPtr parse_expr() { return parse_ternary(); }

    ExprPtr parse_ternary() {
        ExprPtr v = parse_or();
        if (!check(Tok::KW_IF)) return v;
        int line = advance().line;
        ExprPtr cond = parse_or();
        expect(Tok::KW_ELSE, "'else' in conditional expression");
        ExprPtr els = parse_ternary();
        auto e = std::make_unique<Expr>();
        e->kind = ExprKind::IfExp;
        e->line = line;
        e->a = std::move(v);
        e->b = std::move(cond);
        e->c = std::move(els);
        return e;
    }

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

    // returns op or -1; negate for "not in" / "is not"
    int comp_op(bool& negate) {
        negate = false;
        switch (peek().kind) {
        case Tok::EQ: return KOP_EQ;
        case Tok::NE: return KOP_NE;
        case Tok::LT: return KOP_LT;
        case Tok::GT: return KOP_GT;
        case Tok::LE: return KOP_LE;
        case Tok::GE: return KOP_GE;
        case Tok::KW_IN: return KOP_IN;
        case Tok::KW_IS:
            if (peek(1).kind == Tok::KW_NOT) { pos++; return KOP_ISNOT; }
            return KOP_IS;
        case Tok::KW_NOT:
            if (peek(1).kind == Tok::KW_IN) { negate = true; pos++; return KOP_IN; }
            return -1;
        default: return -1;
        }
    }

    ExprPtr parse_comparison() {
        ExprPtr left = parse_bitor();
        bool neg1;
        int op = comp_op(neg1);
        if (op < 0) return left;
        int line = advance().line;
        ExprPtr right = parse_bitor();

        auto wrap_neg = [&](ExprPtr x, bool neg, int ln) -> ExprPtr {
            if (!neg) return x;
            auto n = std::make_unique<Expr>();
            n->kind = ExprKind::Unary;
            n->line = ln;
            n->op = KUOP_NOT;
            n->a = std::move(x);
            return n;
        };

        ExprPtr result = wrap_neg(mkbin(op, line, std::move(left), clone_expr(right.get())), neg1, line);
        // chained comparisons: a < b < c  →  (a < b) and (b < c)
        for (;;) {
            bool negN;
            int opN = comp_op(negN);
            if (opN < 0) break;
            int lnN = advance().line;
            ExprPtr next = parse_bitor();
            ExprPtr pair = wrap_neg(
                mkbin(opN, lnN, std::move(right), clone_expr(next.get())), negN, lnN);
            right = std::move(next);
            auto conj = std::make_unique<Expr>();
            conj->kind = ExprKind::BoolOp;
            conj->line = lnN;
            conj->op = 0; // and
            conj->a = std::move(result);
            conj->b = std::move(pair);
            result = std::move(conj);
        }
        return result;
    }

    ExprPtr parse_bitor() {
        ExprPtr left = parse_bitxor();
        while (check(Tok::PIPE)) {
            int line = advance().line;
            left = mkbin(KOP_BITOR, line, std::move(left), parse_bitxor());
        }
        return left;
    }

    ExprPtr parse_bitxor() {
        ExprPtr left = parse_bitand();
        while (check(Tok::CARET)) {
            int line = advance().line;
            left = mkbin(KOP_BITXOR, line, std::move(left), parse_bitand());
        }
        return left;
    }

    ExprPtr parse_bitand() {
        ExprPtr left = parse_shift();
        while (check(Tok::AMP)) {
            int line = advance().line;
            left = mkbin(KOP_BITAND, line, std::move(left), parse_shift());
        }
        return left;
    }

    ExprPtr parse_shift() {
        ExprPtr left = parse_arith();
        for (;;) {
            int op;
            if (check(Tok::LSHIFT)) op = KOP_SHL;
            else if (check(Tok::RSHIFT)) op = KOP_SHR;
            else break;
            int line = advance().line;
            left = mkbin(op, line, std::move(left), parse_arith());
        }
        return left;
    }

    ExprPtr parse_arith() {
        ExprPtr left = parse_term();
        for (;;) {
            int op;
            if (check(Tok::PLUS)) op = KOP_ADD;
            else if (check(Tok::MINUS)) op = KOP_SUB;
            else break;
            int line = advance().line;
            left = mkbin(op, line, std::move(left), parse_term());
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
            left = mkbin(op, line, std::move(left), parse_factor());
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
        if (check(Tok::TILDE)) {
            int line = advance().line;
            auto e = std::make_unique<Expr>();
            e->kind = ExprKind::Unary;
            e->line = line;
            e->op = KUOP_INV;
            e->a = parse_factor();
            return e;
        }
        return parse_power();
    }

    ExprPtr parse_power() {
        ExprPtr base = parse_postfix();
        if (check(Tok::POW)) {
            int line = advance().line;
            return mkbin(KOP_POW, line, std::move(base), parse_factor()); // right-assoc
        }
        return base;
    }

    // call argument list after '('
    void parse_call_args(Expr* call) {
        if (!check(Tok::RPAREN)) {
            do {
                if (check(Tok::RPAREN)) break; // trailing comma
                if (check(Tok::STAR) || check(Tok::POW)) {
                    // f(*seq) spreads positionally, f(**mapping) as keywords.
                    bool dbl = check(Tok::POW);
                    int line = advance().line;
                    auto sp = std::make_unique<Expr>();
                    sp->kind = ExprKind::Starred;
                    sp->line = line;
                    sp->ival = dbl ? 2 : 1;
                    sp->a = parse_expr();
                    call->args.push_back(std::move(sp));
                    continue;
                }
                if (check(Tok::NAME) && peek(1).kind == Tok::ASSIGN) {
                    std::string kw = advance().text;
                    advance(); // '='
                    call->kwargs.emplace_back(kw, parse_expr());
                    continue;
                }
                ExprPtr a = parse_expr();
                if (check(Tok::KW_FOR)) { // generator expression argument
                    a = parse_comprehension_tail(std::move(a));
                }
                call->args.push_back(std::move(a));
            } while (match(Tok::COMMA));
        }
        expect(Tok::RPAREN, "')'");
        if (call->args.size() + call->kwargs.size() > 16)
            throw CompileError(call->line, "too many arguments (max 16)");
    }

    // Parse one or more 'for ... in ... [if ...]' clauses into `clauses`.
    void parse_comp_clauses(std::vector<CompClause>& clauses) {
        while (check(Tok::KW_FOR)) {
            advance();
            CompClause cl;
            // targets: NAME [, NAME]*  or  ( NAME [, NAME]* )
            bool paren = match(Tok::LPAREN);
            cl.targets.push_back(expect(Tok::NAME, "comprehension target").text);
            while (match(Tok::COMMA)) {
                if (paren && check(Tok::RPAREN)) break;
                cl.targets.push_back(expect(Tok::NAME, "comprehension target").text);
            }
            if (paren) expect(Tok::RPAREN, "')'");
            expect(Tok::KW_IN, "'in'");
            cl.iter = parse_or();
            while (check(Tok::KW_IF)) {
                advance();
                cl.conds.push_back(parse_or());
            }
            clauses.push_back(std::move(cl));
        }
    }

    ExprPtr parse_comprehension_tail(ExprPtr element) {
        auto e = std::make_unique<Expr>();
        e->kind = ExprKind::ListComp;
        e->line = peek().line;
        e->a = std::move(element);
        parse_comp_clauses(e->clauses);
        return e;
    }

    ExprPtr parse_postfix() {
        ExprPtr e = parse_atom();
        for (;;) {
            if (check(Tok::LPAREN)) {
                int line = advance().line;
                if (e->kind == ExprKind::Attr) {
                    auto c = std::make_unique<Expr>();
                    c->kind = ExprKind::MethodCall;
                    c->line = line;
                    c->sval = e->sval;
                    c->a = std::move(e->a);
                    parse_call_args(c.get());
                    e = std::move(c);
                } else {
                    auto c = std::make_unique<Expr>();
                    c->kind = ExprKind::Call;
                    c->line = line;
                    c->a = std::move(e);
                    parse_call_args(c.get());
                    e = std::move(c);
                }
            } else if (check(Tok::LBRACKET)) {
                int line = advance().line;
                // index or slice
                ExprPtr start, stop, step;
                bool is_slice = false;
                if (!check(Tok::COLON)) start = parse_expr();
                if (start && check(Tok::COMMA)) { // a[x, y] — tuple index
                    auto lst = std::make_unique<Expr>();
                    lst->kind = ExprKind::ListLit;
                    lst->line = line;
                    lst->args.push_back(std::move(start));
                    while (match(Tok::COMMA)) {
                        if (check(Tok::RBRACKET)) break;
                        lst->args.push_back(parse_expr());
                    }
                    start = std::move(lst);
                }
                if (check(Tok::COLON)) {
                    is_slice = true;
                    advance();
                    if (!check(Tok::COLON) && !check(Tok::RBRACKET)) stop = parse_expr();
                    if (match(Tok::COLON)) {
                        if (!check(Tok::RBRACKET)) step = parse_expr();
                    }
                }
                expect(Tok::RBRACKET, "']'");
                if (is_slice) {
                    auto c = std::make_unique<Expr>();
                    c->kind = ExprKind::Slice;
                    c->line = line;
                    c->a = std::move(e);
                    c->args.push_back(std::move(start));
                    c->args.push_back(std::move(stop));
                    c->args.push_back(std::move(step));
                    e = std::move(c);
                } else {
                    auto c = std::make_unique<Expr>();
                    c->kind = ExprKind::Index;
                    c->line = line;
                    c->a = std::move(e);
                    c->b = std::move(start);
                    e = std::move(c);
                }
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

    ExprPtr parse_starred_or_expr() {
        if (check(Tok::STAR)) {
            int line = advance().line;
            auto e = std::make_unique<Expr>();
            e->kind = ExprKind::Starred;
            e->line = line;
            e->a = parse_expr();
            return e;
        }
        return parse_expr();
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
            while (check(Tok::STRING)) { // implicit concatenation: "a" "b"
                e->sval += peek().text;
                advance();
            }
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
        case Tok::ELLIPSIS: {
            // Ellipsis literal: modeled as None (used in stubs / annotations).
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
        case Tok::KW_LAMBDA: {
            int line = advance().line;
            auto e = std::make_unique<Expr>();
            e->kind = ExprKind::Lambda;
            e->line = line;
            if (!check(Tok::COLON)) {
                do {
                    if (check(Tok::COLON)) break;
                    e->params.push_back(expect(Tok::NAME, "lambda parameter").text);
                } while (match(Tok::COMMA));
            }
            expect(Tok::COLON, "':' in lambda");
            e->a = parse_ternary(); // lambda body is a single expression
            if (e->params.size() > 16) err("too many lambda parameters (max 16)");
            return e;
        }
        case Tok::KW_YIELD: err("generators (yield) are not supported");
        case Tok::LPAREN: {
            int line = advance().line;
            if (check(Tok::RPAREN)) { // empty tuple → empty list
                advance();
                auto e = std::make_unique<Expr>();
                e->kind = ExprKind::ListLit;
                e->line = line;
                return e;
            }
            ExprPtr inner = parse_expr();
            if (check(Tok::KW_FOR)) {
                ExprPtr comp = parse_comprehension_tail(std::move(inner));
                expect(Tok::RPAREN, "')'");
                return comp;
            }
            if (check(Tok::COMMA)) { // tuple → list
                auto e = std::make_unique<Expr>();
                e->kind = ExprKind::ListLit;
                e->line = line;
                e->args.push_back(std::move(inner));
                while (match(Tok::COMMA)) {
                    if (check(Tok::RPAREN)) break;
                    e->args.push_back(parse_expr());
                }
                expect(Tok::RPAREN, "')'");
                return e;
            }
            expect(Tok::RPAREN, "')'");
            return inner;
        }
        case Tok::LBRACKET: {
            auto e = mk(ExprKind::ListLit);
            advance();
            if (check(Tok::RBRACKET)) {
                advance();
                return e;
            }
            ExprPtr first = parse_starred_or_expr();
            if (check(Tok::KW_FOR)) {
                if (first->kind == ExprKind::Starred)
                    err("cannot use * in a comprehension element");
                ExprPtr comp = parse_comprehension_tail(std::move(first));
                expect(Tok::RBRACKET, "']'");
                return comp;
            }
            e->args.push_back(std::move(first));
            while (match(Tok::COMMA)) {
                if (check(Tok::RBRACKET)) break;
                e->args.push_back(parse_starred_or_expr());
            }
            expect(Tok::RBRACKET, "']'");
            return e;
        }
        case Tok::LBRACE: {
            int line = advance().line;
            if (check(Tok::RBRACE)) { // {} = empty dict
                advance();
                auto e = std::make_unique<Expr>();
                e->kind = ExprKind::MapLit;
                e->line = line;
                return e;
            }
            ExprPtr first = parse_expr();
            if (check(Tok::COLON)) { // dict or dict-comprehension
                advance();
                ExprPtr v = parse_expr();
                if (check(Tok::KW_FOR)) {
                    auto e = std::make_unique<Expr>();
                    e->kind = ExprKind::MapComp;
                    e->line = line;
                    e->pairs.emplace_back(std::move(first), std::move(v));
                    parse_comp_clauses(e->clauses);
                    expect(Tok::RBRACE, "'}'");
                    return e;
                }
                auto e = std::make_unique<Expr>();
                e->kind = ExprKind::MapLit;
                e->line = line;
                e->pairs.emplace_back(std::move(first), std::move(v));
                while (match(Tok::COMMA)) {
                    if (check(Tok::RBRACE)) break;
                    ExprPtr k2 = parse_expr();
                    expect(Tok::COLON, "':'");
                    e->pairs.emplace_back(std::move(k2), parse_expr());
                }
                expect(Tok::RBRACE, "'}'");
                return e;
            }
            if (check(Tok::KW_FOR)) { // set comprehension
                auto e = std::make_unique<Expr>();
                e->kind = ExprKind::SetComp;
                e->line = line;
                e->a = std::move(first);
                parse_comp_clauses(e->clauses);
                expect(Tok::RBRACE, "'}'");
                return e;
            }
            auto e = std::make_unique<Expr>();
            e->kind = ExprKind::SetLit;
            e->line = line;
            e->args.push_back(std::move(first));
            while (match(Tok::COMMA)) {
                if (check(Tok::RBRACE)) break;
                e->args.push_back(parse_expr());
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
        if (!check(Tok::NEWLINE)) { // inline body:  if x: do_it()
            std::vector<StmtPtr> body;
            for (;;) {
                body.push_back(parse_small_stmt());
                if (match(Tok::SEMI)) {
                    if (check(Tok::NEWLINE)) break;
                    continue;
                }
                break;
            }
            expect(Tok::NEWLINE, "newline");
            return body;
        }
        expect(Tok::NEWLINE, "newline");
        expect(Tok::INDENT, "an indented block");
        std::vector<StmtPtr> body;
        while (!check(Tok::DEDENT) && !check(Tok::END)) parse_stmt_into(body);
        expect(Tok::DEDENT, "dedent");
        return body;
    }

    void parse_stmt_into(std::vector<StmtPtr>& body) {
        switch (peek().kind) {
        case Tok::KW_IF: body.push_back(parse_if()); return;
        case Tok::KW_WHILE: body.push_back(parse_while()); return;
        case Tok::KW_FOR: body.push_back(parse_for()); return;
        case Tok::KW_DEF: body.push_back(parse_def()); return;
        case Tok::KW_CLASS: body.push_back(parse_class()); return;
        case Tok::KW_TRY: body.push_back(parse_try()); return;
        case Tok::KW_WITH: body.push_back(parse_with()); return;
        case Tok::AT: body.push_back(parse_decorated()); return;
        case Tok::KW_DEL: break;
        case Tok::KW_NONLOCAL: err("'nonlocal' is not supported");
        default: break;
        }
        // one or more simple statements separated by ';'
        for (;;) {
            body.push_back(parse_small_stmt());
            if (match(Tok::SEMI)) {
                if (check(Tok::NEWLINE)) { advance(); return; }
                continue;
            }
            expect(Tok::NEWLINE, "newline");
            return;
        }
    }

    StmtPtr parse_small_stmt() {
        const Token& t = peek();
        switch (t.kind) {
        case Tok::KW_RETURN: {
            if (func_depth == 0) err("'return' outside function");
            int line = advance().line;
            auto s = mks(StmtKind::Return, line);
            if (!check(Tok::NEWLINE) && !check(Tok::SEMI)) {
                ExprPtr v = parse_expr();
                if (check(Tok::COMMA)) { // return a, b → return [a, b]
                    auto lst = std::make_unique<Expr>();
                    lst->kind = ExprKind::ListLit;
                    lst->line = line;
                    lst->args.push_back(std::move(v));
                    while (match(Tok::COMMA)) {
                        if (check(Tok::NEWLINE)) break;
                        lst->args.push_back(parse_expr());
                    }
                    v = std::move(lst);
                }
                s->e1 = std::move(v);
            }
            return s;
        }
        case Tok::KW_BREAK: {
            if (loop_depth == 0) err("'break' outside loop");
            int line = advance().line;
            return mks(StmtKind::Break, line);
        }
        case Tok::KW_CONTINUE: {
            if (loop_depth == 0) err("'continue' outside loop");
            int line = advance().line;
            return mks(StmtKind::Continue, line);
        }
        case Tok::KW_PASS: {
            int line = advance().line;
            return mks(StmtKind::Pass, line);
        }
        case Tok::KW_IMPORT: return parse_import();
        case Tok::KW_FROM: return parse_from_import();
        case Tok::KW_RAISE: {
            int line = advance().line;
            auto s = mks(StmtKind::Raise, line);
            if (!check(Tok::NEWLINE) && !check(Tok::SEMI)) {
                s->e1 = parse_expr();
                if (match(Tok::KW_FROM)) parse_expr(); // raise X from Y: cause ignored
            } else {
                s->raise_mode = 1; // bare re-raise
            }
            return s;
        }
        case Tok::KW_ASSERT: {
            int line = advance().line;
            ExprPtr cond = parse_expr();
            ExprPtr msg;
            if (match(Tok::COMMA)) msg = parse_expr();
            // desugar: if not cond: raise AssertionError(msg)
            auto iff = mks(StmtKind::If, line);
            auto notc = std::make_unique<Expr>();
            notc->kind = ExprKind::Unary;
            notc->line = line;
            notc->op = KUOP_NOT;
            notc->a = std::move(cond);
            iff->e1 = std::move(notc);
            auto rs = mks(StmtKind::Raise, line);
            rs->raise_mode = 2;
            rs->name = "AssertionError";
            rs->e1 = std::move(msg); // may be null
            iff->body.push_back(std::move(rs));
            return iff;
        }
        case Tok::KW_DEL: {
            int line = advance().line;
            auto st = mks(StmtKind::Del, line);
            st->targets.push_back(parse_expr());
            while (match(Tok::COMMA)) {
                if (check(Tok::NEWLINE) || check(Tok::SEMI)) break;
                st->targets.push_back(parse_expr());
            }
            for (auto& t : st->targets)
                if (t->kind != ExprKind::Index && t->kind != ExprKind::Name)
                    throw CompileError(line, "can only 'del' a name or subscript");
            return st;
        }
        case Tok::KW_GLOBAL: {
            int line = advance().line;
            auto s = mks(StmtKind::Global, line);
            s->params.push_back(expect(Tok::NAME, "name").text);
            while (match(Tok::COMMA)) s->params.push_back(expect(Tok::NAME, "name").text);
            return s;
        }
        default: return parse_simple();
        }
    }

    StmtPtr parse_import() {
        int line = advance().line;
        // import a.b [as x] [, c [as y]]*  — only the first module kept per stmt;
        // extra modules become chained Import stmts is not possible here (single
        // return), so we parse them into one stmt list via a wrapper below.
        auto s = mks(StmtKind::Import, line);
        s->name = parse_dotted_name();
        if (match(Tok::KW_AS)) s->alias = expect(Tok::NAME, "alias").text;
        while (match(Tok::COMMA)) {
            // represent extra "import b" as nested Import in body
            auto extra = mks(StmtKind::Import, line);
            extra->name = parse_dotted_name();
            if (match(Tok::KW_AS)) extra->alias = expect(Tok::NAME, "alias").text;
            s->body.push_back(std::move(extra));
        }
        return s;
    }

    std::string parse_dotted_name() {
        std::string n = expect(Tok::NAME, "module name").text;
        while (match(Tok::DOT)) n += "." + expect(Tok::NAME, "module name").text;
        return n;
    }

    StmtPtr parse_from_import() {
        int line = advance().line;
        auto s = mks(StmtKind::FromImport, line);
        // Relative import: one or more leading dots (from . / from .mod / from ..pkg).
        int dots = 0;
        while (check(Tok::DOT) || check(Tok::ELLIPSIS)) {
            dots += check(Tok::ELLIPSIS) ? 3 : 1;
            advance();
        }
        if (dots > 0) s->relative = true;
        if (check(Tok::NAME)) s->name = parse_dotted_name();
        // else: `from . import x` — imported names are sibling modules.
        expect(Tok::KW_IMPORT, "'import'");
        if (check(Tok::STAR)) {
            // `from X import *`: we can't enumerate names for a native/unknown
            // module, but for a bundled local module every top-level name is
            // already a global, so a star import is a harmless no-op there.
            advance();
            s->star = true;
            return s;
        }
        if (match(Tok::LPAREN)) { // parenthesized import list (may span lines)
            do {
                if (check(Tok::RPAREN)) break;
                std::string n = expect(Tok::NAME, "name").text;
                std::string a = n;
                if (match(Tok::KW_AS)) a = expect(Tok::NAME, "alias").text;
                s->import_names.emplace_back(n, a);
            } while (match(Tok::COMMA));
            expect(Tok::RPAREN, "')'");
            return s;
        }
        do {
            std::string n = expect(Tok::NAME, "name").text;
            std::string a = n;
            if (match(Tok::KW_AS)) a = expect(Tok::NAME, "alias").text;
            s->import_names.emplace_back(n, a);
        } while (match(Tok::COMMA));
        return s;
    }

    StmtPtr parse_if() {
        int line = advance().line; // 'if' / 'elif'
        auto s = mks(StmtKind::If, line);
        s->e1 = parse_expr();
        expect(Tok::COLON, "':'");
        s->body = parse_block();
        if (check(Tok::KW_ELIF)) {
            s->orelse.push_back(parse_if());
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
        if (match(Tok::KW_ELSE)) { // while ... else: runs if no break
            expect(Tok::COLON, "':'");
            s->orelse = parse_block();
        }
        return s;
    }

    StmtPtr parse_for() {
        int line = advance().line;
        auto s = mks(StmtKind::For, line);
        s->params.push_back(expect(Tok::NAME, "loop variable").text);
        while (match(Tok::COMMA))
            s->params.push_back(expect(Tok::NAME, "loop variable").text);
        s->name = s->params[0];
        expect(Tok::KW_IN, "'in'");
        s->e1 = parse_expr();
        if (check(Tok::COMMA)) { // for x in a, b, c → iterate a tuple(list)
            auto lst = std::make_unique<Expr>();
            lst->kind = ExprKind::ListLit;
            lst->line = line;
            lst->args.push_back(std::move(s->e1));
            while (match(Tok::COMMA)) lst->args.push_back(parse_expr());
            s->e1 = std::move(lst);
        }
        expect(Tok::COLON, "':'");
        loop_depth++;
        s->body = parse_block();
        loop_depth--;
        if (match(Tok::KW_ELSE)) { // for ... else: runs if no break
            expect(Tok::COLON, "':'");
            s->orelse = parse_block();
        }
        return s;
    }

    StmtPtr parse_with() {
        int line = advance().line; // 'with'
        auto s = mks(StmtKind::With, line);
        s->e1 = parse_expr();
        if (match(Tok::KW_AS)) s->name = expect(Tok::NAME, "name").text;
        // additional context managers: with a as x, b as y:
        std::vector<StmtPtr> extra;
        while (match(Tok::COMMA)) {
            auto w2 = mks(StmtKind::With, line);
            w2->e1 = parse_expr();
            if (match(Tok::KW_AS)) w2->name = expect(Tok::NAME, "name").text;
            extra.push_back(std::move(w2));
        }
        expect(Tok::COLON, "':'");
        std::vector<StmtPtr> body = parse_block();
        // nest extra context managers inside
        for (size_t i = extra.size(); i-- > 0;) {
            extra[i]->body = std::move(body);
            body.clear();
            body.push_back(std::move(extra[i]));
        }
        s->body = std::move(body);
        return s;
    }

    StmtPtr parse_try() {
        int line = advance().line;
        auto s = mks(StmtKind::Try, line);
        expect(Tok::COLON, "':'");
        s->body = parse_block();
        bool any = false;
        while (check(Tok::KW_EXCEPT)) {
            any = true;
            advance();
            ExceptClause h;
            if (!check(Tok::COLON)) {
                parse_expr(); // exception type (or tuple): parsed and ignored
                if (match(Tok::KW_AS)) h.as_name = expect(Tok::NAME, "name").text;
            }
            expect(Tok::COLON, "':'");
            h.body = parse_block();
            s->handlers.push_back(std::move(h));
        }
        if (match(Tok::KW_ELSE)) {
            if (!any) err("'else' requires at least one 'except' clause");
            expect(Tok::COLON, "':'");
            s->orelse = parse_block();
        }
        if (match(Tok::KW_FINALLY)) {
            any = true;
            expect(Tok::COLON, "':'");
            s->final_body = parse_block();
        }
        if (!any) err("'try' requires 'except' or 'finally'");
        return s;
    }

    void skip_type_params() {
        if (!check(Tok::LBRACKET)) return;
        int depth = 0;
        do {
            if (check(Tok::LBRACKET)) depth++;
            else if (check(Tok::RBRACKET)) depth--;
            advance();
        } while (depth > 0 && !check(Tok::END));
    }

    StmtPtr parse_decorated() {
        std::vector<ExprPtr> decos;
        while (check(Tok::AT)) {
            advance();
            decos.push_back(parse_expr());
            expect(Tok::NEWLINE, "newline after decorator");
        }
        StmtPtr target;
        if (check(Tok::KW_DEF)) target = parse_def();
        else if (check(Tok::KW_CLASS)) target = parse_class();
        else err("expected 'def' or 'class' after decorator");
        target->decorators = std::move(decos);
        return target;
    }

    StmtPtr parse_def() {
        int line = advance().line;
        auto s = mks(StmtKind::FuncDef, line);
        s->name = expect(Tok::NAME, "function name").text;
        skip_type_params(); // PEP 695: def f[T](...)
        expect(Tok::LPAREN, "'('");
        bool seen_default = false;
        if (!check(Tok::RPAREN)) {
            do {
                if (check(Tok::RPAREN)) break; // trailing comma
                if (check(Tok::SLASH)) { advance(); continue; } // positional-only marker
                if (check(Tok::POW)) { // **kwargs
                    advance();
                    if (!s->kwarg.empty())
                        err("a function can have only one **kwargs parameter");
                    s->kwarg = expect(Tok::NAME, "parameter name").text;
                    if (match(Tok::COLON)) parse_expr(); // annotation: ignored
                    continue;
                }
                if (check(Tok::STAR)) { // *args, or a bare '*' keyword-only marker
                    advance();
                    // everything declared after this point is keyword-only
                    if (s->nposparams < 0) s->nposparams = (int)s->params.size();
                    if (!check(Tok::NAME)) continue; // bare '*'
                    if (!s->vararg.empty())
                        err("a function can have only one *args parameter");
                    s->vararg = expect(Tok::NAME, "parameter name").text;
                    if (match(Tok::COLON)) parse_expr(); // annotation: ignored
                    continue;
                }
                if (!s->kwarg.empty())
                    err("no parameter may follow **kwargs");
                s->params.push_back(expect(Tok::NAME, "parameter name").text);
                if (match(Tok::COLON)) parse_expr(); // type annotation: ignored
                if (match(Tok::ASSIGN)) {
                    seen_default = true;
                    // Any expression is allowed (e.g. ThreadType.USER, CONST+1):
                    // codegen evaluates it in the function prologue when the
                    // caller omits the argument.
                    s->defaults.push_back(parse_expr());
                } else if (seen_default) {
                    throw CompileError(peek().line,
                                       "non-default parameter after default parameter");
                }
            } while (match(Tok::COMMA));
        }
        expect(Tok::RPAREN, "')'");
        if (s->params.size() > 16) throw CompileError(line, "too many parameters (max 16)");
        if (match(Tok::ARROW)) parse_expr(); // return annotation: ignored
        expect(Tok::COLON, "':'");
        func_depth++;
        int save_loops = loop_depth;
        loop_depth = 0;
        s->body = parse_block();
        loop_depth = save_loops;
        func_depth--;
        return s;
    }


    StmtPtr parse_class() {
        int line = advance().line;
        if (func_depth > 0 || class_depth > 0)
            throw CompileError(line, "nested classes are not supported");
        auto s = mks(StmtKind::ClassDef, line);
        s->name = expect(Tok::NAME, "class name").text;
        skip_type_params(); // PEP 695: class C[T]:
        if (match(Tok::LPAREN)) {
            // Bases may be arbitrary expressions (e.g. unittest.TestCase, tk.Tk).
            // We only model inheritance from a class defined in this file; any
            // other base is parsed and ignored (class created with no base).
            if (!check(Tok::RPAREN)) {
                bool first = true;
                do {
                    if (check(Tok::RPAREN)) break;
                    if (check(Tok::NAME) && peek(1).kind == Tok::ASSIGN) { // metaclass= etc.
                        advance(); advance();
                        parse_expr();
                        continue;
                    }
                    ExprPtr base = parse_expr();
                    if (first && base->kind == ExprKind::Name && base->sval != "object")
                        s->alias = base->sval;
                    first = false;
                } while (match(Tok::COMMA));
            }
            expect(Tok::RPAREN, "')'");
        }
        expect(Tok::COLON, "':'");
        expect(Tok::NEWLINE, "newline");
        expect(Tok::INDENT, "an indented block");
        class_depth++;
        while (!check(Tok::DEDENT) && !check(Tok::END)) {
            if (check(Tok::KW_DEF)) {
                s->body.push_back(parse_def());
            } else if (check(Tok::KW_PASS)) {
                advance();
                expect(Tok::NEWLINE, "newline");
            } else if (check(Tok::STRING)) { // docstring
                advance();
                expect(Tok::NEWLINE, "newline");
            } else if (check(Tok::NAME) &&
                       (peek(1).kind == Tok::ASSIGN || peek(1).kind == Tok::COLON)) {
                // class attribute: NAME [":" type] "=" expr
                auto a = mks(StmtKind::Assign, peek().line);
                a->name = advance().text;
                if (match(Tok::COLON)) parse_expr();
                if (check(Tok::NEWLINE)) { // bare annotation: declares nothing
                    advance();
                    continue;
                }
                expect(Tok::ASSIGN, "'='");
                a->e1 = parse_expr();
                expect(Tok::NEWLINE, "newline");
                s->body.push_back(std::move(a));
            } else if (check(Tok::AT)) {
                s->body.push_back(parse_decorated());
            } else {
                throw CompileError(peek().line,
                                   "only methods and simple attribute assignments are "
                                   "supported in class bodies");
            }
        }
        class_depth--;
        expect(Tok::DEDENT, "dedent");
        return s;
    }

    // assignment / expression statement (NEWLINE/SEMI left unconsumed)
    StmtPtr parse_simple() {
        int line = peek().line;
        ExprPtr e = parse_expr();

        // variable annotation:  x: int [= value]  /  self.x: T = v  /  a[i]: T = v
        if (check(Tok::COLON) && (e->kind == ExprKind::Name || e->kind == ExprKind::Attr ||
                                  e->kind == ExprKind::Index)) {
            advance();
            parse_expr(); // annotation ignored
            if (match(Tok::ASSIGN)) {
                ExprPtr value = parse_rhs_values_as_one(line);
                return make_assign(std::move(e), std::move(value), line);
            }
            return mks(StmtKind::Pass, line); // bare annotation declares nothing
        }

        // multi-target tuple assignment:  a, b = ...
        if (check(Tok::COMMA)) {
            auto s = mks(StmtKind::MultiAssign, line);
            s->targets.push_back(std::move(e));
            while (match(Tok::COMMA)) {
                if (check(Tok::ASSIGN)) break;
                s->targets.push_back(parse_starred_or_expr());
            }
            expect(Tok::ASSIGN, "'='");
            s->values.push_back(parse_expr());
            while (match(Tok::COMMA)) {
                if (check(Tok::NEWLINE) || check(Tok::SEMI)) break;
                s->values.push_back(parse_expr());
            }
            for (auto& t2 : s->targets) check_target(t2.get());
            if (s->values.size() > 1 && s->values.size() != s->targets.size())
                throw CompileError(line, "unbalanced tuple assignment");
            return s;
        }

        Tok k = peek().kind;
        if (k == Tok::ASSIGN) {
            advance();
            ExprPtr value = parse_expr();
            // chained assignment: a = b = value
            if (check(Tok::ASSIGN)) {
                auto s = mks(StmtKind::MultiAssign, line);
                s->alias = "chain";
                check_target(e.get());
                s->targets.push_back(std::move(e));
                while (match(Tok::ASSIGN)) {
                    check_target(value.get());
                    s->targets.push_back(std::move(value));
                    value = parse_expr();
                }
                s->values.push_back(std::move(value));
                return s;
            }
            // tuple RHS: x = 1, 2 → x = [1, 2]
            if (check(Tok::COMMA)) {
                auto lst = std::make_unique<Expr>();
                lst->kind = ExprKind::ListLit;
                lst->line = line;
                lst->args.push_back(std::move(value));
                while (match(Tok::COMMA)) {
                    if (check(Tok::NEWLINE) || check(Tok::SEMI)) break;
                    lst->args.push_back(parse_expr());
                }
                value = std::move(lst);
            }
            return make_assign(std::move(e), std::move(value), line);
        }
        int aug = -1;
        switch (k) {
        case Tok::PLUSEQ: aug = KOP_ADD; break;
        case Tok::MINUSEQ: aug = KOP_SUB; break;
        case Tok::STAREQ: aug = KOP_MUL; break;
        case Tok::SLASHEQ: aug = KOP_DIV; break;
        case Tok::DSLASHEQ: aug = KOP_FLOORDIV; break;
        case Tok::PERCENTEQ: aug = KOP_MOD; break;
        case Tok::POWEQ: aug = KOP_POW; break;
        case Tok::AMPEQ: aug = KOP_BITAND; break;
        case Tok::PIPEEQ: aug = KOP_BITOR; break;
        case Tok::CARETEQ: aug = KOP_BITXOR; break;
        case Tok::LSHIFTEQ: aug = KOP_SHL; break;
        case Tok::RSHIFTEQ: aug = KOP_SHR; break;
        default: break;
        }
        if (aug >= 0) {
            advance();
            ExprPtr rhs = parse_expr();
            ExprPtr lhs_copy = clone_expr(e.get());
            ExprPtr value = mkbin(aug, line, std::move(lhs_copy), std::move(rhs));
            return make_assign(std::move(e), std::move(value), line);
        }
        auto s = mks(StmtKind::ExprStmt, line);
        s->e1 = std::move(e);
        return s;
    }

    ExprPtr parse_rhs_values_as_one(int line) {
        ExprPtr v = parse_expr();
        if (check(Tok::COMMA)) {
            auto lst = std::make_unique<Expr>();
            lst->kind = ExprKind::ListLit;
            lst->line = line;
            lst->args.push_back(std::move(v));
            while (match(Tok::COMMA)) {
                if (check(Tok::NEWLINE) || check(Tok::SEMI)) break;
                lst->args.push_back(parse_expr());
            }
            return lst;
        }
        return v;
    }

    void check_target(const Expr* e) {
        // `a, *rest = seq` — a starred target absorbs the middle of the sequence.
        if (e->kind == ExprKind::Starred) {
            check_target(e->a.get());
            return;
        }
        if (e->kind != ExprKind::Name && e->kind != ExprKind::Index &&
            e->kind != ExprKind::Attr)
            throw CompileError(e->line, "invalid assignment target");
    }

    StmtPtr make_assign(ExprPtr target, ExprPtr value, int line) {
        if (target->kind == ExprKind::ListLit) {
            auto s = mks(StmtKind::MultiAssign, line);
            for (auto& t : target->args) {
                if (t->kind != ExprKind::Name && t->kind != ExprKind::Index &&
                    t->kind != ExprKind::Attr)
                    throw CompileError(line, "unsupported unpacking target");
                s->targets.push_back(std::move(t));
            }
            s->values.push_back(std::move(value));
            return s;
        }
        if (target->kind == ExprKind::Name) {
            auto s = mks(StmtKind::Assign, line);
            s->name = target->sval;
            s->e1 = std::move(value);
            return s;
        }
        if (target->kind == ExprKind::Index) {
            auto s = mks(StmtKind::IndexAssign, line);
            s->e1 = std::move(target->a);
            s->e2 = std::move(target->b);
            s->e3 = std::move(value);
            return s;
        }
        if (target->kind == ExprKind::Attr) {
            auto s = mks(StmtKind::AttrAssign, line);
            s->e1 = std::move(target->a);
            s->name = target->sval;
            s->e3 = std::move(value);
            return s;
        }
        throw CompileError(line, "invalid assignment target");
    }

    Module run() {
        Module m;
        while (!check(Tok::END)) {
            if (match(Tok::NEWLINE)) continue;
            parse_stmt_into(m.body);
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
    case KOP_POW: return "**";
    case KOP_IS: return "is";
    case KOP_ISNOT: return "is-not";
    case KOP_BITAND: return "&";
    case KOP_BITOR: return "|";
    case KOP_BITXOR: return "^";
    case KOP_SHL: return "<<";
    case KOP_SHR: return ">>";
    default: return "?";
    }
}

std::string dump_expr(const Expr* e) {
    switch (e->kind) {
    case ExprKind::IntLit: return std::to_string(e->ival);
    case ExprKind::FloatLit: return std::to_string(e->fval);
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
    case ExprKind::IfExp:
        return "(ifexp " + dump_expr(e->b.get()) + " " + dump_expr(e->a.get()) + " " +
               dump_expr(e->c.get()) + ")";
    case ExprKind::Call: {
        std::string s = "(call " + dump_expr(e->a.get());
        for (auto& a : e->args) s += " " + dump_expr(a.get());
        for (auto& kv : e->kwargs) s += " " + kv.first + "=" + dump_expr(kv.second.get());
        return s + ")";
    }
    case ExprKind::MethodCall: {
        std::string s = "(method " + dump_expr(e->a.get()) + " ." + e->sval;
        for (auto& a : e->args) s += " " + dump_expr(a.get());
        for (auto& kv : e->kwargs) s += " " + kv.first + "=" + dump_expr(kv.second.get());
        return s + ")";
    }
    case ExprKind::Index:
        return "(index " + dump_expr(e->a.get()) + " " + dump_expr(e->b.get()) + ")";
    case ExprKind::Slice: {
        std::string s = "(slice " + dump_expr(e->a.get());
        for (auto& p : e->args) s += " " + (p ? dump_expr(p.get()) : std::string("_"));
        return s + ")";
    }
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
    case ExprKind::SetLit: {
        std::string s = "(set";
        for (auto& a : e->args) s += " " + dump_expr(a.get());
        return s + ")";
    }
    case ExprKind::Lambda: {
        std::string s = "(lambda (";
        for (size_t i = 0; i < e->params.size(); i++) s += (i ? " " : "") + e->params[i];
        return s + ") " + dump_expr(e->a.get()) + ")";
    }
    case ExprKind::Closure: return "(closure " + std::to_string(e->res_idx) + ")";
    case ExprKind::ListComp:
    case ExprKind::SetComp:
    case ExprKind::MapComp: {
        std::string tag = e->kind == ExprKind::ListComp   ? "listcomp"
                          : e->kind == ExprKind::SetComp ? "setcomp"
                                                         : "mapcomp";
        std::string s = "(" + tag + " ";
        if (e->kind == ExprKind::MapComp)
            s += dump_expr(e->pairs[0].first.get()) + ":" + dump_expr(e->pairs[0].second.get());
        else
            s += dump_expr(e->a.get());
        for (auto& cl : e->clauses) {
            s += " for";
            for (auto& t : cl.targets) s += " " + t;
            s += " in " + dump_expr(cl.iter.get());
            for (auto& c : cl.conds) s += " if " + dump_expr(c.get());
        }
        return s + ")";
    }
    case ExprKind::Starred: return "(* " + dump_expr(e->a.get()) + ")";
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
    case StmtKind::AttrAssign:
        return pad + "(setattr " + dump_expr(s->e1.get()) + " ." + s->name + " " +
               dump_expr(s->e3.get()) + ")\n";
    case StmtKind::MultiAssign: {
        std::string r = pad + "(multi= (";
        for (size_t i = 0; i < s->targets.size(); i++)
            r += (i ? " " : "") + dump_expr(s->targets[i].get());
        r += ") (";
        for (size_t i = 0; i < s->values.size(); i++)
            r += (i ? " " : "") + dump_expr(s->values[i].get());
        return r + "))\n";
    }
    case StmtKind::If: {
        std::string r = pad + "(if " + dump_expr(s->e1.get()) + "\n" + block(s->body);
        if (!s->orelse.empty()) r += pad + " else\n" + block(s->orelse);
        return r + pad + ")\n";
    }
    case StmtKind::While:
        return pad + "(while " + dump_expr(s->e1.get()) + "\n" + block(s->body) + pad + ")\n";
    case StmtKind::For: {
        std::string vars = s->params[0];
        for (size_t i = 1; i < s->params.size(); i++) vars += "," + s->params[i];
        return pad + "(for " + vars + " " + dump_expr(s->e1.get()) + "\n" + block(s->body) +
               pad + ")\n";
    }
    case StmtKind::FuncDef: {
        std::string r = pad + "(def " + s->name + " (";
        for (size_t i = 0; i < s->params.size(); i++)
            r += (i ? " " : "") + s->params[i];
        return r + ")\n" + block(s->body) + pad + ")\n";
    }
    case StmtKind::ClassDef: {
        std::string r = pad + "(class " + s->name;
        if (!s->alias.empty()) r += "(" + s->alias + ")";
        return r + "\n" + block(s->body) + pad + ")\n";
    }
    case StmtKind::Return:
        return pad + "(return" + (s->e1 ? " " + dump_expr(s->e1.get()) : "") + ")\n";
    case StmtKind::Break: return pad + "(break)\n";
    case StmtKind::Continue: return pad + "(continue)\n";
    case StmtKind::Pass: return pad + "(pass)\n";
    case StmtKind::Import:
        return pad + "(import " + s->name +
               (s->alias.empty() ? "" : " as " + s->alias) + ")\n";
    case StmtKind::FromImport: {
        std::string r = pad + "(from " + s->name + " import";
        for (auto& p : s->import_names)
            r += " " + p.first + (p.second != p.first ? " as " + p.second : "");
        return r + ")\n";
    }
    case StmtKind::Global: {
        std::string r = pad + "(global";
        for (auto& n : s->params) r += " " + n;
        return r + ")\n";
    }
    case StmtKind::Try: {
        std::string r = pad + "(try\n" + block(s->body);
        for (auto& h : s->handlers) {
            r += pad + " except" + (h.as_name.empty() ? "" : " as " + h.as_name) + "\n";
            for (auto& st : h.body) r += dump_stmt(st.get(), indent + 1);
        }
        if (!s->orelse.empty()) r += pad + " else\n" + block(s->orelse);
        if (!s->final_body.empty()) r += pad + " finally\n" + block(s->final_body);
        return r + pad + ")\n";
    }
    case StmtKind::Del: {
        std::string r = pad + "(del";
        for (auto& t : s->targets) r += " " + dump_expr(t.get());
        return r + ")\n";
    }
    case StmtKind::With: {
        std::string r = pad + "(with " + dump_expr(s->e1.get());
        if (!s->name.empty()) r += " as " + s->name;
        return r + "\n" + block(s->body) + pad + ")\n";
    }
    case StmtKind::Raise:
        if (s->raise_mode == 1) return pad + "(raise)\n";
        if (s->raise_mode == 2)
            return pad + "(raise " + s->name +
                   (s->e1 ? " " + dump_expr(s->e1.get()) : "") + ")\n";
        return pad + "(raise " + dump_expr(s->e1.get()) + ")\n";
    }
    return pad + "?\n";
}

std::string dump_module(const Module& m) {
    std::string r;
    for (auto& s : m.body) r += dump_stmt(s.get());
    return r;
}

} // namespace kami
