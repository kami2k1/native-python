#include "codegen.h"

#include "../../runtime/include/kami_builtins.h"
#include "../../runtime/include/kami_runtime.h"

#include <cinttypes>
#include <cstdio>
#include <cstring>
#include <functional>

namespace kami {

namespace {

std::string escape_ir_string(const std::string& s) {
    static const char* hex = "0123456789ABCDEF";
    std::string out;
    for (unsigned char c : s) {
        if (c >= 0x20 && c < 0x7f && c != '"' && c != '\\') {
            out += (char)c;
        } else {
            out += '\\';
            out += hex[c >> 4];
            out += hex[c & 15];
        }
    }
    out += "\\00";
    return out;
}

// Largest number of positional arguments a function accepts. A `*args`
// catch-all swallows everything up to the runtime's argument-buffer limit.
size_t max_arity(const Stmt* f) {
    return f->vararg ? 16 : f->params.size();
}

std::string fmt_double(double d) {
    uint64_t bits;
    memcpy(&bits, &d, 8);
    char buf[32];
    snprintf(buf, sizeof buf, "0x%016" PRIX64, bits);
    return buf;
}

struct StrTable {
    std::vector<std::string> strs;
    int intern(const std::string& s) {
        for (size_t i = 0; i < strs.size(); i++)
            if (strs[i] == s) return (int)i;
        strs.push_back(s);
        return (int)strs.size() - 1;
    }
    std::string ref(int i) const { return "@.s" + std::to_string(i); }
};

static bool stmts_have_try(const std::vector<StmtPtr>& body);
static bool stmt_has_try(const Stmt* s) {
    if (s->kind == StmtKind::Try) return true;
    if (stmts_have_try(s->body) || stmts_have_try(s->orelse) ||
        stmts_have_try(s->final_body))
        return true;
    for (auto& h : s->handlers)
        if (stmts_have_try(h.body)) return true;
    return false;
}
static bool stmts_have_try(const std::vector<StmtPtr>& body) {
    for (auto& s : body)
        if (stmt_has_try(s.get())) return true;
    return false;
}

static int g_outline_counter = 0;

struct FnGen {
    const Module& mod;
    StrTable& strtab;

    // shared frame/slot state
    int nlocals = 0;
    int temps_base = 0; // nlocals (+1 when a return-spill slot is reserved)
    int spill_slot = -1;
    int temp_top = 0;
    int max_temps = 0;
    int max_call_args = 1;
    bool is_module = false;

    // per-emission-context state (outlined try bodies get their own context)
    struct Ctx {
        std::string fname; // empty = main body
        std::string body;
        int reg = 0;
        int label = 0;
        bool terminated = false;
        bool outlined = false;
        std::vector<std::pair<std::string, std::string>> loops; // (continue, break)
    };
    std::vector<Ctx> ctxs;
    std::string extra_fns; // outlined function definitions

    FnGen(const Module& m, StrTable& st) : mod(m), strtab(st) { ctxs.emplace_back(); }

    Ctx& C() { return ctxs.back(); }
    std::string r() { return "%r" + std::to_string(C().reg++); }
    std::string newlabel() {
        return (C().outlined ? "T" : "L") + std::to_string(C().label++);
    }
    void emit(const std::string& line) { C().body += "  " + line + "\n"; }
    void start_block(const std::string& l) {
        if (!C().terminated) emit("br label %" + l);
        C().body += l + ":\n";
        C().terminated = false;
    }

    int alloc_temp() {
        int t = temps_base + temp_top;
        temp_top++;
        if (temp_top > max_temps) max_temps = temp_top;
        return t;
    }

    // Reserve a frame slot that survives temp_top resets (used by `with` to keep
    // the context manager alive across the outlined body).
    //
    // These slots live above the temp window, but `max_temps` is only final once
    // the whole function has been generated — so the index cannot be computed
    // here. Emitting a placeholder that finish() patches keeps persistent slots
    // and temporaries from ever aliasing. (They did before, which is why
    // `with` inside a loop used to lose its context manager and never run
    // __exit__.)
    static const int PERSISTENT_BASE = 1 << 20;
    int persistent_top = 0;
    int alloc_slot_persistent() { return PERSISTENT_BASE + persistent_top++; }

    std::string slot_index(int slot) const {
        if (slot >= PERSISTENT_BASE)
            return "@@PSLOT" + std::to_string(slot - PERSISTENT_BASE) + "@@";
        return std::to_string(slot);
    }

    std::string slot_ptr(int slot) {
        std::string p = r();
        emit(p + " = getelementptr inbounds %kv, ptr %frame, i64 " + slot_index(slot));
        return p;
    }

    void fill_argbuf(const std::vector<int>& slots) {
        if ((int)slots.size() > max_call_args) max_call_args = (int)slots.size();
        for (size_t k = 0; k < slots.size(); k++) {
            std::string sp = slot_ptr(slots[k]);
            std::string bp = r();
            emit(bp + " = getelementptr inbounds ptr, ptr %argbuf, i64 " + std::to_string(k));
            emit("store ptr " + sp + ", ptr " + bp + ", align 8");
        }
    }

    std::string truthy(int slot) {
        std::string sp = slot_ptr(slot);
        std::string t = r();
        emit(t + " = call i32 @kami_truthy(ptr " + sp + ")");
        std::string b = r();
        emit(b + " = icmp ne i32 " + t + ", 0");
        return b;
    }

    void assign_var(Res res, int64_t idx, int src_slot) {
        std::string sp = slot_ptr(src_slot);
        if (res == Res::Local) {
            std::string dp = slot_ptr((int)idx);
            emit("call void @kami_copy(ptr " + dp + ", ptr " + sp + ")");
        } else {
            emit("call void @kami_global_set(i64 " + std::to_string(idx) + ", ptr " + sp + ")");
        }
    }
    void assign_kind(int kind, int64_t idx, int src_slot) {
        assign_var(kind == 1 ? Res::Local : Res::Global, idx, src_slot);
    }

    int read_var(Res res, int64_t idx) {
        if (res == Res::Local) return (int)idx;
        int t = alloc_temp();
        std::string tp = slot_ptr(t);
        emit("call void @kami_global_get(ptr " + tp + ", i64 " + std::to_string(idx) + ")");
        return t;
    }

    std::string str_const(const std::string& s) { return strtab.ref(strtab.intern(s)); }

    // ---- expressions ----
    int gen_expr(const Expr* e) {
        switch (e->kind) {
        case ExprKind::IntLit: {
            int t = alloc_temp();
            emit("call void @kami_make_int(ptr " + slot_ptr(t) + ", i64 " +
                 std::to_string(e->ival) + ")");
            return t;
        }
        case ExprKind::FloatLit: {
            int t = alloc_temp();
            emit("call void @kami_make_float(ptr " + slot_ptr(t) + ", double " +
                 fmt_double(e->fval) + ")");
            return t;
        }
        case ExprKind::BoolLit: {
            int t = alloc_temp();
            emit("call void @kami_make_bool(ptr " + slot_ptr(t) + ", i64 " +
                 std::to_string(e->ival) + ")");
            return t;
        }
        case ExprKind::NoneLit: {
            int t = alloc_temp();
            emit("call void @kami_make_none(ptr " + slot_ptr(t) + ")");
            return t;
        }
        case ExprKind::StrLit: {
            int t = alloc_temp();
            emit("call void @kami_make_str(ptr " + slot_ptr(t) + ", ptr " + str_const(e->sval) +
                 ", i64 " + std::to_string(e->sval.size()) + ")");
            return t;
        }
        case ExprKind::Name:
            if (e->res == Res::BuiltinFunc) {
                int t = alloc_temp();
                emit("call void @kami_make_builtin_func(ptr " + slot_ptr(t) + ", i64 " +
                     std::to_string(e->res_idx) + ", ptr " + str_const(e->sval) + ")");
                return t;
            }
            if (e->res == Res::Capture) {
                int t = alloc_temp();
                std::string cp = r();
                emit(cp + " = getelementptr inbounds %kv, ptr %captures, i64 " +
                     std::to_string(e->res_idx));
                emit("call void @kami_copy(ptr " + slot_ptr(t) + ", ptr " + cp + ")");
                return t;
            }
            return read_var(e->res, e->res_idx);
        case ExprKind::Binary: {
            int save = temp_top;
            int a = gen_expr(e->a.get());
            int b = gen_expr(e->b.get());
            std::string ap = slot_ptr(a), bp = slot_ptr(b);
            temp_top = save;
            int t = alloc_temp();
            emit("call void @kami_binop(i64 " + std::to_string(e->op) + ", ptr " + slot_ptr(t) +
                 ", ptr " + ap + ", ptr " + bp + ")");
            return t;
        }
        case ExprKind::Unary: {
            int save = temp_top;
            int a = gen_expr(e->a.get());
            std::string ap = slot_ptr(a);
            temp_top = save;
            int t = alloc_temp();
            emit("call void @kami_unop(i64 " + std::to_string(e->op) + ", ptr " + slot_ptr(t) +
                 ", ptr " + ap + ")");
            return t;
        }
        case ExprKind::BoolOp: {
            int t = alloc_temp();
            int save = temp_top;
            int a = gen_expr(e->a.get());
            emit("call void @kami_copy(ptr " + slot_ptr(t) + ", ptr " + slot_ptr(a) + ")");
            temp_top = save;
            std::string b = truthy(t);
            std::string Leval = newlabel(), Lend = newlabel();
            if (e->op == 0)
                emit("br i1 " + b + ", label %" + Leval + ", label %" + Lend);
            else
                emit("br i1 " + b + ", label %" + Lend + ", label %" + Leval);
            C().terminated = true;
            start_block(Leval);
            int save2 = temp_top;
            int bslot = gen_expr(e->b.get());
            emit("call void @kami_copy(ptr " + slot_ptr(t) + ", ptr " + slot_ptr(bslot) + ")");
            temp_top = save2;
            start_block(Lend);
            return t;
        }
        case ExprKind::IfExp: {
            int t = alloc_temp();
            int save = temp_top;
            int c = gen_expr(e->b.get());
            std::string b = truthy(c);
            temp_top = save;
            std::string Lthen = newlabel(), Lelse = newlabel(), Lend = newlabel();
            emit("br i1 " + b + ", label %" + Lthen + ", label %" + Lelse);
            C().terminated = true;
            start_block(Lthen);
            {
                int s2 = temp_top;
                int v = gen_expr(e->a.get());
                emit("call void @kami_copy(ptr " + slot_ptr(t) + ", ptr " + slot_ptr(v) + ")");
                temp_top = s2;
            }
            emit("br label %" + Lend);
            C().terminated = true;
            start_block(Lelse);
            {
                int s2 = temp_top;
                int v = gen_expr(e->c.get());
                emit("call void @kami_copy(ptr " + slot_ptr(t) + ", ptr " + slot_ptr(v) + ")");
                temp_top = s2;
            }
            start_block(Lend);
            return t;
        }
        case ExprKind::Index: {
            int save = temp_top;
            int a = gen_expr(e->a.get());
            int b = gen_expr(e->b.get());
            std::string ap = slot_ptr(a), bp = slot_ptr(b);
            temp_top = save;
            int t = alloc_temp();
            emit("call void @kami_index_get(ptr " + slot_ptr(t) + ", ptr " + ap + ", ptr " + bp +
                 ")");
            return t;
        }
        case ExprKind::Slice: {
            int save = temp_top;
            int base = gen_expr(e->a.get());
            int parts[3];
            for (int i = 0; i < 3; i++) {
                if (e->args[i]) {
                    parts[i] = gen_expr(e->args[i].get());
                } else {
                    parts[i] = alloc_temp();
                    emit("call void @kami_make_none(ptr " + slot_ptr(parts[i]) + ")");
                }
            }
            std::string bp = slot_ptr(base), p0 = slot_ptr(parts[0]), p1 = slot_ptr(parts[1]),
                        p2 = slot_ptr(parts[2]);
            temp_top = save;
            int t = alloc_temp();
            emit("call void @kami_slice(ptr " + slot_ptr(t) + ", ptr " + bp + ", ptr " + p0 +
                 ", ptr " + p1 + ", ptr " + p2 + ")");
            return t;
        }
        case ExprKind::Attr: {
            int save = temp_top;
            int base = gen_expr(e->a.get());
            std::string bp = slot_ptr(base);
            temp_top = save;
            int t = alloc_temp();
            emit("call void @kami_attr_get(ptr " + slot_ptr(t) + ", ptr " + bp + ", ptr " +
                 str_const(e->sval) + ")");
            return t;
        }
        case ExprKind::ListLit: {
            bool anyStar = false;
            for (auto& a : e->args)
                if (a->kind == ExprKind::Starred) anyStar = true;
            if (!anyStar) {
                int save = temp_top;
                std::vector<int> slots;
                for (auto& a : e->args) slots.push_back(gen_expr(a.get()));
                fill_argbuf(slots);
                temp_top = save;
                int t = alloc_temp();
                emit("call void @kami_make_list(ptr " + slot_ptr(t) + ", ptr %argbuf, i64 " +
                     std::to_string(slots.size()) + ")");
                return t;
            }
            // starred: start empty, append plain items / extend starred iterables
            int t = alloc_temp();
            emit("call void @kami_make_list(ptr " + slot_ptr(t) + ", ptr %argbuf, i64 0)");
            for (auto& a : e->args) {
                int save = temp_top;
                if (a->kind == ExprKind::Starred) {
                    int seq = gen_expr(a->a.get());
                    std::vector<int> one{seq};
                    fill_argbuf(one);
                    int dummy = alloc_temp();
                    emit("call void @kami_method(ptr " + slot_ptr(dummy) + ", ptr " +
                         slot_ptr(t) + ", ptr " + str_const("extend") +
                         ", ptr %argbuf, i64 1)");
                } else {
                    int v = gen_expr(a.get());
                    std::vector<int> one{v};
                    fill_argbuf(one);
                    int dummy = alloc_temp();
                    emit("call void @kami_method(ptr " + slot_ptr(dummy) + ", ptr " +
                         slot_ptr(t) + ", ptr " + str_const("append") +
                         ", ptr %argbuf, i64 1)");
                }
                temp_top = save;
            }
            return t;
        }
        case ExprKind::Starred:
            throw CompileError(e->line, "* is only allowed inside a list/call");
        case ExprKind::SetLit: {
            int t = alloc_temp();
            emit("call void @kami_make_set(ptr " + slot_ptr(t) + ")");
            for (auto& a : e->args) {
                int save = temp_top;
                int v = gen_expr(a.get());
                emit("call void @kami_set_add(ptr " + slot_ptr(t) + ", ptr " + slot_ptr(v) +
                     ")");
                temp_top = save;
            }
            return t;
        }
        case ExprKind::MapLit: {
            int t = alloc_temp();
            emit("call void @kami_make_map(ptr " + slot_ptr(t) + ")");
            for (auto& p : e->pairs) {
                int save = temp_top;
                int k = gen_expr(p.first.get());
                int v = gen_expr(p.second.get());
                emit("call void @kami_index_set(ptr " + slot_ptr(t) + ", ptr " + slot_ptr(k) +
                     ", ptr " + slot_ptr(v) + ")");
                temp_top = save;
            }
            return t;
        }
        case ExprKind::ListComp:
        case ExprKind::SetComp:
        case ExprKind::MapComp:
            return gen_comprehension(e);
        case ExprKind::Closure:
            return gen_closure(e->res_idx);
        case ExprKind::Call: {
            const Expr* callee = e->a.get();
            // Result slot is allocated BEFORE the arguments so it can never
            // alias an argument slot: kami_call_value writes the result (e.g. a
            // new instance) before user __init__ code reads the arguments.
            int t = alloc_temp();
            int save = temp_top;
            std::vector<int> slots;
            for (auto& a : e->args) slots.push_back(gen_expr(a.get()));
            if (callee->res == Res::UserFunc) {
                fill_argbuf(slots);
                const Stmt* f = mod.functions[(size_t)callee->res_idx];
                emit("call void @u_" + f->alias + "(ptr " + slot_ptr(t) +
                     ", ptr %argbuf, i64 " + std::to_string(slots.size()) + ", ptr null)");
                temp_top = save;
                return t;
            }
            if (callee->res == Res::BuiltinFunc) {
                fill_argbuf(slots);
                emit("call void @kami_builtin(i64 " + std::to_string(callee->res_idx) + ", ptr " +
                     slot_ptr(t) + ", ptr %argbuf, i64 " + std::to_string(slots.size()) + ")");
                temp_top = save;
                return t;
            }
            int c = gen_expr(callee);
            std::string cp = slot_ptr(c);
            fill_argbuf(slots);
            emit("call void @kami_call_value(ptr " + slot_ptr(t) + ", ptr " + cp +
                 ", ptr %argbuf, i64 " + std::to_string(slots.size()) + ")");
            temp_top = save;
            return t;
        }
        case ExprKind::CCall: return gen_ccall(e);
        case ExprKind::MethodCall: {
            int t = alloc_temp(); // before args: see Call above
            int save = temp_top;
            int base = gen_expr(e->a.get());
            std::string basep = slot_ptr(base);
            std::vector<int> slots;
            for (auto& a : e->args) slots.push_back(gen_expr(a.get()));
            fill_argbuf(slots);
            emit("call void @kami_method(ptr " + slot_ptr(t) + ", ptr " + basep + ", ptr " +
                 str_const(e->sval) + ", ptr %argbuf, i64 " + std::to_string(slots.size()) +
                 ")");
            temp_top = save;
            return t;
        }
        }
        throw CompileError(e->line, "internal error: bad expression kind in codegen");
    }

    // ---- direct C-ABI calls ------------------------------------------------
    // The whole point of a CCall is that no dynamic dispatch happens: values are
    // unboxed into machine types, the C function is called exactly as C would
    // call it, and the result is boxed back. `sqrt(2.0)` becomes three
    // instructions, not a builtin-table lookup.
    static const char* ir_type(char code) {
        switch (code) {
        case 'i': return "i64";
        case 'l': return "i32";
        case 'd': return "double";
        case 'f': return "float";
        case 's': return "ptr";
        default: return "void";
        }
    }

    int gen_ccall(const Expr* e) {
        int t = alloc_temp(); // result slot first: see Call above
        int save = temp_top;
        std::vector<int> slots;
        for (auto& a : e->args) slots.push_back(gen_expr(a.get()));
        // Unbox every argument through the runtime, which raises a catchable
        // Python-level error on a type mismatch instead of corrupting the stack.
        // The helpers always yield the widest form (i64 / double / ptr); narrow
        // parameters are then truncated to their real C width.
        std::vector<std::string> vals;
        for (size_t i = 0; i < slots.size(); i++) {
            char code = e->csig[i + 1];
            const char* helper = (code == 'd' || code == 'f')
                                     ? "kami_c_arg_f64"
                                     : (code == 's' ? "kami_c_arg_cstr" : "kami_c_arg_i64");
            const char* wide = (code == 'd' || code == 'f') ? "double"
                                                           : (code == 's' ? "ptr" : "i64");
            std::string v = r();
            emit(v + " = call " + wide + " @" + helper + "(ptr " + slot_ptr(slots[i]) + ")");
            if (code == 'l') {
                std::string n = r();
                emit(n + " = trunc i64 " + v + " to i32");
                v = n;
            } else if (code == 'f') {
                std::string n = r();
                emit(n + " = fptrunc double " + v + " to float");
                v = n;
            }
            vals.push_back(std::string(ir_type(code)) + " " + v);
        }
        std::string argstr;
        for (size_t i = 0; i < vals.size(); i++) argstr += (i ? ", " : "") + vals[i];
        char ret = e->csig[0];
        std::string rp = slot_ptr(t);
        if (ret == 'v') {
            emit("call void @" + e->sval + "(" + argstr + ")");
            emit("call void @kami_make_none(ptr " + rp + ")");
            temp_top = save;
            return t;
        }
        std::string rv = r();
        emit(rv + " = call " + ir_type(ret) + " @" + e->sval + "(" + argstr + ")");
        if (ret == 'l') { // C int → Python int: sign-extend, never read garbage
            std::string w = r();
            emit(w + " = sext i32 " + rv + " to i64");
            emit("call void @kami_make_int(ptr " + rp + ", i64 " + w + ")");
        } else if (ret == 'f') {
            std::string w = r();
            emit(w + " = fpext float " + rv + " to double");
            emit("call void @kami_make_float(ptr " + rp + ", double " + w + ")");
        } else if (ret == 'd') {
            emit("call void @kami_make_float(ptr " + rp + ", double " + rv + ")");
        } else if (ret == 's') {
            emit("call void @kami_c_ret_cstr(ptr " + rp + ", ptr " + rv + ")");
        } else {
            emit("call void @kami_make_int(ptr " + rp + ", i64 " + rv + ")");
        }
        temp_top = save;
        return t;
    }

    // Materialize a capture source (in enclosing-frame terms) into a slot,
    // returning that slot index.
    int capture_source_slot(int kind, int64_t idx) {
        if (kind == 1) return (int)idx; // enclosing local slot
        int t = alloc_temp();
        if (kind == 2) { // global
            emit("call void @kami_global_get(ptr " + slot_ptr(t) + ", i64 " +
                 std::to_string(idx) + ")");
        } else { // 3: enclosing capture
            std::string cp = r();
            emit(cp + " = getelementptr inbounds %kv, ptr %captures, i64 " +
                 std::to_string(idx));
            emit("call void @kami_copy(ptr " + slot_ptr(t) + ", ptr " + cp + ")");
        }
        return t;
    }

    int gen_closure(int64_t func_index) {
        const Stmt* fn = mod.functions[(size_t)func_index];
        int result = alloc_temp();
        int save = temp_top;
        std::vector<int> caps;
        for (size_t i = 0; i < fn->multi_tkind.size(); i++)
            caps.push_back(capture_source_slot(fn->multi_tkind[i], fn->multi_tidx[i]));
        fill_argbuf(caps);
        size_t fmin = fn->params.size() - fn->defaults.size() - (fn->vararg ? 1 : 0);
        emit("call void @kami_make_closure(ptr " + slot_ptr(result) + ", ptr @u_" + fn->alias +
             ", i64 " + std::to_string(fmin) + ", i64 " + std::to_string(max_arity(fn)) +
             ", ptr " + str_const(fn->name) + ", ptr %argbuf, i64 " +
             std::to_string(caps.size()) + ")");
        temp_top = save;
        return result;
    }

    int gen_comprehension(const Expr* e) {
        int result = alloc_temp();
        if (e->kind == ExprKind::ListComp)
            emit("call void @kami_make_list(ptr " + slot_ptr(result) + ", ptr %argbuf, i64 0)");
        else if (e->kind == ExprKind::SetComp)
            emit("call void @kami_make_set(ptr " + slot_ptr(result) + ")");
        else
            emit("call void @kami_make_map(ptr " + slot_ptr(result) + ")");
        // emit the innermost body (element production)
        std::function<void()> emit_elem = [&]() {
            int save2 = temp_top;
            if (e->kind == ExprKind::MapComp) {
                int k = gen_expr(e->pairs[0].first.get());
                int v = gen_expr(e->pairs[0].second.get());
                emit("call void @kami_index_set(ptr " + slot_ptr(result) + ", ptr " +
                     slot_ptr(k) + ", ptr " + slot_ptr(v) + ")");
            } else if (e->kind == ExprKind::SetComp) {
                int v = gen_expr(e->a.get());
                emit("call void @kami_set_add(ptr " + slot_ptr(result) + ", ptr " +
                     slot_ptr(v) + ")");
            } else {
                int v = gen_expr(e->a.get());
                std::vector<int> one{v};
                fill_argbuf(one);
                int dummy = alloc_temp();
                emit("call void @kami_method(ptr " + slot_ptr(dummy) + ", ptr " +
                     slot_ptr(result) + ", ptr " + str_const("append") +
                     ", ptr %argbuf, i64 1)");
            }
            temp_top = save2;
        };
        // recursively nest the clauses
        std::function<void(size_t)> gen_clause = [&](size_t ci) {
            if (ci == e->clauses.size()) { emit_elem(); return; }
            const CompClause& cl = e->clauses[ci];
            auto body = [&](int elem_slot) {
                if (cl.targets.size() == 1) {
                    assign_kind(cl.tkind[0], cl.tidx[0], elem_slot);
                } else {
                    for (size_t i = 0; i < cl.targets.size(); i++) {
                        int save2 = temp_top;
                        int part = alloc_temp();
                        emit("call void @kami_unpack(ptr " + slot_ptr(part) + ", ptr " +
                             slot_ptr(elem_slot) + ", i64 " + std::to_string(i) + ", i64 " +
                             std::to_string(cl.targets.size()) + ")");
                        assign_kind(cl.tkind[i], cl.tidx[i], part);
                        temp_top = save2;
                    }
                }
                // conditions: skip element if any is falsey
                std::vector<std::string> skips;
                for (auto& cptr : cl.conds) {
                    int save2 = temp_top;
                    int c = gen_expr(cptr.get());
                    std::string b = truthy(c);
                    temp_top = save2;
                    std::string Lyes = newlabel(), Lskip = newlabel();
                    emit("br i1 " + b + ", label %" + Lyes + ", label %" + Lskip);
                    C().terminated = true;
                    start_block(Lyes);
                    skips.push_back(Lskip);
                }
                gen_clause(ci + 1);
                for (size_t i = skips.size(); i-- > 0;) start_block(skips[i]);
            };
            gen_iteration(cl.iter.get(), body, nullptr, nullptr);
        };
        gen_clause(0);
        return result;
    }

    // Shared iteration engine: iterates `iter` (range-call specialization or
    // generic sequence), calling body(elem_slot). Loop labels returned for
    // break/continue via out params when used by For.
    template <typename BodyFn>
    void gen_iteration(const Expr* iter, BodyFn body, std::string* out_step,
                       std::string* out_end, std::function<void()> on_normal_exit = {}) {
        bool is_range = iter->kind == ExprKind::Call &&
                        iter->a->res == Res::BuiltinFunc && iter->a->res_idx == KB_RANGE;
        if (is_range) {
            const auto& args = iter->args;
            int s_start, s_stop, s_step;
            if (args.size() == 1) {
                s_start = alloc_temp();
                emit("call void @kami_make_int(ptr " + slot_ptr(s_start) + ", i64 0)");
                s_stop = gen_expr(args[0].get());
            } else {
                s_start = gen_expr(args[0].get());
                s_stop = gen_expr(args[1].get());
            }
            if (args.size() == 3) {
                s_step = gen_expr(args[2].get());
            } else {
                s_step = alloc_temp();
                emit("call void @kami_make_int(ptr " + slot_ptr(s_step) + ", i64 1)");
            }
            int s_i = alloc_temp();
            emit("call void @kami_copy(ptr " + slot_ptr(s_i) + ", ptr " + slot_ptr(s_start) +
                 ")");
            std::string Lcond = newlabel(), Lbody = newlabel(), Lstep = newlabel(),
                        Lelse = newlabel(), Lend = newlabel();
            if (out_step) *out_step = Lstep;
            if (out_end) *out_end = Lend;
            start_block(Lcond);
            std::string c = r();
            emit(c + " = call i32 @kami_range_cond(ptr " + slot_ptr(s_i) + ", ptr " +
                 slot_ptr(s_stop) + ", ptr " + slot_ptr(s_step) + ")");
            std::string b = r();
            emit(b + " = icmp ne i32 " + c + ", 0");
            emit("br i1 " + b + ", label %" + Lbody + ", label %" + Lelse);
            C().terminated = true;
            start_block(Lbody);
            body(s_i);
            start_block(Lstep);
            emit("call void @kami_binop(i64 " + std::to_string((int)KOP_ADD) + ", ptr " +
                 slot_ptr(s_i) + ", ptr " + slot_ptr(s_i) + ", ptr " + slot_ptr(s_step) + ")");
            emit("br label %" + Lcond);
            C().terminated = true;
            start_block(Lelse); // normal exit (loop exhausted, not break) → for/else
            if (on_normal_exit) on_normal_exit();
            if (!C().terminated) emit("br label %" + Lend);
            C().terminated = true;
            start_block(Lend);
            return;
        }
        int s_raw = gen_expr(iter);
        int s_seq = alloc_temp();
        emit("call void @kami_iter_prep(ptr " + slot_ptr(s_seq) + ", ptr " + slot_ptr(s_raw) +
             ")");
        int s_idx = alloc_temp();
        emit("call void @kami_make_int(ptr " + slot_ptr(s_idx) + ", i64 0)");
        int s_one = alloc_temp();
        emit("call void @kami_make_int(ptr " + slot_ptr(s_one) + ", i64 1)");
        std::string Lcond = newlabel(), Lbody = newlabel(), Lstep = newlabel(),
                    Lelse = newlabel(), Lend = newlabel();
        if (out_step) *out_step = Lstep;
        if (out_end) *out_end = Lend;
        start_block(Lcond);
        std::string c = r();
        emit(c + " = call i32 @kami_iter_cond(ptr " + slot_ptr(s_seq) + ", ptr " +
             slot_ptr(s_idx) + ")");
        std::string b = r();
        emit(b + " = icmp ne i32 " + c + ", 0");
        emit("br i1 " + b + ", label %" + Lbody + ", label %" + Lelse);
        C().terminated = true;
        start_block(Lbody);
        int elem = alloc_temp();
        emit("call void @kami_iter_get(ptr " + slot_ptr(elem) + ", ptr " + slot_ptr(s_seq) +
             ", ptr " + slot_ptr(s_idx) + ")");
        body(elem);
        start_block(Lstep);
        emit("call void @kami_binop(i64 " + std::to_string((int)KOP_ADD) + ", ptr " +
             slot_ptr(s_idx) + ", ptr " + slot_ptr(s_idx) + ", ptr " + slot_ptr(s_one) + ")");
        emit("br label %" + Lcond);
        C().terminated = true;
        start_block(Lelse); // normal exit → for/else clause
        if (on_normal_exit) on_normal_exit();
        if (!C().terminated) emit("br label %" + Lend);
        C().terminated = true;
        start_block(Lend);
    }

    // ---- statements ----
    void gen_stmts(const std::vector<StmtPtr>& body) {
        for (auto& s : body) {
            if (C().terminated) break;
            gen_stmt(s.get());
        }
    }

    void emit_return_value(const Stmt* s) {
        if (s->e1) {
            int v = gen_expr(s->e1.get());
            if (C().outlined) {
                emit("call void @kami_copy(ptr " + slot_ptr(spill_slot) + ", ptr " +
                     slot_ptr(v) + ")");
            } else {
                emit("call void @kami_copy(ptr %ret, ptr " + slot_ptr(v) + ")");
            }
        } else {
            if (C().outlined)
                emit("call void @kami_make_none(ptr " + slot_ptr(spill_slot) + ")");
            else
                emit("call void @kami_make_none(ptr %ret)");
        }
    }

    void gen_stmt(const Stmt* s) {
        int save = temp_top;
        switch (s->kind) {
        case StmtKind::ExprStmt:
            gen_expr(s->e1.get());
            break;
        case StmtKind::Assign: {
            int v = gen_expr(s->e1.get());
            assign_var(s->target_res, s->target_idx, v);
            break;
        }
        case StmtKind::IndexAssign: {
            int base = gen_expr(s->e1.get());
            int idx = gen_expr(s->e2.get());
            int val = gen_expr(s->e3.get());
            emit("call void @kami_index_set(ptr " + slot_ptr(base) + ", ptr " + slot_ptr(idx) +
                 ", ptr " + slot_ptr(val) + ")");
            break;
        }
        case StmtKind::AttrAssign: {
            int base = gen_expr(s->e1.get());
            int val = gen_expr(s->e3.get());
            emit("call void @kami_attr_set(ptr " + slot_ptr(base) + ", ptr " +
                 str_const(s->name) + ", ptr " + slot_ptr(val) + ")");
            break;
        }
        case StmtKind::MultiAssign:
            gen_multi_assign(s);
            break;
        case StmtKind::If: {
            int c = gen_expr(s->e1.get());
            std::string b = truthy(c);
            temp_top = save;
            std::string Lthen = newlabel();
            std::string Lelse = s->orelse.empty() ? "" : newlabel();
            std::string Lend = newlabel();
            emit("br i1 " + b + ", label %" + Lthen + ", label %" +
                 (Lelse.empty() ? Lend : Lelse));
            C().terminated = true;
            start_block(Lthen);
            gen_stmts(s->body);
            if (!C().terminated) emit("br label %" + Lend);
            C().terminated = true;
            if (!Lelse.empty()) {
                start_block(Lelse);
                gen_stmts(s->orelse);
                if (!C().terminated) emit("br label %" + Lend);
                C().terminated = true;
            }
            start_block(Lend);
            break;
        }
        case StmtKind::While: {
            std::string Lcond = newlabel(), Lbody = newlabel(), Lelse = newlabel(),
                        Lend = newlabel();
            start_block(Lcond);
            int c = gen_expr(s->e1.get());
            std::string b = truthy(c);
            temp_top = save;
            emit("br i1 " + b + ", label %" + Lbody + ", label %" + Lelse);
            C().terminated = true;
            start_block(Lbody);
            C().loops.push_back({Lcond, Lend});
            gen_stmts(s->body);
            C().loops.pop_back();
            if (!C().terminated) emit("br label %" + Lcond);
            C().terminated = true;
            start_block(Lelse); // ran when the condition became false (no break)
            if (!s->orelse.empty()) gen_stmts(s->orelse);
            if (!C().terminated) emit("br label %" + Lend);
            C().terminated = true;
            start_block(Lend);
            break;
        }
        case StmtKind::For: {
            std::string Lstep, Lend;
            auto body = [&](int elem) {
                if (s->params.size() == 1) {
                    assign_var(s->target_res, s->target_idx, elem);
                } else {
                    for (size_t i = 0; i < s->params.size(); i++) {
                        int save2 = temp_top;
                        int part = alloc_temp();
                        emit("call void @kami_unpack(ptr " + slot_ptr(part) + ", ptr " +
                             slot_ptr(elem) + ", i64 " + std::to_string(i) + ", i64 " +
                             std::to_string(s->params.size()) + ")");
                        assign_kind(s->multi_tkind[i], s->multi_tidx[i], part);
                        temp_top = save2;
                    }
                }
                // loop body runs with break/continue targets registered lazily:
                // we don't know Lstep/Lend inside this lambda's call position, so
                // gen_iteration fills them via out params *before* calling us.
                C().loops.push_back({Lstep, Lend});
                gen_stmts(s->body);
                C().loops.pop_back();
            };
            gen_iteration(s->e1.get(), body, &Lstep, &Lend,
                          s->orelse.empty() ? std::function<void()>{}
                                            : [&]() { gen_stmts(s->orelse); });
            break;
        }
        case StmtKind::FuncDef:
            if (s->global_idx < 0) { // nested function -> closure bound to a local
                int c = gen_closure(s->func_index);
                assign_var(s->target_res, s->target_idx, c);
            } else if (!s->decorators.empty()) {
                // top-level decorated function: name = d1(d2(...(name)))
                int cur = alloc_temp();
                emit("call void @kami_global_get(ptr " + slot_ptr(cur) + ", i64 " +
                     std::to_string(s->global_idx) + ")");
                for (size_t i = s->decorators.size(); i-- > 0;)
                    cur = apply_decorator(s->decorators[i].get(), cur);
                std::string sp = slot_ptr(cur);
                emit("call void @kami_global_set(i64 " + std::to_string(s->global_idx) +
                     ", ptr " + sp + ")");
            }
            break;
        case StmtKind::ClassDef: {
            if (!s->decorators.empty()) {
                int cur = alloc_temp();
                emit("call void @kami_global_get(ptr " + slot_ptr(cur) + ", i64 " +
                     std::to_string(s->global_idx) + ")");
                for (size_t i = s->decorators.size(); i-- > 0;)
                    cur = apply_decorator(s->decorators[i].get(), cur);
                emit("call void @kami_global_set(i64 " + std::to_string(s->global_idx) +
                     ", ptr " + slot_ptr(cur) + ")");
            }
            // class attribute assignments run here, in module order
            for (auto& msp : s->body) {
                if (msp->kind != StmtKind::Assign) continue;
                int save2 = temp_top;
                int cls = alloc_temp();
                emit("call void @kami_global_get(ptr " + slot_ptr(cls) + ", i64 " +
                     std::to_string(s->global_idx) + ")");
                int v = gen_expr(msp->e1.get());
                emit("call void @kami_attr_set(ptr " + slot_ptr(cls) + ", ptr " +
                     str_const(msp->name) + ", ptr " + slot_ptr(v) + ")");
                temp_top = save2;
            }
            break;
        }
        case StmtKind::Return: {
            emit_return_value(s);
            if (C().outlined) {
                emit("ret i64 1");
            } else {
                emit("call void @kami_frame_pop()");
                emit("ret void");
            }
            C().terminated = true;
            break;
        }
        case StmtKind::Break:
            if (!C().loops.empty()) {
                emit("br label %" + C().loops.back().second);
            } else {
                emit("ret i64 2"); // propagate through kami_try
            }
            C().terminated = true;
            break;
        case StmtKind::Continue:
            if (!C().loops.empty()) {
                emit("br label %" + C().loops.back().first);
            } else {
                emit("ret i64 3");
            }
            C().terminated = true;
            break;
        case StmtKind::Raise:
            gen_raise(s);
            break;
        case StmtKind::Try:
            gen_try(s);
            break;
        case StmtKind::Del:
            for (auto& t : s->targets) {
                int save2 = temp_top;
                if (t->kind == ExprKind::Index) {
                    int base = gen_expr(t->a.get());
                    int idx = gen_expr(t->b.get());
                    emit("call void @kami_del_index(ptr " + slot_ptr(base) + ", ptr " +
                         slot_ptr(idx) + ")");
                } else { // Name: rebind to None (best effort)
                    int none = alloc_temp();
                    emit("call void @kami_make_none(ptr " + slot_ptr(none) + ")");
                    assign_var(t->res, t->res_idx, none);
                }
                temp_top = save2;
            }
            break;
        case StmtKind::With:
            gen_with(s);
            break;
        case StmtKind::Pass:
        case StmtKind::Import:
        case StmtKind::FromImport:
        case StmtKind::Global:
            break;
        }
        temp_top = save;
    }

    int apply_decorator(const Expr* deco, int arg_slot) {
        int fn = gen_expr(deco);
        std::string fp = slot_ptr(fn);
        std::vector<int> a{arg_slot};
        fill_argbuf(a);
        int t = alloc_temp();
        emit("call void @kami_call_value(ptr " + slot_ptr(t) + ", ptr " + fp +
             ", ptr %argbuf, i64 1)");
        return t;
    }

    void gen_multi_assign(const Stmt* s) {
        auto assign_target = [&](const Expr* t, int vslot) {
            if (t->kind == ExprKind::Name) {
                assign_var(t->res, t->res_idx, vslot);
            } else if (t->kind == ExprKind::Index) {
                int save2 = temp_top;
                int base = gen_expr(t->a.get());
                int idx = gen_expr(t->b.get());
                emit("call void @kami_index_set(ptr " + slot_ptr(base) + ", ptr " +
                     slot_ptr(idx) + ", ptr " + slot_ptr(vslot) + ")");
                temp_top = save2;
            } else { // Attr
                int save2 = temp_top;
                int base = gen_expr(t->a.get());
                emit("call void @kami_attr_set(ptr " + slot_ptr(base) + ", ptr " +
                     str_const(t->sval) + ", ptr " + slot_ptr(vslot) + ")");
                temp_top = save2;
            }
        };
        if (s->alias == "chain") { // a = b = value
            int v = gen_expr(s->values[0].get());
            for (auto& t : s->targets) assign_target(t.get(), v);
            return;
        }
        if (s->values.size() > 1) { // a, b = x, y : evaluate all RHS first
            std::vector<int> vs;
            for (auto& v : s->values) vs.push_back(gen_expr(v.get()));
            for (size_t i = 0; i < s->targets.size(); i++)
                assign_target(s->targets[i].get(), vs[i]);
            return;
        }
        // a, b = expr : runtime unpack
        int v = gen_expr(s->values[0].get());
        for (size_t i = 0; i < s->targets.size(); i++) {
            int save2 = temp_top;
            int part = alloc_temp();
            emit("call void @kami_unpack(ptr " + slot_ptr(part) + ", ptr " + slot_ptr(v) +
                 ", i64 " + std::to_string(i) + ", i64 " + std::to_string(s->targets.size()) +
                 ")");
            assign_target(s->targets[i].get(), part);
            temp_top = save2;
        }
    }

    // with ctx as name:  body   →  ctx-mgr enter/exit around an outlined body
    // so __exit__ runs even if the body raises (via kami_try's unwind repair).
    void gen_with(const Stmt* s) {
        int save = temp_top;
        int ctx = gen_expr(s->e1.get());
        // keep the context manager alive in a stable frame slot
        int ctx_slot = alloc_slot_persistent();
        emit("call void @kami_copy(ptr " + slot_ptr(ctx_slot) + ", ptr " + slot_ptr(ctx) + ")");
        // entered = with_enter(ctx)
        std::vector<int> a1{ctx_slot};
        fill_argbuf(a1);
        int entered = alloc_slot_persistent();
        emit("call void @kami_builtin(i64 " + std::to_string((int64_t)KB_WITH_ENTER) + ", ptr " +
             slot_ptr(entered) + ", ptr %argbuf, i64 1)");
        if (!s->name.empty()) assign_var(s->target_res, s->target_idx, entered);
        temp_top = save;

        // outline the body so __exit__ always runs
        std::string fname = "kami_withb_" + std::to_string(g_outline_counter++);
        ctxs.emplace_back();
        C().fname = fname;
        C().outlined = true;
        gen_stmts(s->body);
        if (!C().terminated) emit("ret i64 0");
        {
            Ctx done = std::move(ctxs.back());
            ctxs.pop_back();
            extra_fns += "define internal i64 @" + done.fname +
                         "(ptr %frame) {\nentry:\n  %argbuf = alloca ptr, i64 @@ARGBUF@@, "
                         "align 8\n" +
                         done.body + "}\n\n";
        }
        std::string code = r();
        emit(code + " = call i64 @kami_try(ptr @" + fname + ", ptr %frame)");
        // __exit__ always
        std::vector<int> a2{ctx_slot};
        fill_argbuf(a2);
        int dummy = alloc_temp();
        emit("call void @kami_builtin(i64 " + std::to_string((int64_t)KB_WITH_EXIT) + ", ptr " +
             slot_ptr(dummy) + ", ptr %argbuf, i64 1)");
        temp_top = save;
        // propagate exception / control flow (same protocol as gen_try tail)
        dispatch_try_code(code);
    }

    // Reused by gen_with: propagate a kami_try result code (-1 exc, 1 ret, 2/3
    // break/continue). Returns having possibly terminated the block.
    void dispatch_try_code(const std::string& code) {
        std::string isexc = r();
        emit(isexc + " = icmp eq i64 " + code + ", -1");
        std::string Lre = newlabel(), Lok = newlabel();
        emit("br i1 " + isexc + ", label %" + Lre + ", label %" + Lok);
        C().terminated = true;
        start_block(Lre);
        emit("call void @kami_rethrow()");
        emit("unreachable");
        C().terminated = true;
        start_block(Lok);
        {
            std::string isret = r();
            emit(isret + " = icmp eq i64 " + code + ", 1");
            std::string Lret = newlabel(), Ln = newlabel();
            emit("br i1 " + isret + ", label %" + Lret + ", label %" + Ln);
            C().terminated = true;
            start_block(Lret);
            if (C().outlined) {
                emit("ret i64 1");
            } else {
                emit("call void @kami_copy(ptr %ret, ptr " + slot_ptr(spill_slot) + ")");
                emit("call void @kami_frame_pop()");
                emit("ret void");
            }
            C().terminated = true;
            start_block(Ln);
        }
        if (!C().loops.empty() || C().outlined) {
            for (int64_t k = 2; k <= 3; k++) {
                std::string is = r();
                emit(is + " = icmp eq i64 " + code + ", " + std::to_string(k));
                std::string Ly = newlabel(), Ln = newlabel();
                emit("br i1 " + is + ", label %" + Ly + ", label %" + Ln);
                C().terminated = true;
                start_block(Ly);
                if (!C().loops.empty())
                    emit("br label %" +
                         (k == 2 ? C().loops.back().second : C().loops.back().first));
                else
                    emit("ret i64 " + std::to_string(k));
                C().terminated = true;
                start_block(Ln);
            }
        }
    }

    void gen_raise(const Stmt* s) {
        if (s->raise_mode == 1) {
            emit("call void @kami_rethrow()");
            emit("unreachable");
            C().terminated = true;
            return;
        }
        int msg;
        if (s->raise_mode == 2) {
            if (s->e1) {
                // msg = "Name: " + str(arg)
                int save = temp_top;
                int prefix = alloc_temp();
                std::string pf = s->name + ": ";
                emit("call void @kami_make_str(ptr " + slot_ptr(prefix) + ", ptr " +
                     str_const(pf) + ", i64 " + std::to_string(pf.size()) + ")");
                int v = gen_expr(s->e1.get());
                std::vector<int> one{v};
                fill_argbuf(one);
                int vs = alloc_temp();
                emit("call void @kami_builtin(i64 " + std::to_string((int64_t)KB_STR) +
                     ", ptr " + slot_ptr(vs) + ", ptr %argbuf, i64 1)");
                std::string pp = slot_ptr(prefix), vp = slot_ptr(vs);
                temp_top = save;
                msg = alloc_temp();
                emit("call void @kami_binop(i64 " + std::to_string((int)KOP_ADD) + ", ptr " +
                     slot_ptr(msg) + ", ptr " + pp + ", ptr " + vp + ")");
            } else {
                msg = alloc_temp();
                emit("call void @kami_make_str(ptr " + slot_ptr(msg) + ", ptr " +
                     str_const(s->name) + ", i64 " + std::to_string(s->name.size()) + ")");
            }
        } else {
            msg = gen_expr(s->e1.get());
        }
        emit("call void @kami_raise(ptr " + slot_ptr(msg) + ")");
        emit("unreachable");
        C().terminated = true;
    }

    void gen_try(const Stmt* s) {
        // 1) outline the body
        std::string fname = "kami_tryb_" + std::to_string(g_outline_counter++);
        ctxs.emplace_back();
        C().fname = fname;
        C().outlined = true;
        gen_stmts(s->body);
        if (!C().terminated) emit("ret i64 0");
        {
            Ctx done = std::move(ctxs.back());
            ctxs.pop_back();
            extra_fns += "define internal i64 @" + done.fname +
                         "(ptr %frame) {\n" +
                         "entry:\n  %argbuf = alloca ptr, i64 @@ARGBUF@@, align 8\n" +
                         done.body + "}\n\n";
        }
        // 2) protected call
        std::string code = r();
        emit(code + " = call i64 @kami_try(ptr @" + fname + ", ptr %frame)");
        // 3) exception dispatch
        if (!s->handlers.empty()) {
            std::string isexc = r();
            emit(isexc + " = icmp eq i64 " + code + ", -1");
            std::string Lexc = newlabel(), Lnoexc = newlabel();
            emit("br i1 " + isexc + ", label %" + Lexc + ", label %" + Lnoexc);
            C().terminated = true;
            start_block(Lexc);
            const ExceptClause& h = s->handlers[0]; // first handler catches all
            if (!h.as_name.empty()) {
                int save2 = temp_top;
                int ev = alloc_temp();
                emit("call void @kami_last_error(ptr " + slot_ptr(ev) + ")");
                assign_kind(h.as_kind, h.as_idx, ev);
                temp_top = save2;
            }
            gen_stmts(h.body);
            if (!C().terminated) emit("br label %" + Lnoexc);
            C().terminated = true;
            start_block(Lnoexc);
        }
        // 4) else clause: only when the body completed normally (code == 0)
        if (!s->orelse.empty()) {
            std::string isok = r();
            emit(isok + " = icmp eq i64 " + code + ", 0");
            std::string Lelse = newlabel(), Ljoin = newlabel();
            emit("br i1 " + isok + ", label %" + Lelse + ", label %" + Ljoin);
            C().terminated = true;
            start_block(Lelse);
            gen_stmts(s->orelse);
            if (!C().terminated) emit("br label %" + Ljoin);
            C().terminated = true;
            start_block(Ljoin);
        }
        // 5) finally
        if (!s->final_body.empty()) gen_stmts(s->final_body);
        if (C().terminated) return; // finally ended in return/raise
        // 6) uncaught exception (try/finally without except): rethrow
        if (s->handlers.empty()) {
            std::string isexc = r();
            emit(isexc + " = icmp eq i64 " + code + ", -1");
            std::string Lre = newlabel(), Lok = newlabel();
            emit("br i1 " + isexc + ", label %" + Lre + ", label %" + Lok);
            C().terminated = true;
            start_block(Lre);
            emit("call void @kami_rethrow()");
            emit("unreachable");
            C().terminated = true;
            start_block(Lok);
        }
        // 7) control-flow codes from the body
        if (!is_module) { // return
            std::string isret = r();
            emit(isret + " = icmp eq i64 " + code + ", 1");
            std::string Lret = newlabel(), Ln = newlabel();
            emit("br i1 " + isret + ", label %" + Lret + ", label %" + Ln);
            C().terminated = true;
            start_block(Lret);
            if (C().outlined) {
                emit("ret i64 1"); // propagate outward (spill already set)
            } else {
                emit("call void @kami_copy(ptr %ret, ptr " + slot_ptr(spill_slot) + ")");
                emit("call void @kami_frame_pop()");
                emit("ret void");
            }
            C().terminated = true;
            start_block(Ln);
        }
        bool can_break = !C().loops.empty() || C().outlined;
        if (can_break) {
            for (int64_t k = 2; k <= 3; k++) { // 2 = break, 3 = continue
                std::string is = r();
                emit(is + " = icmp eq i64 " + code + ", " + std::to_string(k));
                std::string Ly = newlabel(), Ln = newlabel();
                emit("br i1 " + is + ", label %" + Ly + ", label %" + Ln);
                C().terminated = true;
                start_block(Ly);
                if (!C().loops.empty()) {
                    emit("br label %" +
                         (k == 2 ? C().loops.back().second : C().loops.back().first));
                } else {
                    emit("ret i64 " + std::to_string(k));
                }
                C().terminated = true;
                start_block(Ln);
            }
        }
    }

    // Emits parameter copying (with default-value filling) into the current
    // body context. Must be called before generating any statements.
    void gen_prologue(const Stmt* f) {
        size_t nparams = f->params.size();
        size_t ndefaults = f->defaults.size();
        if (f->vararg) nparams--; // handled below, after the fixed parameters
        for (size_t i = 0; i < nparams; i++) {
            bool has_default = i + ndefaults >= nparams;
            if (!has_default) {
                std::string pi = r(), ai = r(), si = r();
                emit(pi + " = getelementptr inbounds ptr, ptr %argv, i64 " +
                     std::to_string(i));
                emit(ai + " = load ptr, ptr " + pi + ", align 8");
                emit(si + " = getelementptr inbounds %kv, ptr %frame, i64 " +
                     std::to_string(i));
                emit("call void @kami_copy(ptr " + si + ", ptr " + ai + ")");
                continue;
            }
            const Expr* dflt = f->defaults[i + ndefaults - nparams].get();
            std::string have = r();
            emit(have + " = icmp sgt i64 %nargs, " + std::to_string(i));
            std::string Lp = newlabel(), Ld = newlabel(), Ln = newlabel();
            emit("br i1 " + have + ", label %" + Lp + ", label %" + Ld);
            C().terminated = true;
            start_block(Lp);
            {
                std::string pi = r(), ai = r(), si = r();
                emit(pi + " = getelementptr inbounds ptr, ptr %argv, i64 " +
                     std::to_string(i));
                emit(ai + " = load ptr, ptr " + pi + ", align 8");
                emit(si + " = getelementptr inbounds %kv, ptr %frame, i64 " +
                     std::to_string(i));
                emit("call void @kami_copy(ptr " + si + ", ptr " + ai + ")");
            }
            emit("br label %" + Ln);
            C().terminated = true;
            start_block(Ld);
            {
                int save = temp_top;
                int v = gen_expr(dflt);
                std::string si = slot_ptr((int)i);
                emit("call void @kami_copy(ptr " + si + ", ptr " + slot_ptr(v) + ")");
                temp_top = save;
            }
            start_block(Ln);
        }
        if (f->vararg)
            emit("call void @kami_pack_varargs(ptr " + slot_ptr((int)nparams) +
                 ", ptr %argv, i64 %nargs, i64 " + std::to_string(nparams) + ")");
    }

    // Replaces every @@PSLOTn@@ placeholder with the real frame index, now that
    // the temp window has its final size.
    void patch_pslots(std::string& ir) const {
        for (int k = 0; k < persistent_top; k++) {
            std::string needle = "@@PSLOT" + std::to_string(k) + "@@";
            std::string repl = std::to_string(temps_base + max_temps + k);
            size_t p = 0;
            while ((p = ir.find(needle, p)) != std::string::npos) {
                ir.replace(p, needle.size(), repl);
                p += repl.size();
            }
        }
    }

    // ---- whole function ----
    std::string finish(const std::string& fn_name, bool has_try) {
        (void)has_try;
        int total = temps_base + max_temps + persistent_top;
        if (total < 1) total = 1;
        std::string out;
        out += "define void @" + fn_name +
               "(ptr %ret, ptr %argv, i64 %nargs, ptr %captures) {\n";
        out += "entry:\n";
        out += "  %frame = alloca %kv, i64 " + std::to_string(total) + ", align 8\n";
        out += "  %argbuf = alloca ptr, i64 " + std::to_string(max_call_args) + ", align 8\n";
        out += "  call void @kami_frame_push(ptr %frame, i64 " + std::to_string(total) + ")\n";
        out += ctxs[0].body;
        if (!ctxs[0].terminated) {
            out += "  call void @kami_make_none(ptr %ret)\n";
            out += "  call void @kami_frame_pop()\n";
            out += "  ret void\n";
        }
        out += "}\n\n";
        // patch argbuf sizes in outlined bodies
        std::string extras = extra_fns;
        std::string needle = "@@ARGBUF@@";
        std::string repl = std::to_string(max_call_args);
        size_t p = 0;
        while ((p = extras.find(needle, p)) != std::string::npos) {
            extras.replace(p, needle.size(), repl);
            p += repl.size();
        }
        patch_pslots(extras);
        patch_pslots(out);
        return extras + out;
    }
};

const char* RUNTIME_DECLS = R"(declare void @kami_rt_init(i64, ptr)
declare void @kami_rt_shutdown()
declare void @kami_globals_init(i64)
declare void @kami_global_get(ptr, i64)
declare void @kami_global_set(i64, ptr)
declare void @kami_global_make_func(i64, ptr, i64, i64, ptr)
declare void @kami_global_make_class(i64, ptr, i64)
declare void @kami_class_add_method(i64, ptr, ptr, i64, i64)
declare void @kami_frame_push(ptr, i64)
declare void @kami_frame_pop()
declare void @kami_copy(ptr, ptr)
declare void @kami_make_none(ptr)
declare void @kami_make_bool(ptr, i64)
declare void @kami_make_int(ptr, i64)
declare void @kami_make_float(ptr, double)
declare void @kami_make_str(ptr, ptr, i64)
declare void @kami_make_list(ptr, ptr, i64)
declare void @kami_pack_varargs(ptr, ptr, i64, i64)
declare void @kami_make_map(ptr)
declare void @kami_make_builtin_func(ptr, i64, ptr)
declare void @kami_make_closure(ptr, ptr, i64, i64, ptr, ptr, i64)
declare void @kami_make_set(ptr)
declare void @kami_set_add(ptr, ptr)
declare i32 @kami_truthy(ptr)
declare void @kami_binop(i64, ptr, ptr, ptr)
declare void @kami_unop(i64, ptr, ptr)
declare void @kami_index_get(ptr, ptr, ptr)
declare void @kami_index_set(ptr, ptr, ptr)
declare void @kami_del_index(ptr, ptr)
declare void @kami_slice(ptr, ptr, ptr, ptr, ptr)
declare void @kami_attr_get(ptr, ptr, ptr)
declare void @kami_attr_set(ptr, ptr, ptr)
declare void @kami_call_value(ptr, ptr, ptr, i64)
declare void @kami_method(ptr, ptr, ptr, ptr, i64)
declare void @kami_builtin(i64, ptr, ptr, i64)
declare i32 @kami_range_cond(ptr, ptr, ptr)
declare void @kami_iter_prep(ptr, ptr)
declare i32 @kami_iter_cond(ptr, ptr)
declare void @kami_iter_get(ptr, ptr, ptr)
declare void @kami_unpack(ptr, ptr, i64, i64)
declare i64 @kami_try(ptr, ptr)
declare void @kami_last_error(ptr)
declare void @kami_raise(ptr)
declare void @kami_rethrow()
declare void @kami_run_module(ptr)
declare i64 @kami_c_arg_i64(ptr)
declare double @kami_c_arg_f64(ptr)
declare ptr @kami_c_arg_cstr(ptr)
declare void @kami_c_ret_cstr(ptr, ptr)

)";

} // namespace

std::string codegen(const Module& m, const std::string& source_name) {
    StrTable strtab;
    std::string fns;
    g_outline_counter = 0;

    auto gen_fn = [&](const Stmt* f) {
        FnGen g(m, strtab);
        g.nlocals = f->nlocals;
        bool ht = stmts_have_try(f->body);
        g.temps_base = f->nlocals + (ht ? 1 : 0);
        g.spill_slot = ht ? f->nlocals : -1;
        g.gen_prologue(f);
        g.gen_stmts(f->body);
        fns += g.finish("u_" + f->alias, ht);
    };
    for (const Stmt* f : m.functions) gen_fn(f);

    // module body
    FnGen g(m, strtab);
    g.is_module = true;
    bool mht = stmts_have_try(m.body);
    g.nlocals = 0;
    g.temps_base = mht ? 1 : 0;
    g.spill_slot = mht ? 0 : -1;
    g.gen_stmts(m.body);
    fns += g.finish("kamipy_module", mht);

    // main
    std::string main_fn;
    main_fn += "define i32 @main(i32 %argc, ptr %cargv) {\nentry:\n";
    main_fn += "  %argc64 = sext i32 %argc to i64\n";
    main_fn += "  call void @kami_rt_init(i64 %argc64, ptr %cargv)\n";
    main_fn += "  call void @kami_globals_init(i64 " + std::to_string(m.nglobals) + ")\n";
    for (const Stmt* f : m.functions) {
        if (f->global_idx < 0) continue; // methods have no global binding
        int si = strtab.intern(f->name);
        size_t fmin = f->params.size() - f->defaults.size() - (f->vararg ? 1 : 0);
        main_fn += "  call void @kami_global_make_func(i64 " + std::to_string(f->global_idx) +
                   ", ptr @u_" + f->alias + ", i64 " + std::to_string(fmin) + ", i64 " +
                   std::to_string(max_arity(f)) + ", ptr " + strtab.ref(si) + ")\n";
    }
    for (const Stmt* c : m.classes) {
        int si = strtab.intern(c->name);
        // find base class global idx
        int64_t parent = -1;
        if (!c->alias.empty()) {
            for (const Stmt* c2 : m.classes)
                if (c2->name == c->alias) parent = c2->global_idx;
        }
        main_fn += "  call void @kami_global_make_class(i64 " + std::to_string(c->global_idx) +
                   ", ptr " + strtab.ref(si) + ", i64 " + std::to_string(parent) + ")\n";
        for (auto& msp : c->body) {
            if (msp->kind != StmtKind::FuncDef) continue;
            const Stmt* mth = msp.get();
            int mi = strtab.intern(mth->name);
            size_t mmin = mth->params.size() - mth->defaults.size() - (mth->vararg ? 1 : 0);
            main_fn += "  call void @kami_class_add_method(i64 " +
                       std::to_string(c->global_idx) + ", ptr " + strtab.ref(mi) + ", ptr @u_" +
                       mth->alias + ", i64 " + std::to_string(mmin) + ", i64 " +
                       std::to_string(max_arity(mth)) + ")\n";
        }
    }
    main_fn += "  call void @kami_run_module(ptr @kamipy_module)\n";
    main_fn += "  call void @kami_rt_shutdown()\n";
    main_fn += "  ret i32 0\n}\n";

    std::string out;
    out += "; KamiPython compiled module: " + source_name + "\n";
    out += "%kv = type { i64, i64 }\n\n";
    for (size_t i = 0; i < strtab.strs.size(); i++) {
        const std::string& s = strtab.strs[i];
        out += "@.s" + std::to_string(i) + " = private unnamed_addr constant [" +
               std::to_string(s.size() + 1) + " x i8] c\"" + escape_ir_string(s) + "\"\n";
    }
    out += "\n";
    out += RUNTIME_DECLS;
    // C functions bound from C extension modules / ctypes / cffi. They are
    // ordinary external symbols: the linker resolves them against libc, libm,
    // ws2_32 or whatever -l flag the driver added for them.
    if (!m.natives.empty()) {
        out += "; --- C-ABI bindings ---\n";
        for (const NativeDecl& n : m.natives) {
            std::string params;
            for (size_t i = 1; i < n.csig.size(); i++)
                params += (i > 1 ? ", " : "") + std::string(FnGen::ir_type(n.csig[i]));
            out += "declare " + std::string(FnGen::ir_type(n.csig[0])) + " @" + n.symbol + "(" +
                   params + ")\n";
        }
        out += "\n";
    }
    out += fns;
    out += main_fn;
    return out;
}

} // namespace kami
