// Type inference, unboxing analysis and monomorphization (Part II of the
// performance work).
//
// The pass computes, per function:
//   * a static type for every local slot (join over all assignments),
//   * a static type for every parameter (join over all direct call sites,
//     TY_ANY as soon as the function's name escapes as a value),
//   * the return type (join over all return expressions), and
//   * whether the whole body fits the "native subset" — in which case codegen
//     emits an additional monomorphized specialization
//     `@n_<alias>(i64/double, ...) -> i64/double/void` that keeps every value
//     in CPU registers (no KamiValue boxing, no GC, no runtime calls).
//
// Every expression is stamped with its static type (Expr::sty); codegen uses
// the stamps to emit unboxed LLVM IR for arithmetic, comparisons, truthiness
// tests and range loops even inside functions that are not fully native.
//
// The analysis is a classic optimistic fixed point over a small lattice
// (TY_BOT < INT/FLOAT/BOOL/STR/NONE < TY_ANY): joins only move up, so the
// iteration terminates.
#include "ast.h"

#include "../../runtime/include/kami_builtins.h"
#include "../../runtime/include/kami_runtime.h"

#include <cstring>
#include <functional>
#include <set>
#include <unordered_map>
#include <vector>

namespace kami {
namespace {

uint8_t join(uint8_t a, uint8_t b) {
    if (a == b) return a;
    if (a == TY_BOT) return b;
    if (b == TY_BOT) return a;
    // bool and int share the i64 payload; reading either as i64 is sound and
    // Python's arithmetic already treats True as 1.
    if ((a == TY_INT && b == TY_BOOL) || (a == TY_BOOL && b == TY_INT)) return TY_INT;
    return TY_ANY;
}

bool is_intlike(uint8_t t) { return t == TY_INT || t == TY_BOOL; }
bool is_numeric(uint8_t t) { return is_intlike(t) || t == TY_FLOAT; }
bool is_primitive(uint8_t t) {
    return t == TY_INT || t == TY_FLOAT || t == TY_BOOL || t == TY_NONE;
}

// Return type of a builtin call, when statically known.
uint8_t builtin_ret(int64_t id) {
    switch (id) {
    case KB_LEN: case KB_ORD: case KB_INT: case KB_MATH_FLOOR: case KB_MATH_CEIL:
    case KB_MATH_FACTORIAL: case KB_MATH_GCD: case KB_MATH_ISQRT: case KB_MATH_TRUNC:
    case KB_RANDOM_RANDINT: case KB_RANDOM_RANDRANGE:
        return TY_INT;
    case KB_FLOAT: case KB_MATH_SQRT: case KB_MATH_SIN: case KB_MATH_COS:
    case KB_MATH_TAN: case KB_MATH_EXP: case KB_MATH_LOG: case KB_MATH_POW:
    case KB_MATH_FABS: case KB_MATH_HYPOT: case KB_MATH_LOG2: case KB_MATH_LOG10:
    case KB_MATH_ATAN: case KB_MATH_ASIN: case KB_MATH_ACOS: case KB_MATH_ATAN2:
    case KB_MATH_DEGREES: case KB_MATH_RADIANS:
    case KB_TIME_TIME: case KB_TIME_MONOTONIC: case KB_TIME_PERF_COUNTER:
    case KB_RANDOM_RANDOM: case KB_RANDOM_UNIFORM:
        return TY_FLOAT;
    case KB_BOOL: case KB_ISINSTANCE: case KB_ALL: case KB_ANY:
    case KB_MATH_ISNAN: case KB_MATH_ISINF:
        return TY_BOOL;
    case KB_STR: case KB_CHR: case KB_BIN: case KB_HEX: case KB_OCT:
    case KB_FORMAT: case KB_INPUT:
        return TY_STR;
    case KB_PRINT: case KB_PRINT_EX:
        return TY_NONE;
    default:
        return TY_ANY;
    }
}

} // namespace

// libm mapping for builtin math calls usable inside native bodies:
// (id, C symbol, arity). Only 1-arg double(double) and 2-arg variants.
const char* native_math_symbol(int64_t id, int* arity) {
    *arity = 1;
    switch (id) {
    case KB_MATH_SQRT: return "sqrt";
    case KB_MATH_SIN: return "sin";
    case KB_MATH_COS: return "cos";
    case KB_MATH_TAN: return "tan";
    case KB_MATH_EXP: return "exp";
    case KB_MATH_FABS: return "fabs";
    case KB_MATH_LOG2: return "log2";
    case KB_MATH_LOG10: return "log10";
    case KB_MATH_ATAN: return "atan";
    case KB_MATH_ASIN: return "asin";
    case KB_MATH_ACOS: return "acos";
    case KB_MATH_FLOOR: return nullptr; // returns int in Python: keep boxed
    case KB_MATH_ATAN2: *arity = 2; return "atan2";
    case KB_MATH_POW: *arity = 2; return "pow";
    default: return nullptr;
    }
}

namespace {

struct TypeInf {
    Module& mod;
    std::vector<uint8_t> gtypes; // global slot types (currently always TY_ANY)
    bool changed = false;

    explicit TypeInf(Module& m) : mod(m) {}

    FuncTypeInfo& info(size_t fi) { return mod.ftypes[fi]; }

    // ---- per-function state while walking a body ----
    struct FnCtx {
        Stmt* def = nullptr;   // null = module body
        FuncTypeInfo* fi = nullptr;
    };

    void join_into(uint8_t& slot, uint8_t t) {
        uint8_t j = join(slot, t);
        if (j != slot) {
            slot = j;
            changed = true;
        }
    }

    void join_local(FnCtx& c, int64_t idx, uint8_t t) {
        if (!c.fi) return; // module level: globals are not unboxed
        if (idx >= 0 && (size_t)idx < c.fi->locals.size()) join_into(c.fi->locals[idx], t);
    }

    uint8_t local_type(FnCtx& c, int64_t idx) {
        if (!c.fi) return TY_ANY;
        if (idx < 0 || (size_t)idx >= c.fi->locals.size()) return TY_ANY;
        return c.fi->locals[idx];
    }

    // ---- expression typing (also stamps e->sty) ----
    uint8_t type_expr(FnCtx& c, Expr* e) {
        uint8_t t = type_expr_inner(c, e);
        // Slots hold BOT only before any assignment; treat as ANY for reads
        // (never unbox a maybe-uninitialized value).
        if (t == TY_BOT) t = TY_ANY;
        e->sty = t;
        return t;
    }

    uint8_t type_expr_inner(FnCtx& c, Expr* e) {
        switch (e->kind) {
        case ExprKind::IntLit: return TY_INT;
        case ExprKind::FloatLit: return TY_FLOAT;
        case ExprKind::BoolLit: return TY_BOOL;
        case ExprKind::StrLit: return TY_STR;
        case ExprKind::NoneLit: return TY_NONE;
        case ExprKind::Name:
            if (e->res == Res::Local) return local_type(c, e->res_idx);
            return TY_ANY;
        case ExprKind::Binary: {
            uint8_t a = type_expr(c, e->a.get());
            uint8_t b = type_expr(c, e->b.get());
            switch (e->op) {
            case KOP_EQ: case KOP_NE: case KOP_LT: case KOP_GT: case KOP_LE:
            case KOP_GE: case KOP_IN: case KOP_IS: case KOP_ISNOT:
                return TY_BOOL;
            case KOP_ADD:
                if (a == TY_STR && b == TY_STR) return TY_STR;
                [[fallthrough]];
            case KOP_SUB: case KOP_MUL:
                if (is_intlike(a) && is_intlike(b)) return TY_INT;
                if (is_numeric(a) && is_numeric(b)) return TY_FLOAT;
                if (e->op == KOP_MUL && ((a == TY_STR && is_intlike(b)) ||
                                         (b == TY_STR && is_intlike(a))))
                    return TY_STR;
                return TY_ANY;
            case KOP_DIV:
                if (is_numeric(a) && is_numeric(b)) return TY_FLOAT;
                return TY_ANY;
            case KOP_FLOORDIV: case KOP_MOD:
                if (is_intlike(a) && is_intlike(b)) return TY_INT;
                if (is_numeric(a) && is_numeric(b)) return TY_FLOAT;
                if (e->op == KOP_MOD && a == TY_STR) return TY_STR; // "%d" % x
                return TY_ANY;
            case KOP_BITAND: case KOP_BITOR: case KOP_BITXOR:
            case KOP_SHL: case KOP_SHR:
                if (is_intlike(a) && is_intlike(b)) return TY_INT;
                return TY_ANY;
            case KOP_POW:
                if (is_numeric(a) && is_numeric(b) && (a == TY_FLOAT || b == TY_FLOAT))
                    return TY_FLOAT;
                return TY_ANY; // int**int may go float on negative exponents
            default: return TY_ANY;
            }
        }
        case ExprKind::Unary: {
            uint8_t a = type_expr(c, e->a.get());
            if (e->op == KUOP_NOT) return TY_BOOL;
            if (e->op == KUOP_NEG) return is_intlike(a) ? TY_INT
                                       : a == TY_FLOAT  ? TY_FLOAT
                                                        : TY_ANY;
            if (e->op == KUOP_INV) return is_intlike(a) ? TY_INT : TY_ANY;
            return TY_ANY;
        }
        case ExprKind::BoolOp: {
            uint8_t a = type_expr(c, e->a.get());
            uint8_t b = type_expr(c, e->b.get());
            return join(a, b); // `x and y` yields one of the operands
        }
        case ExprKind::IfExp: {
            uint8_t a = type_expr(c, e->a.get());
            type_expr(c, e->b.get()); // condition
            uint8_t d = type_expr(c, e->c.get());
            return join(a, d);
        }
        case ExprKind::Call: {
            for (auto& a : e->args) type_expr(c, a.get());
            for (auto& kv : e->kwargs) type_expr(c, kv.second.get());
            const Expr* callee = e->a.get();
            if (callee->res == Res::UserFunc) {
                size_t fi = (size_t)callee->res_idx;
                // feed argument types into the callee's parameter joins
                FuncTypeInfo& cal = info(fi);
                const Stmt* def = mod.functions[fi];
                if (!cal.escapes) {
                    for (size_t i = 0; i < e->args.size() && i < cal.params.size(); i++)
                        join_into(cal.params[i], e->args[i]->sty);
                    // omitted trailing arguments: the default expressions run
                    // in the callee prologue; join their (context-free) types.
                    size_t ndef = def->defaults.size(), np = def->params.size();
                    for (size_t i = e->args.size(); i < np; i++) {
                        size_t di = i + ndef;
                        if (di >= np && def->defaults[di - np]) {
                            FnCtx none;
                            join_into(cal.params[i],
                                      type_expr(none, def->defaults[di - np].get()));
                        }
                    }
                }
                return cal.ret == TY_BOT ? TY_ANY : cal.ret;
            }
            if (callee->res == Res::BuiltinFunc) return builtin_ret(callee->res_idx);
            type_expr(c, e->a.get());
            return TY_ANY;
        }
        case ExprKind::CallStar: {
            type_expr(c, e->a.get());
            for (auto& a : e->args) type_expr(c, a.get());
            for (auto& p : e->pairs) {
                if (p.first) type_expr(c, p.first.get());
                type_expr(c, p.second.get());
            }
            return TY_ANY;
        }
        case ExprKind::MethodCall: {
            type_expr(c, e->a.get());
            for (auto& a : e->args) type_expr(c, a.get());
            for (auto& kv : e->kwargs) type_expr(c, kv.second.get());
            return TY_ANY;
        }
        case ExprKind::CCall: {
            for (auto& a : e->args) type_expr(c, a.get());
            if (!e->csig.empty()) {
                char r = e->csig[0];
                if (r == 'd' || r == 'f') return TY_FLOAT;
                if (r == 'i' || r == 'l') return TY_INT;
            }
            return TY_ANY;
        }
        case ExprKind::Index: {
            uint8_t a = type_expr(c, e->a.get());
            type_expr(c, e->b.get());
            return a == TY_STR ? TY_STR : TY_ANY;
        }
        case ExprKind::Slice: {
            uint8_t a = type_expr(c, e->a.get());
            for (auto& p : e->args)
                if (p) type_expr(c, p.get());
            return a == TY_STR ? TY_STR : TY_ANY;
        }
        case ExprKind::Attr:
            if (e->a) type_expr(c, e->a.get());
            return TY_ANY;
        case ExprKind::ListLit:
        case ExprKind::SetLit:
            for (auto& a : e->args) type_expr(c, a.get());
            return TY_ANY;
        case ExprKind::MapLit:
            for (auto& p : e->pairs) {
                type_expr(c, p.first.get());
                type_expr(c, p.second.get());
            }
            return TY_ANY;
        case ExprKind::Starred:
            type_expr(c, e->a.get());
            return TY_ANY;
        case ExprKind::ListComp:
        case ExprKind::SetComp:
        case ExprKind::MapComp: {
            for (auto& cl : e->clauses) {
                type_expr(c, cl.iter.get());
                for (size_t i = 0; i < cl.tkind.size(); i++)
                    if (cl.tkind[i] == 1) join_local(c, cl.tidx[i], TY_ANY);
                for (auto& cond : cl.conds) type_expr(c, cond.get());
            }
            if (e->a) type_expr(c, e->a.get());
            if (!e->pairs.empty()) {
                type_expr(c, e->pairs[0].first.get());
                type_expr(c, e->pairs[0].second.get());
            }
            return TY_ANY;
        }
        default: return TY_ANY;
        }
    }

    // Is `e` a range(...) call whose arguments are all statically int?
    bool int_range(Expr* e) {
        if (e->kind != ExprKind::Call || e->a->res != Res::BuiltinFunc ||
            e->a->res_idx != KB_RANGE)
            return false;
        for (auto& a : e->args)
            if (!is_intlike(a->sty)) return false;
        return true;
    }

    // ---- statement walking ----
    void walk_stmts(FnCtx& c, std::vector<StmtPtr>& body) {
        for (auto& sp : body) walk_stmt(c, sp.get());
    }

    void walk_stmt(FnCtx& c, Stmt* s) {
        switch (s->kind) {
        case StmtKind::ExprStmt:
            type_expr(c, s->e1.get());
            return;
        case StmtKind::Assign: {
            uint8_t t = type_expr(c, s->e1.get());
            if (s->target_res == Res::Local) join_local(c, s->target_idx, t);
            return;
        }
        case StmtKind::IndexAssign:
            type_expr(c, s->e1.get());
            type_expr(c, s->e2.get());
            type_expr(c, s->e3.get());
            return;
        case StmtKind::AttrAssign:
            type_expr(c, s->e1.get());
            type_expr(c, s->e3.get());
            return;
        case StmtKind::MultiAssign:
            for (auto& v : s->values) type_expr(c, v.get());
            for (auto& t : s->targets) {
                type_expr(c, t.get());
                if (t->kind == ExprKind::Name && t->res == Res::Local)
                    join_local(c, t->res_idx, TY_ANY);
            }
            return;
        case StmtKind::If:
        case StmtKind::While:
            type_expr(c, s->e1.get());
            walk_stmts(c, s->body);
            walk_stmts(c, s->orelse);
            return;
        case StmtKind::For: {
            type_expr(c, s->e1.get());
            uint8_t elem = int_range(s->e1.get()) ? TY_INT : TY_ANY;
            if (s->params.size() == 1) {
                if (s->target_res == Res::Local) join_local(c, s->target_idx, elem);
            } else {
                for (size_t i = 0; i < s->multi_tkind.size(); i++)
                    if (s->multi_tkind[i] == 1) join_local(c, s->multi_tidx[i], TY_ANY);
            }
            walk_stmts(c, s->body);
            walk_stmts(c, s->orelse);
            return;
        }
        case StmtKind::FuncDef: // nested def: local receives a closure value
            if (s->target_res == Res::Local) join_local(c, s->target_idx, TY_ANY);
            return;                       // its body is walked as its own function
        case StmtKind::Return:
            if (c.fi) {
                uint8_t t = s->e1 ? type_expr(c, s->e1.get()) : TY_NONE;
                join_into(c.fi->ret, t);
            } else if (s->e1) {
                type_expr(c, s->e1.get());
            }
            return;
        case StmtKind::Raise:
            if (s->e1) type_expr(c, s->e1.get());
            return;
        case StmtKind::Try:
            walk_stmts(c, s->body);
            for (auto& h : s->handlers) {
                if (h.as_kind == 1) join_local(c, h.as_idx, TY_ANY);
                walk_stmts(c, h.body);
            }
            walk_stmts(c, s->orelse);
            walk_stmts(c, s->final_body);
            return;
        case StmtKind::With:
            if (s->e1) type_expr(c, s->e1.get());
            if (s->target_res == Res::Local) join_local(c, s->target_idx, TY_ANY);
            walk_stmts(c, s->body);
            return;
        case StmtKind::Del:
            for (auto& t : s->targets) {
                type_expr(c, t.get());
                if (t->kind == ExprKind::Name && t->res == Res::Local)
                    join_local(c, t->res_idx, TY_ANY);
            }
            return;
        default: return;
        }
    }

    // ---- escape detection: a function name used as a value anywhere ----
    void find_escapes() {
        std::unordered_map<int64_t, size_t> gidx_to_fn;
        for (size_t i = 0; i < mod.functions.size(); i++)
            if (mod.functions[i]->global_idx >= 0)
                gidx_to_fn[mod.functions[i]->global_idx] = i;
        // optimistic: module-level functions don't escape...
        for (size_t i = 0; i < mod.functions.size(); i++) {
            Stmt* f = mod.functions[i];
            info(i).escapes = f->global_idx < 0 || !f->decorators.empty() ||
                              !f->vararg.empty() || !f->kwarg.empty();
        }
        // ...until their global is read as a plain value (callee position of a
        // direct call resolves to Res::UserFunc, never Res::Global).
        std::function<void(Expr*)> ex = [&](Expr* e) {
            if (!e) return;
            if (e->kind == ExprKind::Name && e->res == Res::Global) {
                auto it = gidx_to_fn.find(e->res_idx);
                if (it != gidx_to_fn.end()) info(it->second).escapes = true;
            }
            ex(e->a.get());
            ex(e->b.get());
            ex(e->c.get());
            for (auto& a : e->args) ex(a.get());
            for (auto& kv : e->kwargs) ex(kv.second.get());
            for (auto& p : e->pairs) {
                ex(p.first.get());
                ex(p.second.get());
            }
            for (auto& cl : e->clauses) {
                ex(cl.iter.get());
                for (auto& cond : cl.conds) ex(cond.get());
            }
        };
        std::function<void(std::vector<StmtPtr>&)> sts = [&](std::vector<StmtPtr>& body) {
            for (auto& sp : body) {
                Stmt* s = sp.get();
                ex(s->e1.get());
                ex(s->e2.get());
                ex(s->e3.get());
                for (auto& t : s->targets) ex(t.get());
                for (auto& v : s->values) ex(v.get());
                for (auto& d : s->decorators) ex(d.get());
                for (auto& d : s->defaults) ex(d.get());
                sts(s->body);
                sts(s->orelse);
                sts(s->final_body);
                for (auto& h : s->handlers) sts(h.body);
            }
        };
        sts(mod.body);
        for (Stmt* f : mod.functions) sts(f->body);
        // params of escaping functions are unknown
        for (size_t i = 0; i < mod.functions.size(); i++)
            if (info(i).escapes)
                for (auto& p : info(i).params) p = TY_ANY;
    }

    // ---- native-subset check (monomorphization eligibility) ----
    bool native_expr(const Expr* e) {
        if (!is_primitive(e->sty)) return false;
        switch (e->kind) {
        case ExprKind::IntLit:
        case ExprKind::FloatLit:
        case ExprKind::BoolLit:
            return true;
        case ExprKind::Name:
            return e->res == Res::Local && is_primitive(e->sty) && e->sty != TY_NONE;
        case ExprKind::Binary:
            switch (e->op) {
            case KOP_ADD: case KOP_SUB: case KOP_MUL: case KOP_DIV:
            case KOP_FLOORDIV: case KOP_MOD:
            case KOP_EQ: case KOP_NE: case KOP_LT: case KOP_GT:
            case KOP_LE: case KOP_GE:
            case KOP_BITAND: case KOP_BITOR: case KOP_BITXOR:
                return is_numeric(e->a->sty) && is_numeric(e->b->sty) &&
                       native_expr(e->a.get()) && native_expr(e->b.get());
            default: return false;
            }
        case ExprKind::Unary:
            return (e->op == KUOP_NEG || e->op == KUOP_NOT) && native_expr(e->a.get());
        case ExprKind::BoolOp:
        case ExprKind::IfExp:
            return native_expr(e->a.get()) && native_expr(e->b.get()) &&
                   (!e->c || native_expr(e->c.get()));
        case ExprKind::Call: {
            const Expr* callee = e->a.get();
            if (callee->res == Res::UserFunc) {
                size_t fi = (size_t)callee->res_idx;
                if (!mod.ftypes[fi].native_ok) return false;
                const Stmt* def = mod.functions[fi];
                if (e->args.size() != def->params.size()) return false; // defaults
                for (auto& a : e->args)
                    if (!native_expr(a.get())) return false;
                return true;
            }
            if (callee->res == Res::BuiltinFunc) {
                int arity = 0;
                if (native_math_symbol(callee->res_idx, &arity) &&
                    (int)e->args.size() == arity) {
                    for (auto& a : e->args)
                        if (!native_expr(a.get()) || !is_numeric(a->sty)) return false;
                    return true;
                }
                if (callee->res_idx == KB_ABS && e->args.size() == 1)
                    return native_expr(e->args[0].get()) && is_numeric(e->args[0]->sty);
                return false;
            }
            return false;
        }
        case ExprKind::CCall: {
            // double(double...) C calls (e.g. `from _math import sqrt`)
            if (e->csig.empty() || e->csig[0] != 'd') return false;
            for (size_t i = 1; i < e->csig.size(); i++)
                if (e->csig[i] != 'd') return false;
            for (auto& a : e->args)
                if (!native_expr(a.get()) || !is_numeric(a->sty)) return false;
            return true;
        }
        default: return false;
        }
    }

    bool native_stmts(const std::vector<StmtPtr>& body) {
        for (auto& sp : body)
            if (!native_stmt(sp.get())) return false;
        return true;
    }

    bool native_stmt(const Stmt* s) {
        switch (s->kind) {
        case StmtKind::Pass:
        case StmtKind::Break:
        case StmtKind::Continue:
            return true;
        case StmtKind::ExprStmt:
            return native_expr(s->e1.get());
        case StmtKind::Assign:
            return s->target_res == Res::Local && native_expr(s->e1.get());
        case StmtKind::Return:
            return !s->e1 || native_expr(s->e1.get());
        case StmtKind::If:
        case StmtKind::While:
            return native_expr(s->e1.get()) && native_stmts(s->body) &&
                   native_stmts(s->orelse);
        case StmtKind::For: {
            if (s->params.size() != 1 || s->target_res != Res::Local) return false;
            Expr* it = s->e1.get();
            if (it->kind != ExprKind::Call || it->a->res != Res::BuiltinFunc ||
                it->a->res_idx != KB_RANGE)
                return false;
            for (auto& a : it->args)
                if (!native_expr(a.get()) || !is_intlike(a->sty)) return false;
            return native_stmts(s->body) && s->orelse.empty();
        }
        default: return false;
        }
    }

    // Does every control path through `body` end in return/raise?
    static bool always_returns(const std::vector<StmtPtr>& body) {
        if (body.empty()) return false;
        const Stmt* last = body.back().get();
        switch (last->kind) {
        case StmtKind::Return:
        case StmtKind::Raise:
            return true;
        case StmtKind::If:
            return always_returns(last->body) && always_returns(last->orelse);
        case StmtKind::While: // `while True:` with no break would qualify, but
        case StmtKind::For:   // proving it is not worth the complexity
        default:
            return false;
        }
    }

    void compute_native() {
        // optimistic init: every non-escaping module-level function with
        // primitive params/locals/ret is a candidate...
        for (size_t i = 0; i < mod.functions.size(); i++) {
            Stmt* f = mod.functions[i];
            FuncTypeInfo& fi = info(i);
            bool ok = !fi.escapes && f->global_idx >= 0 && f->defaults.empty() &&
                      f->vararg.empty() && f->kwarg.empty() && f->decorators.empty();
            if (ok)
                for (uint8_t p : fi.params)
                    if (!is_primitive(p) || p == TY_NONE) ok = false;
            if (ok)
                for (uint8_t l : fi.locals)
                    if (!(is_primitive(l) || l == TY_BOT)) ok = false;
            if (ok && !(is_primitive(fi.ret) || fi.ret == TY_BOT)) ok = false;
            fi.native_ok = ok;
        }
        // ...then drop everything whose body leaves the native subset, until
        // stable (a dropped callee can disqualify its callers).
        bool again = true;
        while (again) {
            again = false;
            for (size_t i = 0; i < mod.functions.size(); i++) {
                if (!info(i).native_ok) continue;
                if (!native_stmts(mod.functions[i]->body)) {
                    info(i).native_ok = false;
                    again = true;
                }
            }
        }
    }

    void run() {
        mod.ftypes.assign(mod.functions.size(), FuncTypeInfo{});
        for (size_t i = 0; i < mod.functions.size(); i++) {
            Stmt* f = mod.functions[i];
            size_t nlocals = (size_t)f->nlocals;
            mod.ftypes[i].locals.assign(nlocals, TY_BOT);
            mod.ftypes[i].params.assign(f->params.size(), TY_BOT);
        }
        find_escapes();
        // fixed point: parameter types feed locals feed returns feed call sites
        for (int round = 0; round < 20; round++) {
            changed = false;
            FnCtx modctx; // module body: no local table
            walk_stmts(modctx, mod.body);
            for (size_t i = 0; i < mod.functions.size(); i++) {
                Stmt* f = mod.functions[i];
                FnCtx c{f, &mod.ftypes[i]};
                // parameters seed the local slots
                size_t np = f->params.size();
                for (size_t p = 0; p < np && p < c.fi->locals.size(); p++)
                    join_into(c.fi->locals[p],
                              c.fi->escapes ? (uint8_t)TY_ANY : c.fi->params[p]);
                // *args/**kwargs slots are tuple/dict
                size_t extra = np;
                if (!f->vararg.empty()) join_local(c, (int64_t)extra++, TY_ANY);
                if (!f->kwarg.empty()) join_local(c, (int64_t)extra++, TY_ANY);
                walk_stmts(c, f->body);
                if (!always_returns(f->body)) join_into(c.fi->ret, TY_NONE);
            }
            if (!changed) break;
        }
        // functions whose body always falls through also return None
        for (size_t i = 0; i < mod.functions.size(); i++) {
            // (already handled above; keep ret sane)
            if (mod.ftypes[i].ret == TY_BOT) mod.ftypes[i].ret = TY_NONE;
        }
        compute_native();
        // Re-stamp every expression once more with the final tables so codegen
        // sees stable types (earlier rounds may have stamped BOT-era values).
        changed = false;
        FnCtx modctx;
        walk_stmts(modctx, mod.body);
        for (size_t i = 0; i < mod.functions.size(); i++) {
            FnCtx c{mod.functions[i], &mod.ftypes[i]};
            walk_stmts(c, mod.functions[i]->body);
        }
        // For-range loops: stamp the loop-variable type for codegen
        stamp_for_types();
        // an unboxed-numeric `s = s + x` never needs the string fast path
        clear_numeric_iadds(mod.body);
        for (Stmt* f : mod.functions) clear_numeric_iadds(f->body);
    }

    void stamp_for_types() {
        std::function<void(std::vector<StmtPtr>&, FuncTypeInfo*)> walk =
            [&](std::vector<StmtPtr>& body, FuncTypeInfo* fi) {
                for (auto& sp : body) {
                    Stmt* s = sp.get();
                    if (s->kind == StmtKind::For && s->params.size() == 1 &&
                        s->target_res == Res::Local && fi &&
                        (size_t)s->target_idx < fi->locals.size())
                        s->sty = fi->locals[s->target_idx];
                    walk(s->body, fi);
                    walk(s->orelse, fi);
                    walk(s->final_body, fi);
                    for (auto& h : s->handlers) walk(h.body, fi);
                }
            };
        walk(mod.body, nullptr);
        for (size_t i = 0; i < mod.functions.size(); i++)
            walk(mod.functions[i]->body, &mod.ftypes[i]);
    }

    void clear_numeric_iadds(std::vector<StmtPtr>& body) {
        for (auto& sp : body) {
            Stmt* s = sp.get();
            if (s->str_iadd && s->e1 && is_primitive(s->e1->sty)) s->str_iadd = false;
            clear_numeric_iadds(s->body);
            clear_numeric_iadds(s->orelse);
            clear_numeric_iadds(s->final_body);
            for (auto& h : s->handlers) clear_numeric_iadds(h.body);
        }
    }
};

} // namespace

void infer_types(Module& m) {
    TypeInf ti(m);
    ti.run();
}

} // namespace kami
