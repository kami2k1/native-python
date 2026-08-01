#include "codegen.h"

#include "../../runtime/include/kami_builtins.h"
#include "../../runtime/include/kami_runtime.h"

#include <cinttypes>
#include <cstdio>
#include <cstring>
#include <functional>
#include <map>

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

std::string fmt_double(double d) {
    uint64_t bits;
    memcpy(&bits, &d, 8);
    char buf[32];
    snprintf(buf, sizeof buf, "0x%016" PRIX64, bits);
    return buf;
}

struct StrTable {
    std::vector<std::string> strs;
    // String literals evaluated as VALUES get a pre-interned KamiStr* cache
    // (filled once at startup): loading it is 1 instruction instead of a
    // runtime call per evaluation. Keys are intern() ids.
    std::map<int, int> value_cache;
    int intern(const std::string& s) {
        for (size_t i = 0; i < strs.size(); i++)
            if (strs[i] == s) return (int)i;
        strs.push_back(s);
        return (int)strs.size() - 1;
    }
    int cache_for(int si) {
        auto it = value_cache.find(si);
        if (it != value_cache.end()) return it->second;
        int id = (int)value_cache.size();
        value_cache.emplace(si, id);
        return id;
    }
    std::string ref(int i) const { return "@.s" + std::to_string(i); }
    std::string cache_ref(int c) const { return "@.ic" + std::to_string(c); }
};

static bool stmts_have_try(const std::vector<StmtPtr>& body);
static bool stmt_has_try(const Stmt* s) {
    // `with` bodies are outlined exactly like try bodies: a `return` inside
    // either propagates through the spill slot, so both must reserve it.
    if (s->kind == StmtKind::Try || s->kind == StmtKind::With) return true;
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

// ---- function signature metadata (for dynamic *args/**kwargs handling) ----
static std::string join_params(const Stmt* f) {
    std::string s;
    for (size_t i = 0; i < f->params.size(); i++) s += (i ? "," : "") + f->params[i];
    return s;
}
static int64_t func_flags(const Stmt* f) {
    return (f->vararg.empty() ? 0 : 1) | (f->kwarg.empty() ? 0 : 2) |
           (f->is_generator ? 4 : 0); // KFN_GENERATOR
}
static int64_t func_kwonly(const Stmt* f) {
    return f->kwonly >= 0 ? f->kwonly : (int64_t)f->params.size();
}

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

    std::string truthy(int slot, uint8_t sty = TY_ANY) {
        if (sty == TY_INT || sty == TY_BOOL) {
            std::string v = load_i64(slot), b = r();
            emit(b + " = icmp ne i64 " + v + ", 0");
            return b;
        }
        if (sty == TY_FLOAT) {
            std::string v = load_f64(slot), b = r();
            emit(b + " = fcmp une double " + v + ", 0.0");
            return b;
        }
        std::string sp = slot_ptr(slot);
        std::string t = r();
        emit(t + " = call i32 @kami_truthy(ptr " + sp + ")");
        std::string b = r();
        emit(b + " = icmp ne i32 " + t + ", 0");
        return b;
    }

    // ---- unboxed primitive access (type-inference driven) -----------------
    // A %kv slot is { i64 tag, i64 payload }. For values whose static type is
    // a primitive (int/bool/float/none) the payload can be read and written
    // directly — no runtime call, no lock, no boxing.
    static bool prim(uint8_t t) {
        return t == TY_INT || t == TY_FLOAT || t == TY_BOOL || t == TY_NONE;
    }
    static bool intlike(uint8_t t) { return t == TY_INT || t == TY_BOOL; }
    static bool numeric(uint8_t t) { return intlike(t) || t == TY_FLOAT; }

    std::string payload_ptr(int slot) {
        std::string p = r();
        emit(p + " = getelementptr inbounds %kv, ptr %frame, i64 " + std::to_string(slot) +
             ", i32 1");
        return p;
    }
    std::string load_i64(int slot) {
        std::string v = r();
        emit(v + " = load i64, ptr " + payload_ptr(slot) + ", align 8");
        return v;
    }
    std::string load_f64(int slot) {
        std::string v = r();
        emit(v + " = load double, ptr " + payload_ptr(slot) + ", align 8");
        return v;
    }
    // Load a numeric slot as double, converting statically-int values.
    std::string load_num_as_f64(int slot, uint8_t sty) {
        if (sty == TY_FLOAT) return load_f64(slot);
        std::string i = load_i64(slot), v = r();
        emit(v + " = sitofp i64 " + i + " to double");
        return v;
    }
    void store_tag(int slot, int64_t tag) {
        emit("store i64 " + std::to_string(tag) + ", ptr " + slot_ptr(slot) + ", align 8");
    }
    int store_int(const std::string& v) {
        int t = alloc_temp();
        store_tag(t, 2 /*KT_INT*/);
        emit("store i64 " + v + ", ptr " + payload_ptr(t) + ", align 8");
        return t;
    }
    int store_float(const std::string& v) {
        int t = alloc_temp();
        store_tag(t, 3 /*KT_FLOAT*/);
        emit("store double " + v + ", ptr " + payload_ptr(t) + ", align 8");
        return t;
    }
    int store_bool_i1(const std::string& v) {
        int t = alloc_temp();
        store_tag(t, 1 /*KT_BOOL*/);
        std::string z = r();
        emit(z + " = zext i1 " + v + " to i64");
        emit("store i64 " + z + ", ptr " + payload_ptr(t) + ", align 8");
        return t;
    }
    // 16-byte slot copy for statically-primitive values (no GC pointers).
    void copy_prim(int dst, int src) {
        std::string tv = r();
        emit(tv + " = load i64, ptr " + slot_ptr(src) + ", align 8");
        std::string pv = load_i64(src);
        emit("store i64 " + tv + ", ptr " + slot_ptr(dst) + ", align 8");
        emit("store i64 " + pv + ", ptr " + payload_ptr(dst) + ", align 8");
    }
    void panic_block(const std::string& msg, const std::string& cond) {
        std::string Lp = newlabel(), Lok = newlabel();
        emit("br i1 " + cond + ", label %" + Lp + ", label %" + Lok);
        C().terminated = true;
        start_block(Lp);
        emit("call void @kami_panic(ptr " + str_const(msg) + ")");
        emit("unreachable");
        C().terminated = true;
        start_block(Lok);
    }

    // Unboxed int // and % with Python floor/sign semantics and the same
    // error messages as the runtime. Handles b == 0 (panic) and b == -1
    // (separate path: sdiv INT_MIN,-1 traps on x86). When the divisor is a
    // positive constant and the dividend is provably non-negative
    // (typeinf's value-range bit), floored == truncated and the emission
    // collapses to a single sdiv/srem — exactly what Go emits for % and /.
    int gen_int_divmod(const Expr* e, const std::string& a, const std::string& b) {
        int op = (int)e->op;
        bool bconst = e->b->kind == ExprKind::IntLit;
        int64_t bc = bconst ? e->b->ival : 0;
        if (bconst && bc != 0 && bc != -1) {
            std::string q = r(), rm = r();
            if (op == KOP_FLOORDIV) {
                emit(q + " = sdiv i64 " + a + ", " + b);
                if (e->a->nonneg && bc > 0) return store_int(q);
                emit(rm + " = srem i64 " + a + ", " + b);
                std::string nz = r(), x = r(), sd = r(), adj = r(), adj64 = r(), v = r();
                emit(nz + " = icmp ne i64 " + rm + ", 0");
                emit(x + " = xor i64 " + a + ", " + b);
                emit(sd + " = icmp slt i64 " + x + ", 0");
                emit(adj + " = and i1 " + nz + ", " + sd);
                emit(adj64 + " = zext i1 " + adj + " to i64");
                emit(v + " = sub i64 " + q + ", " + adj64);
                return store_int(v);
            }
            emit(rm + " = srem i64 " + a + ", " + b);
            if (e->a->nonneg && bc > 0) return store_int(rm);
            std::string nz = r(), x = r(), sd = r(), adj = r(), addv = r(), v = r();
            emit(nz + " = icmp ne i64 " + rm + ", 0");
            emit(x + " = xor i64 " + rm + ", " + b);
            emit(sd + " = icmp slt i64 " + x + ", 0");
            emit(adj + " = and i1 " + nz + ", " + sd);
            emit(addv + " = select i1 " + adj + ", i64 " + b + ", i64 0");
            emit(v + " = add i64 " + rm + ", " + addv);
            return store_int(v);
        }
        std::string z = r();
        emit(z + " = icmp eq i64 " + b + ", 0");
        panic_block(op == KOP_FLOORDIV ? "integer division by zero"
                                       : "integer modulo by zero",
                    z);
        std::string isneg1 = r();
        emit(isneg1 + " = icmp eq i64 " + b + ", -1");
        std::string Lneg = newlabel(), Lgen = newlabel(), Lend = newlabel();
        emit("br i1 " + isneg1 + ", label %" + Lneg + ", label %" + Lgen);
        C().terminated = true;
        start_block(Lneg);
        std::string vneg = r();
        if (op == KOP_FLOORDIV) emit(vneg + " = sub i64 0, " + a);
        else emit(vneg + " = add i64 0, 0");
        emit("br label %" + Lend);
        C().terminated = true;
        start_block(Lgen);
        std::string q = r(), rm = r();
        emit(q + " = sdiv i64 " + a + ", " + b);
        emit(rm + " = srem i64 " + a + ", " + b);
        std::string vgen;
        if (op == KOP_FLOORDIV) {
            std::string nz = r(), x = r(), sd = r(), adj = r(), adj64 = r();
            emit(nz + " = icmp ne i64 " + rm + ", 0");
            emit(x + " = xor i64 " + a + ", " + b);
            emit(sd + " = icmp slt i64 " + x + ", 0");
            emit(adj + " = and i1 " + nz + ", " + sd);
            emit(adj64 + " = zext i1 " + adj + " to i64");
            vgen = r();
            emit(vgen + " = sub i64 " + q + ", " + adj64);
        } else {
            std::string nz = r(), x = r(), sd = r(), adj = r(), addv = r();
            emit(nz + " = icmp ne i64 " + rm + ", 0");
            emit(x + " = xor i64 " + rm + ", " + b);
            emit(sd + " = icmp slt i64 " + x + ", 0");
            emit(adj + " = and i1 " + nz + ", " + sd);
            emit(addv + " = select i1 " + adj + ", i64 " + b + ", i64 0");
            vgen = r();
            emit(vgen + " = add i64 " + rm + ", " + addv);
        }
        emit("br label %" + Lend);
        C().terminated = true;
        start_block(Lend);
        std::string v = r();
        emit(v + " = phi i64 [ " + vneg + ", %" + Lneg + " ], [ " + vgen + ", %" + Lgen +
             " ]");
        return store_int(v);
    }

    // Attempts an unboxed emission of a binary op; returns -1 when the static
    // types don't allow it.
    int gen_binary_unboxed(const Expr* e, int a, int b) {
        uint8_t ta = e->a->sty, tb = e->b->sty;
        bool cmp = e->op == KOP_EQ || e->op == KOP_NE || e->op == KOP_LT ||
                   e->op == KOP_GT || e->op == KOP_LE || e->op == KOP_GE;
        if (intlike(ta) && intlike(tb)) {
            const char* iop = nullptr;
            switch (e->op) {
            case KOP_ADD: iop = "add"; break;
            case KOP_SUB: iop = "sub"; break;
            case KOP_MUL: iop = "mul"; break;
            case KOP_BITAND: iop = "and"; break;
            case KOP_BITOR: iop = "or"; break;
            case KOP_BITXOR: iop = "xor"; break;
            default: break;
            }
            if (iop) {
                std::string va = load_i64(a), vb = load_i64(b), v = r();
                emit(v + " = " + iop + " i64 " + va + ", " + vb);
                return store_int(v);
            }
            if (cmp) {
                const char* cc = e->op == KOP_EQ   ? "eq"
                                 : e->op == KOP_NE ? "ne"
                                 : e->op == KOP_LT ? "slt"
                                 : e->op == KOP_GT ? "sgt"
                                 : e->op == KOP_LE ? "sle"
                                                   : "sge";
                std::string va = load_i64(a), vb = load_i64(b), v = r();
                emit(v + " = icmp " + std::string(cc) + " i64 " + va + ", " + vb);
                return store_bool_i1(v);
            }
            if (e->op == KOP_FLOORDIV || e->op == KOP_MOD) {
                std::string va = load_i64(a), vb = load_i64(b);
                return gen_int_divmod(e, va, vb);
            }
            if (e->op == KOP_DIV) {
                std::string vb = load_i64(b), z = r();
                emit(z + " = icmp eq i64 " + vb + ", 0");
                panic_block("division by zero", z);
                std::string va = load_i64(a), fa = r(), fb = r(), v = r();
                emit(fa + " = sitofp i64 " + va + " to double");
                emit(fb + " = sitofp i64 " + vb + " to double");
                emit(v + " = fdiv double " + fa + ", " + fb);
                return store_float(v);
            }
            return -1;
        }
        if (numeric(ta) && numeric(tb)) {
            const char* fop = nullptr;
            switch (e->op) {
            case KOP_ADD: fop = "fadd"; break;
            case KOP_SUB: fop = "fsub"; break;
            case KOP_MUL: fop = "fmul"; break;
            default: break;
            }
            if (fop) {
                std::string va = load_num_as_f64(a, ta), vb = load_num_as_f64(b, tb), v = r();
                emit(v + " = " + std::string(fop) + " double " + va + ", " + vb);
                return store_float(v);
            }
            if (e->op == KOP_DIV) {
                std::string vb = load_num_as_f64(b, tb), z = r();
                emit(z + " = fcmp oeq double " + vb + ", 0.0");
                panic_block("division by zero", z);
                std::string va = load_num_as_f64(a, ta), v = r();
                emit(v + " = fdiv double " + va + ", " + vb);
                return store_float(v);
            }
            if (cmp) {
                // ordered compares: NaN comparisons are False (except !=)
                const char* cc = e->op == KOP_EQ   ? "oeq"
                                 : e->op == KOP_NE ? "une"
                                 : e->op == KOP_LT ? "olt"
                                 : e->op == KOP_GT ? "ogt"
                                 : e->op == KOP_LE ? "ole"
                                                   : "oge";
                std::string va = load_num_as_f64(a, ta), vb = load_num_as_f64(b, tb), v = r();
                emit(v + " = fcmp " + std::string(cc) + " double " + va + ", " + vb);
                return store_bool_i1(v);
            }
            return -1;
        }
        return -1;
    }

    void assign_var(Res res, int64_t idx, int src_slot, uint8_t sty = TY_ANY) {
        if (res == Res::Local) {
            if (prim(sty)) { // no GC pointer involved: plain 16-byte copy
                copy_prim((int)idx, src_slot);
                return;
            }
            std::string sp = slot_ptr(src_slot);
            std::string dp = slot_ptr((int)idx);
            emit("call void @kami_copy(ptr " + dp + ", ptr " + sp + ")");
        } else {
            std::string sp = slot_ptr(src_slot);
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
            store_tag(t, 2);
            emit("store i64 " + std::to_string(e->ival) + ", ptr " + payload_ptr(t) +
                 ", align 8");
            return t;
        }
        case ExprKind::FloatLit: {
            int t = alloc_temp();
            store_tag(t, 3);
            emit("store double " + fmt_double(e->fval) + ", ptr " + payload_ptr(t) +
                 ", align 8");
            return t;
        }
        case ExprKind::BoolLit: {
            int t = alloc_temp();
            store_tag(t, 1);
            emit("store i64 " + std::to_string(e->ival) + ", ptr " + payload_ptr(t) +
                 ", align 8");
            return t;
        }
        case ExprKind::NoneLit: {
            int t = alloc_temp();
            store_tag(t, 0);
            emit("store i64 0, ptr " + payload_ptr(t) + ", align 8");
            return t;
        }
        case ExprKind::StrLit: {
            // pre-interned at startup: no allocation, no runtime call
            int t = alloc_temp();
            int c = strtab.cache_for(strtab.intern(e->sval));
            std::string p = r();
            emit(p + " = load ptr, ptr " + strtab.cache_ref(c) + ", align 8");
            store_tag(t, 4 /*KT_STR*/);
            emit("store ptr " + p + ", ptr " + payload_ptr(t) + ", align 8");
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
            temp_top = save;
            // unboxed fast path: both operands statically primitive numbers
            int u = gen_binary_unboxed(e, a, b);
            if (u >= 0) return u;
            std::string ap = slot_ptr(a), bp = slot_ptr(b);
            int t = alloc_temp();
            emit("call void @kami_binop(i64 " + std::to_string(e->op) + ", ptr " + slot_ptr(t) +
                 ", ptr " + ap + ", ptr " + bp + ")");
            return t;
        }
        case ExprKind::Unary: {
            int save = temp_top;
            int a = gen_expr(e->a.get());
            temp_top = save;
            if (e->op == KUOP_NEG && intlike(e->a->sty)) {
                std::string va = load_i64(a), v = r();
                emit(v + " = sub i64 0, " + va);
                return store_int(v);
            }
            if (e->op == KUOP_NEG && e->a->sty == TY_FLOAT) {
                std::string va = load_f64(a), v = r();
                emit(v + " = fneg double " + va);
                return store_float(v);
            }
            if (e->op == KUOP_INV && intlike(e->a->sty)) {
                std::string va = load_i64(a), v = r();
                emit(v + " = xor i64 " + va + ", -1");
                return store_int(v);
            }
            if (e->op == KUOP_NOT && numeric(e->a->sty)) {
                std::string v = r();
                if (e->a->sty == TY_FLOAT) {
                    std::string va = load_f64(a);
                    emit(v + " = fcmp oeq double " + va + ", 0.0");
                } else {
                    std::string va = load_i64(a);
                    emit(v + " = icmp eq i64 " + va + ", 0");
                }
                return store_bool_i1(v);
            }
            std::string ap = slot_ptr(a);
            int t = alloc_temp();
            emit("call void @kami_unop(i64 " + std::to_string(e->op) + ", ptr " + slot_ptr(t) +
                 ", ptr " + ap + ")");
            return t;
        }
        case ExprKind::BoolOp: {
            int t = alloc_temp();
            int save = temp_top;
            int a = gen_expr(e->a.get());
            if (prim(e->a->sty))
                copy_prim(t, a);
            else
                emit("call void @kami_copy(ptr " + slot_ptr(t) + ", ptr " + slot_ptr(a) + ")");
            temp_top = save;
            std::string b = truthy(t, e->a->sty);
            std::string Leval = newlabel(), Lend = newlabel();
            if (e->op == 0)
                emit("br i1 " + b + ", label %" + Leval + ", label %" + Lend);
            else
                emit("br i1 " + b + ", label %" + Lend + ", label %" + Leval);
            C().terminated = true;
            start_block(Leval);
            int save2 = temp_top;
            int bslot = gen_expr(e->b.get());
            if (prim(e->b->sty))
                copy_prim(t, bslot);
            else
                emit("call void @kami_copy(ptr " + slot_ptr(t) + ", ptr " + slot_ptr(bslot) +
                     ")");
            temp_top = save2;
            start_block(Lend);
            return t;
        }
        case ExprKind::IfExp: {
            int t = alloc_temp();
            int save = temp_top;
            int c = gen_expr(e->b.get());
            std::string b = truthy(c, e->b->sty);
            temp_top = save;
            std::string Lthen = newlabel(), Lelse = newlabel(), Lend = newlabel();
            emit("br i1 " + b + ", label %" + Lthen + ", label %" + Lelse);
            C().terminated = true;
            start_block(Lthen);
            {
                int s2 = temp_top;
                int v = gen_expr(e->a.get());
                if (prim(e->a->sty))
                    copy_prim(t, v);
                else
                    emit("call void @kami_copy(ptr " + slot_ptr(t) + ", ptr " + slot_ptr(v) +
                         ")");
                temp_top = s2;
            }
            emit("br label %" + Lend);
            C().terminated = true;
            start_block(Lelse);
            {
                int s2 = temp_top;
                int v = gen_expr(e->c.get());
                if (prim(e->c->sty))
                    copy_prim(t, v);
                else
                    emit("call void @kami_copy(ptr " + slot_ptr(t) + ", ptr " + slot_ptr(v) +
                         ")");
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
                if (!p.first) { // {**other}: merge at runtime
                    int v = gen_expr(p.second.get());
                    emit("call void @kami_map_merge(ptr " + slot_ptr(t) + ", ptr " +
                         slot_ptr(v) + ")");
                    temp_top = save;
                    continue;
                }
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
                // Monomorphized fast path: unbox the arguments straight into
                // CPU registers and call the @n_ specialization.
                size_t fidx = (size_t)callee->res_idx;
                if (fidx < mod.ftypes.size() && eff_native(mod.ftypes[fidx])) {
                    const Stmt* f = mod.functions[fidx];
                    const FuncTypeInfo& fi = mod.ftypes[fidx];
                    const std::vector<uint8_t>& ps = eff_params(fi);
                    uint8_t rt = eff_ret(fi);
                    bool match = e->args.size() == f->params.size();
                    for (size_t i = 0; match && i < e->args.size(); i++) {
                        uint8_t pt = ps[i], at = e->args[i]->sty;
                        if (pt == TY_FLOAT ? at != TY_FLOAT : !intlike(at)) match = false;
                    }
                    if (match) {
                        std::string argstr;
                        for (size_t i = 0; i < e->args.size(); i++) {
                            int sl = slots[i];
                            if (i) argstr += ", ";
                            if (ps[i] == TY_FLOAT)
                                argstr += "double " + load_f64(sl);
                            else
                                argstr += "i64 " + load_i64(sl);
                        }
                        temp_top = save;
                        if (rt == TY_NONE) {
                            emit("call void @n_" + f->alias + "(" + argstr + ")");
                            store_tag(t, 0);
                            emit("store i64 0, ptr " + payload_ptr(t) + ", align 8");
                        } else if (rt == TY_FLOAT) {
                            std::string v = r();
                            emit(v + " = call double @n_" + f->alias + "(" + argstr + ")");
                            store_tag(t, 3);
                            emit("store double " + v + ", ptr " + payload_ptr(t) +
                                 ", align 8");
                        } else {
                            std::string v = r();
                            emit(v + " = call i64 @n_" + f->alias + "(" + argstr + ")");
                            store_tag(t, rt == TY_BOOL ? 1 : 2);
                            emit("store i64 " + v + ", ptr " + payload_ptr(t) + ", align 8");
                        }
                        return t;
                    }
                }
                fill_argbuf(slots);
                const Stmt* f = mod.functions[(size_t)callee->res_idx];
                if (f->is_generator) {
                    // Calling a generator function builds the generator object;
                    // the body (@u_...) runs lazily inside the fiber.
                    emit("call void @kami_gen_create(ptr " + slot_ptr(t) + ", ptr @u_" +
                         f->alias + ", ptr %argbuf, i64 " + std::to_string(slots.size()) +
                         ")");
                    temp_top = save;
                    return t;
                }
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
        case ExprKind::CallStar: {
            // f(*a, **k): build the positional list and keyword dict, then let
            // the runtime match them against the callee's signature.
            int t = alloc_temp();
            int save = temp_top;
            int c = gen_expr(e->a.get());
            int pos = alloc_temp();
            emit("call void @kami_make_list(ptr " + slot_ptr(pos) + ", ptr %argbuf, i64 0)");
            for (auto& a : e->args) {
                int s2 = temp_top;
                bool star = a->kind == ExprKind::Starred;
                int v = gen_expr(star ? a->a.get() : a.get());
                std::vector<int> one{v};
                fill_argbuf(one);
                int dummy = alloc_temp();
                emit("call void @kami_method(ptr " + slot_ptr(dummy) + ", ptr " +
                     slot_ptr(pos) + ", ptr " + str_const(star ? "extend" : "append") +
                     ", ptr %argbuf, i64 1)");
                temp_top = s2;
            }
            int kw = alloc_temp();
            emit("call void @kami_make_map(ptr " + slot_ptr(kw) + ")");
            for (auto& p : e->pairs) {
                int s2 = temp_top;
                if (p.first) { // name=value
                    int k = gen_expr(p.first.get());
                    int v = gen_expr(p.second.get());
                    emit("call void @kami_index_set(ptr " + slot_ptr(kw) + ", ptr " +
                         slot_ptr(k) + ", ptr " + slot_ptr(v) + ")");
                } else { // **mapping
                    int v = gen_expr(p.second.get());
                    emit("call void @kami_map_merge(ptr " + slot_ptr(kw) + ", ptr " +
                         slot_ptr(v) + ")");
                }
                temp_top = s2;
            }
            if (e->sval.empty()) {
                emit("call void @kami_call_star(ptr " + slot_ptr(t) + ", ptr " +
                     slot_ptr(c) + ", ptr " + slot_ptr(pos) + ", ptr " + slot_ptr(kw) +
                     ")");
            } else { // obj.m(*a, **k)
                emit("call void @kami_method_star(ptr " + slot_ptr(t) + ", ptr " +
                     slot_ptr(c) + ", ptr " + str_const(e->sval) + ", ptr " +
                     slot_ptr(pos) + ", ptr " + slot_ptr(kw) + ")");
            }
            temp_top = save;
            return t;
        }
        case ExprKind::CCall: return gen_ccall(e);
        case ExprKind::Yield: {
            if (e->op == 1) { // yield from: drive the sub-iterable to exhaustion
                gen_iteration(e->a.get(), [&](int elem) {
                    int s2 = temp_top;
                    int sent = alloc_temp();
                    emit("call void @kami_gen_yield(ptr " + slot_ptr(sent) + ", ptr " +
                         slot_ptr(elem) + ")");
                    temp_top = s2;
                }, nullptr, nullptr);
                int t = alloc_temp();
                emit("call void @kami_make_none(ptr " + slot_ptr(t) + ")");
                return t;
            }
            int t = alloc_temp(); // receives the value passed to send()
            int save = temp_top;
            int v;
            if (e->a) {
                v = gen_expr(e->a.get());
            } else {
                v = alloc_temp();
                emit("call void @kami_make_none(ptr " + slot_ptr(v) + ")");
            }
            emit("call void @kami_gen_yield(ptr " + slot_ptr(t) + ", ptr " + slot_ptr(v) +
                 ")");
            temp_top = save;
            return t;
        }
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
        size_t fmin = fn->params.size() - fn->defaults.size();
        emit("call void @kami_make_closure(ptr " + slot_ptr(result) + ", ptr @u_" + fn->alias +
             ", i64 " + std::to_string(fmin) + ", i64 " + std::to_string(fn->params.size()) +
             ", ptr " + str_const(fn->name) + ", ptr %argbuf, i64 " +
             std::to_string(caps.size()) + ", i64 " + std::to_string(func_kwonly(fn)) +
             ", i64 " + std::to_string(func_flags(fn)) + ", ptr " +
             str_const(join_params(fn)) + ")");
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
                    std::string b = truthy(c, cptr->sty);
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
        // Range loops always run unboxed: bounds whose static type is unknown
        // get a one-time tag check before the loop (range() would reject
        // non-integers at runtime anyway), then the induction variable lives
        // as a raw i64 — no runtime calls per iteration.
        if (is_range) {
            const auto& args = iter->args;
            int s_i = alloc_temp();
            std::string vstart, vstop, vstep;
            {
                int save = temp_top;
                auto checked_load = [&](const Expr* a) {
                    int slot = gen_expr(a);
                    if (!intlike(a->sty)) { // dynamic: verify the tag once
                        std::string tag = r();
                        emit(tag + " = load i64, ptr " + slot_ptr(slot) + ", align 8");
                        std::string t1 = r(), t2 = r(), ok = r(), bad = r();
                        emit(t1 + " = icmp eq i64 " + tag + ", 2");
                        emit(t2 + " = icmp eq i64 " + tag + ", 1");
                        emit(ok + " = or i1 " + t1 + ", " + t2);
                        emit(bad + " = xor i1 " + ok + ", true");
                        panic_block("range() arguments must be integers", bad);
                    }
                    return load_i64(slot);
                };
                if (args.size() == 1) {
                    vstart = "0";
                    vstop = checked_load(args[0].get());
                } else {
                    vstart = checked_load(args[0].get());
                    vstop = checked_load(args[1].get());
                }
                if (args.size() == 3) {
                    vstep = checked_load(args[2].get());
                    std::string z = r();
                    emit(z + " = icmp eq i64 " + vstep + ", 0");
                    panic_block("range() step must not be zero", z);
                } else {
                    vstep = "1";
                }
                temp_top = save;
            }
            const Expr* step_e = args.size() == 3 ? args[2].get() : nullptr;
            bool step_const = !step_e || step_e->kind == ExprKind::IntLit;
            int64_t step_sign = step_e && step_e->kind == ExprKind::IntLit
                                    ? (step_e->ival < 0 ? -1 : 1)
                                    : 1;
            store_tag(s_i, 2); // the induction slot stays an INT box
            emit("store i64 " + vstart + ", ptr " + payload_ptr(s_i) + ", align 8");
            std::string Lcond = newlabel(), Lbody = newlabel(), Lstep = newlabel(),
                        Lelse = newlabel(), Lend = newlabel();
            if (out_step) *out_step = Lstep;
            if (out_end) *out_end = Lend;
            start_block(Lcond);
            std::string vi = load_i64(s_i);
            std::string b = r();
            if (step_const) {
                emit(b + " = icmp " + (step_sign > 0 ? std::string("slt") : std::string("sgt")) +
                     " i64 " + vi + ", " + vstop);
            } else {
                std::string pos = r(), c1 = r(), c2 = r();
                emit(pos + " = icmp sgt i64 " + vstep + ", 0");
                emit(c1 + " = icmp slt i64 " + vi + ", " + vstop);
                emit(c2 + " = icmp sgt i64 " + vi + ", " + vstop);
                emit(b + " = select i1 " + pos + ", i1 " + c1 + ", i1 " + c2);
            }
            emit("br i1 " + b + ", label %" + Lbody + ", label %" + Lelse);
            C().terminated = true;
            start_block(Lbody);
            body(s_i);
            start_block(Lstep);
            std::string vi2 = load_i64(s_i), vnext = r();
            emit(vnext + " = add i64 " + vi2 + ", " + vstep);
            emit("store i64 " + vnext + ", ptr " + payload_ptr(s_i) + ", align 8");
            emit("br label %" + Lcond);
            C().terminated = true;
            start_block(Lelse);
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
            if (s->str_iadd) {
                // `s = s + x` with no live aliases: append into s's buffer in
                // place (amortized O(1)); falls back to `+` for non-strings.
                int rhs = gen_expr(s->e1->b.get());
                emit("call void @kami_str_iadd(ptr " + slot_ptr((int)s->target_idx) +
                     ", ptr " + slot_ptr(rhs) + ")");
                break;
            }
            int v = gen_expr(s->e1.get());
            if (s->free_hint) // old container is provably dead: recycle it
                emit("call void @kami_free_hint(ptr " + slot_ptr((int)s->target_idx) +
                     ")");
            assign_var(s->target_res, s->target_idx, v, s->e1->sty);
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
            std::string b = truthy(c, s->e1->sty);
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
            std::string b = truthy(c, s->e1->sty);
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
            // range loops always produce raw-int elements (see gen_iteration)
            bool int_iter = s->e1->kind == ExprKind::Call &&
                            s->e1->a->res == Res::BuiltinFunc &&
                            s->e1->a->res_idx == KB_RANGE;
            auto body = [&](int elem) {
                if (s->params.size() == 1) {
                    assign_var(s->target_res, s->target_idx, elem,
                               int_iter ? (uint8_t)TY_INT : (uint8_t)TY_ANY);
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
        case StmtKind::Import:
            if (s->pyext) { // bridged CPython extension module
                int t = alloc_temp();
                emit("call void @kami_pyext_import(ptr " + slot_ptr(t) + ", ptr " +
                     str_const(s->name) + ")");
                emit("call void @kami_global_set(i64 " + std::to_string(s->global_idx) +
                     ", ptr " + slot_ptr(t) + ")");
            }
            break;
        case StmtKind::FromImport:
            if (s->pyext) { // from _hashlib import openssl_sha256, ...
                int t = alloc_temp();
                emit("call void @kami_pyext_import(ptr " + slot_ptr(t) + ", ptr " +
                     str_const(s->name) + ")");
                for (size_t i = 0; i < s->import_names.size(); i++) {
                    int u = alloc_temp();
                    emit("call void @kami_pyext_getattr(ptr " + slot_ptr(u) + ", ptr " +
                         slot_ptr(t) + ", ptr " + str_const(s->import_names[i].first) +
                         ")");
                    emit("call void @kami_global_set(i64 " +
                         std::to_string(s->multi_tidx[i]) + ", ptr " + slot_ptr(u) + ")");
                }
            }
            break;
        case StmtKind::Pass:
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
                         "(ptr %frame, ptr %captures) {\nentry:\n  %argbuf = alloca ptr, "
                         "i64 @@ARGBUF@@, align 8\n" +
                         done.body + "}\n\n";
        }
        std::string code = r();
        emit(code + " = call i64 @kami_try(ptr @" + fname + ", ptr %frame, ptr %captures)");
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
                         "(ptr %frame, ptr %captures) {\n" +
                         "entry:\n  %argbuf = alloca ptr, i64 @@ARGBUF@@, align 8\n" +
                         done.body + "}\n\n";
        }
        // 2) protected call
        std::string code = r();
        emit(code + " = call i64 @kami_try(ptr @" + fname + ", ptr %frame, ptr %captures)");
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

    // Runtime type guard for speculatively-monomorphized functions: when the
    // dynamic arguments match the assumed signature, dispatch straight to the
    // @n_ specialization; otherwise fall through into the generic boxed body.
    void gen_guard(const Stmt* f, const FuncTypeInfo& fi) {
        size_t P = f->params.size();
        std::string Lboxed = newlabel();
        std::string nok = r();
        emit(nok + " = icmp eq i64 %nargs, " + std::to_string(P));
        std::string Lc = newlabel();
        emit("br i1 " + nok + ", label %" + Lc + ", label %" + Lboxed);
        C().terminated = true;
        start_block(Lc);
        // load each argument pointer and check its tag
        std::vector<std::string> argp(P);
        for (size_t i = 0; i < P; i++) {
            std::string pi = r();
            emit(pi + " = getelementptr inbounds ptr, ptr %argv, i64 " + std::to_string(i));
            argp[i] = r();
            emit(argp[i] + " = load ptr, ptr " + pi + ", align 8");
            std::string tag = r();
            emit(tag + " = load i64, ptr " + argp[i] + ", align 8");
            std::string okv = r();
            if (fi.spec_params[i] == TY_FLOAT) {
                emit(okv + " = icmp eq i64 " + tag + ", 3");
            } else { // int parameter: accept INT and BOOL tags
                std::string t1 = r(), t2 = r();
                emit(t1 + " = icmp eq i64 " + tag + ", 2");
                emit(t2 + " = icmp eq i64 " + tag + ", 1");
                emit(okv + " = or i1 " + t1 + ", " + t2);
            }
            std::string Ln = newlabel();
            emit("br i1 " + okv + ", label %" + Ln + ", label %" + Lboxed);
            C().terminated = true;
            start_block(Ln);
        }
        // all tags match: unbox, call the specialization, box the result
        std::string argstr;
        for (size_t i = 0; i < P; i++) {
            std::string pp = r(), v = r();
            emit(pp + " = getelementptr inbounds i8, ptr " + argp[i] + ", i64 8");
            if (fi.spec_params[i] == TY_FLOAT) {
                emit(v + " = load double, ptr " + pp + ", align 8");
                argstr += (i ? ", " : "") + std::string("double ") + v;
            } else {
                emit(v + " = load i64, ptr " + pp + ", align 8");
                argstr += (i ? ", " : "") + std::string("i64 ") + v;
            }
        }
        std::string retpp = r();
        emit(retpp + " = getelementptr inbounds i8, ptr %ret, i64 8");
        if (fi.spec_ret == TY_NONE) {
            emit("call void @n_" + f->alias + "(" + argstr + ")");
            emit("store i64 0, ptr %ret, align 8");
            emit("store i64 0, ptr " + retpp + ", align 8");
        } else if (fi.spec_ret == TY_FLOAT) {
            std::string v = r();
            emit(v + " = call double @n_" + f->alias + "(" + argstr + ")");
            emit("store i64 3, ptr %ret, align 8");
            emit("store double " + v + ", ptr " + retpp + ", align 8");
        } else {
            std::string v = r();
            emit(v + " = call i64 @n_" + f->alias + "(" + argstr + ")");
            emit("store i64 " + std::string(fi.spec_ret == TY_BOOL ? "1" : "2") +
                 ", ptr %ret, align 8");
            emit("store i64 " + v + ", ptr " + retpp + ", align 8");
        }
        emit("call void @kami_frame_pop()");
        emit("ret void");
        C().terminated = true;
        start_block(Lboxed);
    }

    // Emits parameter copying (with default-value filling) into the current
    // body context. Must be called before generating any statements.
    void gen_prologue(const Stmt* f) {
        size_t nparams = f->params.size();
        size_t ndefaults = f->defaults.size();
        // Branch to `Labsent` when %argv[i] is absent (i >= %nargs) or is the
        // KT_MISSING marker a dynamic call passes for "fill the default";
        // falls through into a fresh block when the argument is real.
        auto arg_or = [&](size_t i, const std::string& Labsent) {
            std::string have = r();
            emit(have + " = icmp sgt i64 %nargs, " + std::to_string(i));
            std::string Lchk = newlabel(), Lp = newlabel();
            emit("br i1 " + have + ", label %" + Lchk + ", label %" + Labsent);
            C().terminated = true;
            start_block(Lchk);
            std::string pi = r(), ai = r(), tag = r(), miss = r();
            emit(pi + " = getelementptr inbounds ptr, ptr %argv, i64 " + std::to_string(i));
            emit(ai + " = load ptr, ptr " + pi + ", align 8");
            emit(tag + " = load i64, ptr " + ai + ", align 8");
            emit(miss + " = icmp eq i64 " + tag + ", 98"); // KT_MISSING
            emit("br i1 " + miss + ", label %" + Labsent + ", label %" + Lp);
            C().terminated = true;
            start_block(Lp);
            return ai; // pointer to the caller's argument value
        };
        for (size_t i = 0; i < nparams; i++) {
            bool has_default = i + ndefaults >= nparams;
            if (has_default && !f->defaults[i + ndefaults - nparams]) {
                // "required keyword-only" hole: the argument must have been
                // supplied (sema guarantees it for direct calls; dynamic calls
                // that fail to provide it die with a clear message).
                std::string Ld = newlabel(), Lgo = newlabel();
                std::string ai = arg_or(i, Ld);
                std::string si = r();
                emit(si + " = getelementptr inbounds %kv, ptr %frame, i64 " +
                     std::to_string(i));
                emit("call void @kami_copy(ptr " + si + ", ptr " + ai + ")");
                emit("br label %" + Lgo);
                C().terminated = true;
                start_block(Ld);
                emit("call void @kami_panic(ptr " +
                     str_const(f->name + "() missing required keyword-only argument '" +
                               f->params[i] + "'") +
                     ")");
                emit("unreachable");
                C().terminated = true;
                start_block(Lgo);
                continue;
            }
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
            std::string Ld = newlabel(), Ln = newlabel();
            std::string ai = arg_or(i, Ld);
            {
                std::string si = r();
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
        // *args tuple / **kwargs dict: passed pre-packed as trailing arguments
        // (by sema for direct calls, by the runtime for dynamic calls); when
        // absent the slots are initialized to an empty tuple / dict.
        size_t extra_at = nparams;
        for (int which = 0; which < 2; which++) {
            const std::string& name = which == 0 ? f->vararg : f->kwarg;
            if (name.empty()) continue;
            std::string have = r();
            emit(have + " = icmp sgt i64 %nargs, " + std::to_string(extra_at));
            std::string Lp = newlabel(), Ld = newlabel(), Ln = newlabel();
            emit("br i1 " + have + ", label %" + Lp + ", label %" + Ld);
            C().terminated = true;
            start_block(Lp);
            {
                std::string pi = r(), ai = r(), si = r();
                emit(pi + " = getelementptr inbounds ptr, ptr %argv, i64 " +
                     std::to_string(extra_at));
                emit(ai + " = load ptr, ptr " + pi + ", align 8");
                emit(si + " = getelementptr inbounds %kv, ptr %frame, i64 " +
                     std::to_string(extra_at));
                emit("call void @kami_copy(ptr " + si + ", ptr " + ai + ")");
            }
            emit("br label %" + Ln);
            C().terminated = true;
            start_block(Ld);
            {
                std::string si = r();
                emit(si + " = getelementptr inbounds %kv, ptr %frame, i64 " +
                     std::to_string(extra_at));
                if (which == 0)
                    emit("call void @kami_make_list(ptr " + si + ", ptr %argbuf, i64 0)");
                else
                    emit("call void @kami_make_map(ptr " + si + ")");
            }
            start_block(Ln);
            extra_at++;
        }
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
declare void @kami_global_make_func(i64, ptr, i64, i64, ptr, i64, i64, ptr)
declare void @kami_global_make_class(i64, ptr, i64, i64)
declare void @kami_class_add_method(i64, ptr, ptr, i64, i64, i64, i64, ptr)
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
declare void @kami_make_closure(ptr, ptr, i64, i64, ptr, ptr, i64, i64, i64, ptr)
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
declare void @kami_call_star(ptr, ptr, ptr, ptr)
declare void @kami_method_star(ptr, ptr, ptr, ptr, ptr)
declare void @kami_str_iadd(ptr, ptr)
declare void @kami_free_hint(ptr)
declare void @kami_intern_str(ptr, ptr, i64)
declare void @kami_pyext_import(ptr, ptr)
declare void @kami_pyext_getattr(ptr, ptr, ptr)
declare void @kami_map_merge(ptr, ptr)
declare void @kami_panic(ptr)
declare i32 @kami_range_cond(ptr, ptr, ptr)
declare void @kami_iter_prep(ptr, ptr)
declare i32 @kami_iter_cond(ptr, ptr)
declare void @kami_iter_get(ptr, ptr, ptr)
declare void @kami_unpack(ptr, ptr, i64, i64)
declare i64 @kami_try(ptr, ptr, ptr)
declare void @kami_last_error(ptr)
declare void @kami_raise(ptr)
declare void @kami_rethrow()
declare void @kami_run_module(ptr)
declare void @kami_gen_create(ptr, ptr, ptr, i64)
declare void @kami_gen_yield(ptr, ptr)
declare i64 @kami_c_arg_i64(ptr)
declare double @kami_c_arg_f64(ptr)
declare ptr @kami_c_arg_cstr(ptr)
declare void @kami_c_ret_cstr(ptr, ptr)

)";

} // namespace

// ---------------------------------------------------------------------------
// Monomorphized native functions (type-inference driven, see typeinf.cpp).
//
// For every function whose parameters, locals and return type are statically
// primitive and whose body stays in the "native subset", we emit an extra
// specialization `@n_<alias>(i64/double, ...) -> i64/double/void`: values live
// in SSA registers / promoted allocas, arithmetic is raw LLVM instructions,
// calls go straight to other @n_ functions. No KamiValue, no GC, no runtime.
struct NativeGen {
    const Module& mod;
    const Stmt* fn;
    const FuncTypeInfo& fi;
    StrTable& strtab;
    std::set<std::string>& libm; // math declares needed by native bodies

    std::string body;
    std::string entry_allocas; // hoisted For-loop induction variables
    int reg = 0, label = 0, nind = 0;
    bool terminated = false;
    std::vector<std::pair<std::string, std::string>> loops; // (continue, break)

    struct V {
        std::string v;
        char k; // 'i' = i64, 'd' = double, 'b' = i1
    };

    NativeGen(const Module& m, const Stmt* f, const FuncTypeInfo& t, StrTable& st,
              std::set<std::string>& lm)
        : mod(m), fn(f), fi(t), strtab(st), libm(lm) {}

    std::string r() { return "%n" + std::to_string(reg++); }
    std::string newlabel() { return "N" + std::to_string(label++); }
    void emit(const std::string& line) { body += "  " + line + "\n"; }
    void start_block(const std::string& l) {
        if (!terminated) emit("br label %" + l);
        body += l + ":\n";
        terminated = false;
    }

    char lk(int64_t slot) { // storage kind of a local slot
        if (slot >= 0 && (size_t)slot < fi.locals.size() && fi.locals[slot] == TY_FLOAT)
            return 'd';
        return 'i';
    }
    static char rk(uint8_t ty) { return ty == TY_FLOAT ? 'd' : 'i'; }
    static const char* irty(char k) { return k == 'd' ? "double" : k == 'b' ? "i1" : "i64"; }

    V conv(V x, char want) {
        if (x.k == want) return x;
        std::string v = r();
        if (want == 'i') {
            if (x.k == 'b') emit(v + " = zext i1 " + x.v + " to i64");
            else emit(v + " = fptosi double " + x.v + " to i64");
        } else if (want == 'd') {
            if (x.k == 'b') {
                std::string t = r();
                emit(t + " = zext i1 " + x.v + " to i64");
                emit(v + " = sitofp i64 " + t + " to double");
            } else {
                emit(v + " = sitofp i64 " + x.v + " to double");
            }
        } else { // 'b'
            if (x.k == 'i') emit(v + " = icmp ne i64 " + x.v + ", 0");
            else emit(v + " = fcmp une double " + x.v + ", 0.0");
        }
        return {v, want};
    }

    void panic_if(const std::string& cond, const std::string& msg) {
        std::string Lp = newlabel(), Lok = newlabel();
        emit("br i1 " + cond + ", label %" + Lp + ", label %" + Lok);
        terminated = true;
        start_block(Lp);
        emit("call void @kami_panic(ptr " + strtab.ref(strtab.intern(msg)) + ")");
        emit("unreachable");
        terminated = true;
        start_block(Lok);
    }

    V gen_int_divmod(const Expr* e, const std::string& a, const std::string& b) {
        int op = (int)e->op;
        bool bconst = e->b->kind == ExprKind::IntLit;
        int64_t bc = bconst ? e->b->ival : 0;
        if (bconst && bc != 0 && bc != -1) {
            std::string q = r(), rm = r();
            if (op == KOP_FLOORDIV) {
                emit(q + " = sdiv i64 " + a + ", " + b);
                if (e->a->nonneg && bc > 0) return {q, 'i'};
                emit(rm + " = srem i64 " + a + ", " + b);
                std::string nz = r(), x = r(), sd = r(), adj = r(), adj64 = r(), v = r();
                emit(nz + " = icmp ne i64 " + rm + ", 0");
                emit(x + " = xor i64 " + a + ", " + b);
                emit(sd + " = icmp slt i64 " + x + ", 0");
                emit(adj + " = and i1 " + nz + ", " + sd);
                emit(adj64 + " = zext i1 " + adj + " to i64");
                emit(v + " = sub i64 " + q + ", " + adj64);
                return {v, 'i'};
            }
            emit(rm + " = srem i64 " + a + ", " + b);
            if (e->a->nonneg && bc > 0) return {rm, 'i'};
            std::string nz = r(), x = r(), sd = r(), adj = r(), addv = r(), v = r();
            emit(nz + " = icmp ne i64 " + rm + ", 0");
            emit(x + " = xor i64 " + rm + ", " + b);
            emit(sd + " = icmp slt i64 " + x + ", 0");
            emit(adj + " = and i1 " + nz + ", " + sd);
            emit(addv + " = select i1 " + adj + ", i64 " + b + ", i64 0");
            emit(v + " = add i64 " + rm + ", " + addv);
            return {v, 'i'};
        }
        std::string z = r();
        emit(z + " = icmp eq i64 " + b + ", 0");
        panic_if(z, op == KOP_FLOORDIV ? "integer division by zero"
                                       : "integer modulo by zero");
        std::string isneg1 = r();
        emit(isneg1 + " = icmp eq i64 " + b + ", -1");
        std::string Lneg = newlabel(), Lgen = newlabel(), Lend = newlabel();
        emit("br i1 " + isneg1 + ", label %" + Lneg + ", label %" + Lgen);
        terminated = true;
        start_block(Lneg);
        std::string vneg = r();
        if (op == KOP_FLOORDIV) emit(vneg + " = sub i64 0, " + a);
        else emit(vneg + " = add i64 0, 0");
        emit("br label %" + Lend);
        terminated = true;
        start_block(Lgen);
        std::string q = r(), rm = r(), vgen;
        emit(q + " = sdiv i64 " + a + ", " + b);
        emit(rm + " = srem i64 " + a + ", " + b);
        if (op == KOP_FLOORDIV) {
            std::string nz = r(), x = r(), sd = r(), adj = r(), adj64 = r();
            emit(nz + " = icmp ne i64 " + rm + ", 0");
            emit(x + " = xor i64 " + a + ", " + b);
            emit(sd + " = icmp slt i64 " + x + ", 0");
            emit(adj + " = and i1 " + nz + ", " + sd);
            emit(adj64 + " = zext i1 " + adj + " to i64");
            vgen = r();
            emit(vgen + " = sub i64 " + q + ", " + adj64);
        } else {
            std::string nz = r(), x = r(), sd = r(), adj = r(), addv = r();
            emit(nz + " = icmp ne i64 " + rm + ", 0");
            emit(x + " = xor i64 " + rm + ", " + b);
            emit(sd + " = icmp slt i64 " + x + ", 0");
            emit(adj + " = and i1 " + nz + ", " + sd);
            emit(addv + " = select i1 " + adj + ", i64 " + b + ", i64 0");
            vgen = r();
            emit(vgen + " = add i64 " + rm + ", " + addv);
        }
        emit("br label %" + Lend);
        terminated = true;
        start_block(Lend);
        std::string v = r();
        emit(v + " = phi i64 [ " + vneg + ", %" + Lneg + " ], [ " + vgen + ", %" + Lgen +
             " ]");
        return {v, 'i'};
    }

    V gen_expr(const Expr* e) {
        switch (e->kind) {
        case ExprKind::IntLit: return {std::to_string(e->ival), 'i'};
        case ExprKind::FloatLit: return {fmt_double(e->fval), 'd'};
        case ExprKind::BoolLit: return {e->ival ? "true" : "false", 'b'};
        case ExprKind::Name: {
            char k = lk(e->res_idx);
            std::string v = r();
            emit(v + " = load " + std::string(irty(k)) + ", ptr %l" +
                 std::to_string(e->res_idx) + ", align 8");
            return {v, k};
        }
        case ExprKind::Binary: {
            bool ints = FnGen::intlike(e->a->sty) && FnGen::intlike(e->b->sty);
            bool cmp = e->op == KOP_EQ || e->op == KOP_NE || e->op == KOP_LT ||
                       e->op == KOP_GT || e->op == KOP_LE || e->op == KOP_GE;
            V a = gen_expr(e->a.get());
            V b = gen_expr(e->b.get());
            if (ints) {
                a = conv(a, 'i');
                b = conv(b, 'i');
                const char* iop = nullptr;
                switch (e->op) {
                case KOP_ADD: iop = "add"; break;
                case KOP_SUB: iop = "sub"; break;
                case KOP_MUL: iop = "mul"; break;
                case KOP_BITAND: iop = "and"; break;
                case KOP_BITOR: iop = "or"; break;
                case KOP_BITXOR: iop = "xor"; break;
                default: break;
                }
                if (iop) {
                    std::string v = r();
                    emit(v + " = " + std::string(iop) + " i64 " + a.v + ", " + b.v);
                    return {v, 'i'};
                }
                if (cmp) {
                    const char* cc = e->op == KOP_EQ   ? "eq"
                                     : e->op == KOP_NE ? "ne"
                                     : e->op == KOP_LT ? "slt"
                                     : e->op == KOP_GT ? "sgt"
                                     : e->op == KOP_LE ? "sle"
                                                       : "sge";
                    std::string v = r();
                    emit(v + " = icmp " + std::string(cc) + " i64 " + a.v + ", " + b.v);
                    return {v, 'b'};
                }
                if (e->op == KOP_FLOORDIV || e->op == KOP_MOD)
                    return gen_int_divmod(e, a.v, b.v);
                if (e->op == KOP_DIV) {
                    std::string z = r();
                    emit(z + " = icmp eq i64 " + b.v + ", 0");
                    panic_if(z, "division by zero");
                    V fa = conv(a, 'd'), fb = conv(b, 'd');
                    std::string v = r();
                    emit(v + " = fdiv double " + fa.v + ", " + fb.v);
                    return {v, 'd'};
                }
            } else { // float math
                V fa = conv(a, 'd'), fb = conv(b, 'd');
                const char* fop = nullptr;
                switch (e->op) {
                case KOP_ADD: fop = "fadd"; break;
                case KOP_SUB: fop = "fsub"; break;
                case KOP_MUL: fop = "fmul"; break;
                default: break;
                }
                if (fop) {
                    std::string v = r();
                    emit(v + " = " + std::string(fop) + " double " + fa.v + ", " + fb.v);
                    return {v, 'd'};
                }
                if (e->op == KOP_DIV) {
                    std::string z = r();
                    emit(z + " = fcmp oeq double " + fb.v + ", 0.0");
                    panic_if(z, "division by zero");
                    std::string v = r();
                    emit(v + " = fdiv double " + fa.v + ", " + fb.v);
                    return {v, 'd'};
                }
                if (e->op == KOP_FLOORDIV || e->op == KOP_MOD) {
                    std::string z = r();
                    emit(z + " = fcmp oeq double " + fb.v + ", 0.0");
                    panic_if(z, e->op == KOP_FLOORDIV ? "float floor division by zero"
                                                      : "float modulo by zero");
                    libm.insert(e->op == KOP_FLOORDIV ? "floor" : "fmod");
                    std::string v = r();
                    if (e->op == KOP_FLOORDIV) {
                        std::string d = r();
                        emit(d + " = fdiv double " + fa.v + ", " + fb.v);
                        emit(v + " = call double @floor(double " + d + ")");
                        return {v, 'd'};
                    }
                    // Python sign rules for float %
                    std::string m0 = r(), nz = r(), xs = r(), neg = r(), adj = r(),
                                addv = r();
                    emit(m0 + " = call double @fmod(double " + fa.v + ", double " + fb.v +
                         ")");
                    emit(nz + " = fcmp une double " + m0 + ", 0.0");
                    emit(xs + " = fmul double " + m0 + ", " + fb.v);
                    emit(neg + " = fcmp olt double " + xs + ", 0.0");
                    emit(adj + " = and i1 " + nz + ", " + neg);
                    emit(addv + " = select i1 " + adj + ", double " + fb.v +
                         ", double 0.0");
                    emit(v + " = fadd double " + m0 + ", " + addv);
                    return {v, 'd'};
                }
                if (cmp) {
                    const char* cc = e->op == KOP_EQ   ? "oeq"
                                     : e->op == KOP_NE ? "une"
                                     : e->op == KOP_LT ? "olt"
                                     : e->op == KOP_GT ? "ogt"
                                     : e->op == KOP_LE ? "ole"
                                                       : "oge";
                    std::string v = r();
                    emit(v + " = fcmp " + std::string(cc) + " double " + fa.v + ", " + fb.v);
                    return {v, 'b'};
                }
            }
            return {"0", 'i'}; // unreachable: typeinf validated the subset
        }
        case ExprKind::Unary: {
            V a = gen_expr(e->a.get());
            if (e->op == KUOP_NOT) {
                V b = conv(a, 'b');
                std::string v = r();
                emit(v + " = xor i1 " + b.v + ", true");
                return {v, 'b'};
            }
            if (a.k == 'd') {
                std::string v = r();
                emit(v + " = fneg double " + a.v);
                return {v, 'd'};
            }
            a = conv(a, 'i');
            std::string v = r();
            emit(v + " = sub i64 0, " + a.v);
            return {v, 'i'};
        }
        case ExprKind::BoolOp: {
            // `a and b` / `a or b` with operand-value semantics via phi
            char k = rk(e->sty == TY_FLOAT ? TY_FLOAT : TY_INT);
            if (e->sty == TY_BOOL) k = 'b';
            V a = conv(gen_expr(e->a.get()), k);
            V at = conv(a, 'b');
            std::string Lb = newlabel(), Lend = newlabel();
            std::string Lcur = "entryless"; // filled below via explicit block
            std::string Lhave = newlabel();
            start_block(Lhave); // materialize a named predecessor for the phi
            if (e->op == 0)
                emit("br i1 " + at.v + ", label %" + Lb + ", label %" + Lend);
            else
                emit("br i1 " + at.v + ", label %" + Lend + ", label %" + Lb);
            terminated = true;
            start_block(Lb);
            V b = conv(gen_expr(e->b.get()), k);
            std::string Lbend = newlabel();
            start_block(Lbend);
            emit("br label %" + Lend);
            terminated = true;
            start_block(Lend);
            std::string v = r();
            emit(v + " = phi " + std::string(irty(k)) + " [ " + a.v + ", %" + Lhave +
                 " ], [ " + b.v + ", %" + Lbend + " ]");
            (void)Lcur;
            return {v, k};
        }
        case ExprKind::IfExp: {
            char k = rk(e->sty == TY_FLOAT ? TY_FLOAT : TY_INT);
            if (e->sty == TY_BOOL) k = 'b';
            V c = conv(gen_expr(e->b.get()), 'b');
            std::string Lt = newlabel(), Lf = newlabel(), Lend = newlabel();
            emit("br i1 " + c.v + ", label %" + Lt + ", label %" + Lf);
            terminated = true;
            start_block(Lt);
            V a = conv(gen_expr(e->a.get()), k);
            std::string Lta = newlabel();
            start_block(Lta);
            emit("br label %" + Lend);
            terminated = true;
            start_block(Lf);
            V d = conv(gen_expr(e->c.get()), k);
            std::string Lfa = newlabel();
            start_block(Lfa);
            emit("br label %" + Lend);
            terminated = true;
            start_block(Lend);
            std::string v = r();
            emit(v + " = phi " + std::string(irty(k)) + " [ " + a.v + ", %" + Lta +
                 " ], [ " + d.v + ", %" + Lfa + " ]");
            return {v, k};
        }
        case ExprKind::Call: {
            const Expr* callee = e->a.get();
            if (callee->res == Res::UserFunc) {
                size_t fidx = (size_t)callee->res_idx;
                const Stmt* def = mod.functions[fidx];
                const FuncTypeInfo& cal = mod.ftypes[fidx];
                const std::vector<uint8_t>& ps = eff_params(cal);
                uint8_t rt = eff_ret(cal);
                std::string args;
                for (size_t i = 0; i < e->args.size(); i++) {
                    char pk = rk(ps[i]);
                    V a = conv(gen_expr(e->args[i].get()), pk);
                    if (i) args += ", ";
                    args += std::string(irty(pk)) + " " + a.v;
                }
                char retk = rk(rt);
                if (rt == TY_NONE) {
                    emit("call void @n_" + def->alias + "(" + args + ")");
                    return {"0", 'i'};
                }
                std::string v = r();
                emit(v + " = call " + std::string(irty(retk)) + " @n_" + def->alias + "(" +
                     args + ")");
                return {v, retk};
            }
            // builtin math (validated by typeinf)
            int arity = 0;
            const char* sym =
                callee->res == Res::BuiltinFunc
                    ? native_math_symbol(callee->res_idx, &arity)
                    : nullptr;
            if (sym) {
                libm.insert(sym);
                std::string args;
                for (size_t i = 0; i < e->args.size(); i++) {
                    V a = conv(gen_expr(e->args[i].get()), 'd');
                    if (i) args += ", ";
                    args += "double " + a.v;
                }
                std::string v = r();
                emit(v + " = call double @" + std::string(sym) + "(" + args + ")");
                return {v, 'd'};
            }
            if (callee->res == Res::BuiltinFunc && callee->res_idx == KB_ABS) {
                V a = gen_expr(e->args[0].get());
                if (a.k == 'd' || e->args[0]->sty == TY_FLOAT) {
                    a = conv(a, 'd');
                    libm.insert("fabs");
                    std::string v = r();
                    emit(v + " = call double @fabs(double " + a.v + ")");
                    return {v, 'd'};
                }
                a = conv(a, 'i');
                std::string neg = r(), isn = r(), v = r();
                emit(isn + " = icmp slt i64 " + a.v + ", 0");
                emit(neg + " = sub i64 0, " + a.v);
                emit(v + " = select i1 " + isn + ", i64 " + neg + ", i64 " + a.v);
                return {v, 'i'};
            }
            return {"0", 'i'}; // unreachable
        }
        case ExprKind::CCall: {
            std::string args;
            for (size_t i = 0; i < e->args.size(); i++) {
                V a = conv(gen_expr(e->args[i].get()), 'd');
                if (i) args += ", ";
                args += "double " + a.v;
            }
            std::string v = r();
            emit(v + " = call double @" + e->sval + "(" + args + ")");
            return {v, 'd'};
        }
        default: return {"0", 'i'}; // unreachable
        }
    }

    void store_local(int64_t slot, V val) {
        char k = lk(slot);
        V x = conv(val, k);
        emit("store " + std::string(irty(k)) + " " + x.v + ", ptr %l" +
             std::to_string(slot) + ", align 8");
    }

    void gen_stmts(const std::vector<StmtPtr>& body_) {
        for (auto& sp : body_) {
            if (terminated) break;
            gen_stmt(sp.get());
        }
    }

    void gen_return(V v) {
        if (fi.ret == TY_NONE) {
            emit("ret void");
        } else {
            char k = rk(fi.ret);
            V x = conv(v, k == 'd' ? 'd' : 'i');
            emit("ret " + std::string(k == 'd' ? "double" : "i64") + " " + x.v);
        }
        terminated = true;
    }

    void gen_stmt(const Stmt* s) {
        switch (s->kind) {
        case StmtKind::Pass: return;
        case StmtKind::ExprStmt:
            gen_expr(s->e1.get());
            return;
        case StmtKind::Assign:
            store_local(s->target_idx, gen_expr(s->e1.get()));
            return;
        case StmtKind::Return:
            gen_return(s->e1 ? gen_expr(s->e1.get()) : V{"0", 'i'});
            return;
        case StmtKind::Break:
            emit("br label %" + loops.back().second);
            terminated = true;
            return;
        case StmtKind::Continue:
            emit("br label %" + loops.back().first);
            terminated = true;
            return;
        case StmtKind::If: {
            V c = conv(gen_expr(s->e1.get()), 'b');
            std::string Lt = newlabel(), Lf = newlabel(), Lend = newlabel();
            emit("br i1 " + c.v + ", label %" + Lt + ", label %" +
                 (s->orelse.empty() ? Lend : Lf));
            terminated = true;
            start_block(Lt);
            gen_stmts(s->body);
            if (!terminated) emit("br label %" + Lend);
            terminated = true;
            if (!s->orelse.empty()) {
                start_block(Lf);
                gen_stmts(s->orelse);
                if (!terminated) emit("br label %" + Lend);
                terminated = true;
            }
            start_block(Lend);
            return;
        }
        case StmtKind::While: {
            std::string Lc = newlabel(), Lb = newlabel(), Lend = newlabel();
            start_block(Lc);
            V c = conv(gen_expr(s->e1.get()), 'b');
            emit("br i1 " + c.v + ", label %" + Lb + ", label %" + Lend);
            terminated = true;
            start_block(Lb);
            loops.push_back({Lc, Lend});
            gen_stmts(s->body);
            loops.pop_back();
            if (!terminated) emit("br label %" + Lc);
            terminated = true;
            start_block(Lend);
            return;
        }
        case StmtKind::For: {
            const auto& args = s->e1->args;
            V start{"0", 'i'}, stop{"0", 'i'}, step{"1", 'i'};
            if (args.size() == 1) {
                stop = conv(gen_expr(args[0].get()), 'i');
            } else {
                start = conv(gen_expr(args[0].get()), 'i');
                stop = conv(gen_expr(args[1].get()), 'i');
            }
            bool step_const = true;
            int64_t step_sign = 1;
            if (args.size() == 3) {
                step = conv(gen_expr(args[2].get()), 'i');
                std::string z = r();
                emit(z + " = icmp eq i64 " + step.v + ", 0");
                panic_if(z, "range() step must not be zero");
                step_const = args[2]->kind == ExprKind::IntLit;
                step_sign = step_const && args[2]->ival < 0 ? -1 : 1;
            }
            std::string ind = "%ind" + std::to_string(nind++);
            entry_allocas += "  " + ind + " = alloca i64, align 8\n";
            emit("store i64 " + start.v + ", ptr " + ind + ", align 8");
            std::string Lc = newlabel(), Lb = newlabel(), Ls = newlabel(),
                        Lend = newlabel();
            start_block(Lc);
            std::string vi = r();
            emit(vi + " = load i64, ptr " + ind + ", align 8");
            std::string b = r();
            if (step_const) {
                emit(b + " = icmp " + (step_sign > 0 ? std::string("slt") : std::string("sgt")) +
                     " i64 " + vi + ", " + stop.v);
            } else {
                std::string pos = r(), c1 = r(), c2 = r();
                emit(pos + " = icmp sgt i64 " + step.v + ", 0");
                emit(c1 + " = icmp slt i64 " + vi + ", " + stop.v);
                emit(c2 + " = icmp sgt i64 " + vi + ", " + stop.v);
                emit(b + " = select i1 " + pos + ", i1 " + c1 + ", i1 " + c2);
            }
            emit("br i1 " + b + ", label %" + Lb + ", label %" + Lend);
            terminated = true;
            start_block(Lb);
            store_local(s->target_idx, {vi, 'i'});
            loops.push_back({Ls, Lend});
            gen_stmts(s->body);
            loops.pop_back();
            start_block(Ls);
            std::string vi2 = r(), vn = r();
            emit(vi2 + " = load i64, ptr " + ind + ", align 8");
            emit(vn + " = add i64 " + vi2 + ", " + step.v);
            emit("store i64 " + vn + ", ptr " + ind + ", align 8");
            emit("br label %" + Lc);
            terminated = true;
            start_block(Lend);
            return;
        }
        default: return; // unreachable: typeinf validated the subset
        }
    }

    std::string finish() {
        std::string sig;
        for (size_t i = 0; i < fn->params.size(); i++) {
            if (i) sig += ", ";
            sig += std::string(irty(rk(fi.params[i]))) + " %p" + std::to_string(i);
        }
        const char* rty = fi.ret == TY_NONE ? "void" : fi.ret == TY_FLOAT ? "double" : "i64";
        std::string out = "define internal " + std::string(rty) + " @n_" + fn->alias + "(" +
                          sig + ") {\nentry:\n";
        for (size_t i = 0; i < fi.locals.size(); i++)
            out += "  %l" + std::to_string(i) + " = alloca " +
                   std::string(irty(lk((int64_t)i))) + ", align 8\n";
        out += entry_allocas;
        for (size_t i = 0; i < fn->params.size(); i++) {
            char k = lk((int64_t)i);
            char pk = rk(fi.params[i]);
            std::string pv = "%p" + std::to_string(i);
            // parameter kind matches the local kind by construction
            out += "  store " + std::string(irty(k)) + " " + pv + ", ptr %l" +
                   std::to_string(i) + ", align 8\n";
            (void)pk;
        }
        out += body;
        if (!terminated) {
            if (fi.ret == TY_NONE) out += "  ret void\n";
            else if (fi.ret == TY_FLOAT) out += "  ret double 0.0\n";
            else out += "  ret i64 0\n";
        }
        out += "}\n\n";
        return out;
    }
};

std::string codegen(const Module& m, const std::string& source_name) {
    StrTable strtab;
    std::string fns;
    g_outline_counter = 0;

    // monomorphized native specializations first (boxed bodies call them)
    std::set<std::string> libm;
    for (size_t i = 0; i < m.functions.size(); i++) {
        if (i >= m.ftypes.size()) break;
        const FuncTypeInfo& fi = m.ftypes[i];
        if (fi.native_ok) {
            NativeGen ng(m, m.functions[i], fi, strtab, libm);
            ng.gen_stmts(m.functions[i]->body);
            fns += ng.finish();
        } else if (fi.guarded) {
            // speculative specialization: emit under the spec type stamps,
            // then restore the generic stamps for the boxed body.
            FuncTypeInfo eff = fi;
            eff.locals = fi.spec_locals;
            eff.params = fi.spec_params;
            eff.ret = fi.spec_ret;
            stamp_function(const_cast<Module&>(m), i, /*spec=*/true);
            NativeGen ng(m, m.functions[i], eff, strtab, libm);
            ng.gen_stmts(m.functions[i]->body);
            fns += ng.finish();
            stamp_function(const_cast<Module&>(m), i, /*spec=*/false);
        }
    }

    auto gen_fn = [&](const Stmt* f, const FuncTypeInfo* fi) {
        FnGen g(m, strtab);
        g.nlocals = f->nlocals;
        bool ht = stmts_have_try(f->body);
        g.temps_base = f->nlocals + (ht ? 1 : 0);
        g.spill_slot = ht ? f->nlocals : -1;
        if (fi && fi->guarded) g.gen_guard(f, *fi);
        g.gen_prologue(f);
        g.gen_stmts(f->body);
        fns += g.finish("u_" + f->alias, ht);
    };
    for (size_t i = 0; i < m.functions.size(); i++)
        gen_fn(m.functions[i], i < m.ftypes.size() ? &m.ftypes[i] : nullptr);

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
    // pre-intern every string literal used as a value (cells live in g_intern,
    // which the GC marks — no extra root registration needed)
    for (auto& [si, ci] : strtab.value_cache)
        main_fn += "  call void @kami_intern_str(ptr @.ic" + std::to_string(ci) +
                   ", ptr " + strtab.ref(si) + ", i64 " +
                   std::to_string(strtab.strs[(size_t)si].size()) + ")\n";
    for (const Stmt* f : m.functions) {
        if (f->global_idx < 0) continue; // methods have no global binding
        int si = strtab.intern(f->name);
        size_t fmin = f->params.size() - f->defaults.size();
        int pi = strtab.intern(join_params(f));
        main_fn += "  call void @kami_global_make_func(i64 " + std::to_string(f->global_idx) +
                   ", ptr @u_" + f->alias + ", i64 " + std::to_string(fmin) + ", i64 " +
                   std::to_string(f->params.size()) + ", ptr " + strtab.ref(si) + ", i64 " +
                   std::to_string(func_kwonly(f)) + ", i64 " + std::to_string(func_flags(f)) +
                   ", ptr " + strtab.ref(pi) + ")\n";
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
                   ", ptr " + strtab.ref(si) + ", i64 " + std::to_string(parent) + ", i64 " +
                   std::to_string(c->is_exception ? 1 : 0) + ")\n";
        for (auto& msp : c->body) {
            if (msp->kind != StmtKind::FuncDef) continue;
            const Stmt* mth = msp.get();
            int mi = strtab.intern(mth->name);
            size_t mmin = mth->params.size() - mth->defaults.size();
            int pi = strtab.intern(join_params(mth));
            main_fn += "  call void @kami_class_add_method(i64 " +
                       std::to_string(c->global_idx) + ", ptr " + strtab.ref(mi) + ", ptr @u_" +
                       mth->alias + ", i64 " + std::to_string(mmin) + ", i64 " +
                       std::to_string(mth->params.size()) + ", i64 " +
                       std::to_string(func_kwonly(mth)) + ", i64 " +
                       std::to_string(func_flags(mth)) + ", ptr " + strtab.ref(pi) + ")\n";
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
    for (auto& [si, ci] : strtab.value_cache)
        out += "@.ic" + std::to_string(ci) + " = private global ptr null\n";
    out += "\n";
    out += RUNTIME_DECLS;
    // libm functions used by monomorphized native bodies (sqrt, sin, ...).
    // Skip anything already declared through the C-ABI bindings below.
    for (const std::string& sym : libm) {
        bool dup = false;
        for (const NativeDecl& n : m.natives)
            if (n.symbol == sym) dup = true;
        if (dup) continue;
        int arity = 2;
        if (sym == "atan2" || sym == "pow" || sym == "fmod") arity = 2;
        else arity = 1;
        out += "declare double @" + sym + "(double" + (arity == 2 ? ", double" : "") + ")\n";
    }
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
