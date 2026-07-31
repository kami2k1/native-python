#include "sema.h"

#include "../../runtime/include/kami_builtins.h"
#include "../../runtime/include/kami_runtime.h"

#include <cmath>
#include <set>
#include <unordered_map>

namespace kami {

namespace {

struct BuiltinSig {
    int64_t id;
    int min_args;
    int max_args;
};

// Unqualified builtins available without import.
static const std::unordered_map<std::string, BuiltinSig>& builtins() {
    static const std::unordered_map<std::string, BuiltinSig> b = {
        {"print", {KB_PRINT, 0, 16}}, {"len", {KB_LEN, 1, 1}},
        {"str", {KB_STR, 1, 1}},      {"int", {KB_INT, 1, 1}},
        {"float", {KB_FLOAT, 1, 1}},  {"abs", {KB_ABS, 1, 1}},
        {"min", {KB_MIN, 2, 16}},     {"max", {KB_MAX, 2, 16}},
        {"ord", {KB_ORD, 1, 1}},      {"chr", {KB_CHR, 1, 1}},
        {"type", {KB_TYPE, 1, 1}},    {"range", {KB_RANGE, 1, 3}},
    };
    return b;
}

// module name -> attr name -> builtin
static const std::unordered_map<std::string, std::unordered_map<std::string, BuiltinSig>>&
modules() {
    static const std::unordered_map<std::string, std::unordered_map<std::string, BuiltinSig>>
        m = {
            {"math",
             {{"sqrt", {KB_MATH_SQRT, 1, 1}}, {"sin", {KB_MATH_SIN, 1, 1}},
              {"cos", {KB_MATH_COS, 1, 1}},   {"tan", {KB_MATH_TAN, 1, 1}},
              {"exp", {KB_MATH_EXP, 1, 1}},   {"log", {KB_MATH_LOG, 1, 1}},
              {"pow", {KB_MATH_POW, 2, 2}},   {"floor", {KB_MATH_FLOOR, 1, 1}},
              {"ceil", {KB_MATH_CEIL, 1, 1}}, {"fabs", {KB_MATH_FABS, 1, 1}}}},
            {"time", {{"time", {KB_TIME_TIME, 0, 0}}, {"sleep", {KB_TIME_SLEEP, 1, 1}}}},
            {"random",
             {{"random", {KB_RANDOM_RANDOM, 0, 0}},
              {"randint", {KB_RANDOM_RANDINT, 2, 2}},
              {"seed", {KB_RANDOM_SEED, 1, 1}}}},
            {"threading",
             {{"spawn", {KB_THREAD_SPAWN, 1, 9}}, {"join", {KB_THREAD_JOIN, 1, 1}}}},
        };
    return m;
}

struct FuncEntry {
    Stmt* def;
    int index;       // in Module::functions
    int64_t global;  // global slot holding the function value
};

struct Sema {
    Module& mod;
    std::unordered_map<std::string, FuncEntry> funcs;
    std::unordered_map<std::string, int64_t> globals;
    std::set<std::string> imports;

    // current function scope (nullptr at module level)
    Stmt* cur_func = nullptr;
    std::unordered_map<std::string, int64_t> locals;

    explicit Sema(Module& m) : mod(m) {}

    [[noreturn]] static void err(int line, const std::string& m) { throw CompileError(line, m); }

    int64_t global_slot(const std::string& name) {
        auto it = globals.find(name);
        if (it != globals.end()) return it->second;
        int64_t idx = (int64_t)globals.size();
        globals.emplace(name, idx);
        return idx;
    }

    // ---- pass A: collect module-level names ----
    void collect_module() {
        for (auto& sp : mod.body) {
            Stmt* s = sp.get();
            switch (s->kind) {
            case StmtKind::FuncDef: {
                if (funcs.count(s->name)) err(s->line, "function '" + s->name + "' redefined");
                if (builtins().count(s->name))
                    err(s->line, "cannot redefine builtin '" + s->name + "'");
                s->func_index = (int)mod.functions.size();
                s->global_idx = global_slot(s->name);
                mod.functions.push_back(s);
                funcs[s->name] = {s, s->func_index, s->global_idx};
                break;
            }
            case StmtKind::Assign: global_slot(s->name); break;
            case StmtKind::For: global_slot(s->name); collect_nested_assigns(s->body, true); break;
            case StmtKind::If:
                collect_nested_assigns(s->body, true);
                collect_nested_assigns(s->orelse, true);
                break;
            case StmtKind::While: collect_nested_assigns(s->body, true); break;
            case StmtKind::Import: {
                if (!modules().count(s->name))
                    err(s->line, "unknown module '" + s->name +
                                     "' (available: math, time, random, threading)");
                imports.insert(s->name);
                break;
            }
            default: break;
            }
        }
    }

    void collect_nested_assigns(std::vector<StmtPtr>& body, bool as_globals) {
        for (auto& sp : body) {
            Stmt* s = sp.get();
            switch (s->kind) {
            case StmtKind::Assign:
                if (as_globals) global_slot(s->name);
                else local_slot(s->name);
                break;
            case StmtKind::For:
                if (as_globals) global_slot(s->name);
                else local_slot(s->name);
                collect_nested_assigns(s->body, as_globals);
                break;
            case StmtKind::If:
                collect_nested_assigns(s->body, as_globals);
                collect_nested_assigns(s->orelse, as_globals);
                break;
            case StmtKind::While: collect_nested_assigns(s->body, as_globals); break;
            case StmtKind::FuncDef:
                err(s->line, "nested functions are not supported");
            default: break;
            }
        }
    }

    int64_t local_slot(const std::string& name) {
        auto it = locals.find(name);
        if (it != locals.end()) return it->second;
        int64_t idx = (int64_t)locals.size();
        locals.emplace(name, idx);
        return idx;
    }

    // ---- resolution ----
    void resolve_target(Stmt* s, const std::string& name) {
        if (cur_func) {
            s->target_res = Res::Local;
            s->target_idx = local_slot(name);
        } else {
            s->target_res = Res::Global;
            s->target_idx = global_slot(name);
        }
    }

    void resolve_name(Expr* e) {
        const std::string& n = e->sval;
        if (cur_func) {
            auto it = locals.find(n);
            if (it != locals.end()) {
                e->res = Res::Local;
                e->res_idx = it->second;
                return;
            }
        }
        auto g = globals.find(n);
        if (g != globals.end()) {
            e->res = Res::Global;
            e->res_idx = g->second;
            return;
        }
        if (imports.count(n))
            err(e->line, "module '" + n + "' can only be used as '" + n + ".<name>'");
        if (builtins().count(n))
            err(e->line, "builtin '" + n + "' can only be called, not used as a value");
        err(e->line, "undefined variable '" + n + "'");
    }

    void resolve_expr(Expr* e) {
        switch (e->kind) {
        case ExprKind::IntLit:
        case ExprKind::FloatLit:
        case ExprKind::StrLit:
        case ExprKind::BoolLit:
        case ExprKind::NoneLit: return;
        case ExprKind::Name: resolve_name(e); return;
        case ExprKind::Binary:
        case ExprKind::BoolOp:
            resolve_expr(e->a.get());
            resolve_expr(e->b.get());
            fold(e);
            return;
        case ExprKind::Unary:
            resolve_expr(e->a.get());
            fold(e);
            return;
        case ExprKind::Index:
            resolve_expr(e->a.get());
            resolve_expr(e->b.get());
            return;
        case ExprKind::Attr: {
            // Only module attribute constants reach here (module.attr not called).
            if (e->a->kind == ExprKind::Name && imports.count(e->a->sval)) {
                const std::string& mn = e->a->sval;
                if (mn == "math" && e->sval == "pi") {
                    e->kind = ExprKind::FloatLit;
                    e->fval = 3.14159265358979323846;
                    e->a.reset();
                    return;
                }
                if (mn == "math" && e->sval == "e") {
                    e->kind = ExprKind::FloatLit;
                    e->fval = 2.71828182845904523536;
                    e->a.reset();
                    return;
                }
                err(e->line, "module '" + mn + "' has no constant '" + e->sval + "'");
            }
            err(e->line, "attribute access is only supported on modules and method calls");
        }
        case ExprKind::Call: {
            for (auto& a : e->args) resolve_expr(a.get());
            Expr* callee = e->a.get();
            if (callee->kind == ExprKind::Name) {
                const std::string& n = callee->sval;
                // local variable holding a function?
                if (cur_func && locals.count(n)) {
                    resolve_expr(callee);
                    return; // dynamic call through value
                }
                auto f = funcs.find(n);
                if (f != funcs.end()) {
                    if ((int)e->args.size() != (int)f->second.def->params.size())
                        err(e->line, n + "() takes " +
                                         std::to_string(f->second.def->params.size()) +
                                         " argument(s) but " + std::to_string(e->args.size()) +
                                         " were given");
                    callee->res = Res::UserFunc;
                    callee->res_idx = f->second.index;
                    return;
                }
                auto b = builtins().find(n);
                if (b != builtins().end() && !globals.count(n)) {
                    if ((int)e->args.size() < b->second.min_args ||
                        (int)e->args.size() > b->second.max_args)
                        err(e->line, n + "() got " + std::to_string(e->args.size()) +
                                         " argument(s)");
                    callee->res = Res::BuiltinFunc;
                    callee->res_idx = b->second.id;
                    return;
                }
            }
            resolve_expr(callee); // dynamic call through a value
            return;
        }
        case ExprKind::MethodCall: {
            for (auto& a : e->args) resolve_expr(a.get());
            // module function call?  math.sqrt(x)
            if (e->a->kind == ExprKind::Name && imports.count(e->a->sval) &&
                !(cur_func && locals.count(e->a->sval)) && !globals.count(e->a->sval)) {
                const std::string& mn = e->a->sval;
                auto& mm = modules().at(mn);
                auto it = mm.find(e->sval);
                if (it == mm.end())
                    err(e->line, "module '" + mn + "' has no function '" + e->sval + "'");
                if ((int)e->args.size() < it->second.min_args ||
                    (int)e->args.size() > it->second.max_args)
                    err(e->line, mn + "." + e->sval + "() got " +
                                     std::to_string(e->args.size()) + " argument(s)");
                // rewrite into a builtin call
                e->kind = ExprKind::Call;
                auto callee = std::make_unique<Expr>();
                callee->kind = ExprKind::Name;
                callee->line = e->line;
                callee->sval = mn + "." + e->sval;
                callee->res = Res::BuiltinFunc;
                callee->res_idx = it->second.id;
                e->a = std::move(callee);
                return;
            }
            resolve_expr(e->a.get());
            return;
        }
        case ExprKind::ListLit:
            for (auto& a : e->args) resolve_expr(a.get());
            return;
        case ExprKind::MapLit:
            for (auto& p : e->pairs) {
                resolve_expr(p.first.get());
                resolve_expr(p.second.get());
            }
            return;
        }
    }

    // ---- constant folding (int/float/bool arithmetic, string concat) ----
    static bool is_const(const Expr* e) {
        return e->kind == ExprKind::IntLit || e->kind == ExprKind::FloatLit ||
               e->kind == ExprKind::BoolLit || e->kind == ExprKind::StrLit;
    }
    static bool numeric(const Expr* e) {
        return e->kind == ExprKind::IntLit || e->kind == ExprKind::FloatLit ||
               e->kind == ExprKind::BoolLit;
    }
    static double numval(const Expr* e) {
        return e->kind == ExprKind::FloatLit ? e->fval : (double)e->ival;
    }

    void fold(Expr* e) {
        if (e->kind == ExprKind::Unary && e->op == KUOP_NEG && numeric(e->a.get())) {
            Expr* a = e->a.get();
            if (a->kind == ExprKind::FloatLit) {
                e->kind = ExprKind::FloatLit;
                e->fval = -a->fval;
            } else {
                e->kind = ExprKind::IntLit;
                e->ival = -a->ival;
            }
            e->a.reset();
            return;
        }
        if (e->kind != ExprKind::Binary) return;
        Expr *a = e->a.get(), *b = e->b.get();
        if (!a || !b || !is_const(a) || !is_const(b)) return;
        // string concat
        if (e->op == KOP_ADD && a->kind == ExprKind::StrLit && b->kind == ExprKind::StrLit) {
            e->kind = ExprKind::StrLit;
            e->sval = a->sval + b->sval;
            e->a.reset();
            e->b.reset();
            return;
        }
        if (!numeric(a) || !numeric(b)) return;
        bool both_int = a->kind != ExprKind::FloatLit && b->kind != ExprKind::FloatLit;
        auto set_int = [&](int64_t v) {
            e->kind = ExprKind::IntLit;
            e->ival = v;
            e->a.reset();
            e->b.reset();
        };
        auto set_float = [&](double v) {
            e->kind = ExprKind::FloatLit;
            e->fval = v;
            e->a.reset();
            e->b.reset();
        };
        auto set_bool = [&](bool v) {
            e->kind = ExprKind::BoolLit;
            e->ival = v;
            e->a.reset();
            e->b.reset();
        };
        int64_t ia = a->ival, ib = b->ival;
        double fa = numval(a), fb = numval(b);
        switch (e->op) {
        case KOP_ADD: both_int ? set_int(ia + ib) : set_float(fa + fb); return;
        case KOP_SUB: both_int ? set_int(ia - ib) : set_float(fa - fb); return;
        case KOP_MUL: both_int ? set_int(ia * ib) : set_float(fa * fb); return;
        case KOP_DIV:
            if (fb == 0.0) return; // let runtime raise
            set_float(fa / fb);
            return;
        case KOP_FLOORDIV:
            if (both_int) {
                if (ib == 0) return;
                int64_t q = ia / ib;
                if ((ia % ib != 0) && ((ia < 0) != (ib < 0))) q--;
                set_int(q);
            } else {
                if (fb == 0.0) return;
                set_float(std::floor(fa / fb));
            }
            return;
        case KOP_MOD:
            if (both_int) {
                if (ib == 0) return;
                int64_t r = ia % ib;
                if (r != 0 && ((r < 0) != (ib < 0))) r += ib;
                set_int(r);
            }
            return;
        case KOP_EQ: set_bool(fa == fb); return;
        case KOP_NE: set_bool(fa != fb); return;
        case KOP_LT: set_bool(fa < fb); return;
        case KOP_GT: set_bool(fa > fb); return;
        case KOP_LE: set_bool(fa <= fb); return;
        case KOP_GE: set_bool(fa >= fb); return;
        default: return;
        }
    }

    // ---- statements ----
    void resolve_stmts(std::vector<StmtPtr>& body) {
        for (auto& sp : body) resolve_stmt(sp.get());
    }

    void resolve_stmt(Stmt* s) {
        switch (s->kind) {
        case StmtKind::ExprStmt: resolve_expr(s->e1.get()); return;
        case StmtKind::Assign:
            resolve_expr(s->e1.get());
            resolve_target(s, s->name);
            return;
        case StmtKind::IndexAssign:
            resolve_expr(s->e1.get());
            resolve_expr(s->e2.get());
            resolve_expr(s->e3.get());
            return;
        case StmtKind::If:
            resolve_expr(s->e1.get());
            resolve_stmts(s->body);
            resolve_stmts(s->orelse);
            return;
        case StmtKind::While:
            resolve_expr(s->e1.get());
            resolve_stmts(s->body);
            return;
        case StmtKind::For:
            resolve_expr(s->e1.get());
            resolve_target(s, s->name);
            resolve_stmts(s->body);
            return;
        case StmtKind::FuncDef: {
            if (cur_func) err(s->line, "nested functions are not supported");
            cur_func = s;
            locals.clear();
            for (auto& p : s->params) {
                if (locals.count(p)) err(s->line, "duplicate parameter '" + p + "'");
                local_slot(p);
            }
            collect_nested_assigns(s->body, false);
            resolve_stmts(s->body);
            s->nlocals = (int)locals.size();
            cur_func = nullptr;
            locals.clear();
            return;
        }
        case StmtKind::Return:
            if (s->e1) resolve_expr(s->e1.get());
            return;
        case StmtKind::Break:
        case StmtKind::Continue:
        case StmtKind::Pass:
        case StmtKind::Import: return;
        }
    }
};

} // namespace

void analyze(Module& m) {
    Sema s(m);
    s.collect_module();
    s.resolve_stmts(m.body);
    m.nglobals = (int64_t)s.globals.size();
}

} // namespace kami
