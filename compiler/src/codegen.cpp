#include "codegen.h"

#include "../../runtime/include/kami_builtins.h"
#include "../../runtime/include/kami_runtime.h"

#include <cinttypes>
#include <cstdio>
#include <cstring>

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
    // Bit-exact double constant for LLVM IR.
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

struct FnGen {
    const Module& mod;
    StrTable& strtab;
    std::string body;
    int nlocals = 0;
    int temp_top = 0;  // next temp slot (relative, temps start at nlocals)
    int max_temps = 0;
    int max_call_args = 1;
    int reg = 0;
    int label = 0;
    bool terminated = false;
    bool is_module = false;
    std::vector<std::pair<std::string, std::string>> loops; // (continue, break)

    FnGen(const Module& m, StrTable& st) : mod(m), strtab(st) {}

    std::string r() { return "%r" + std::to_string(reg++); }
    std::string newlabel() { return "L" + std::to_string(label++); }

    void emit(const std::string& line) { body += "  " + line + "\n"; }

    void start_block(const std::string& l) {
        if (!terminated) emit("br label %" + l);
        body += l + ":\n";
        terminated = false;
    }

    int alloc_temp() {
        int t = nlocals + temp_top;
        temp_top++;
        if (temp_top > max_temps) max_temps = temp_top;
        return t;
    }

    std::string slot_ptr(int slot) {
        std::string p = r();
        emit(p + " = getelementptr inbounds %kv, ptr %frame, i64 " + std::to_string(slot));
        return p;
    }

    void note_call_args(int n) {
        if (n > max_call_args) max_call_args = n;
    }

    // Store arg slot pointers into %argbuf[0..n)
    void fill_argbuf(const std::vector<int>& slots) {
        note_call_args((int)slots.size());
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

    // ---- variables ----
    void assign_var(Res res, int64_t idx, int src_slot) {
        std::string sp = slot_ptr(src_slot);
        if (res == Res::Local) {
            std::string dp = slot_ptr((int)idx);
            emit("call void @kami_copy(ptr " + dp + ", ptr " + sp + ")");
        } else {
            emit("call void @kami_global_set(i64 " + std::to_string(idx) + ", ptr " + sp + ")");
        }
    }

    int read_var(Res res, int64_t idx) {
        if (res == Res::Local) return (int)idx;
        int t = alloc_temp();
        std::string tp = slot_ptr(t);
        emit("call void @kami_global_get(ptr " + tp + ", i64 " + std::to_string(idx) + ")");
        return t;
    }

    // ---- expressions: returns the slot holding the value ----
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
            int si = strtab.intern(e->sval);
            emit("call void @kami_make_str(ptr " + slot_ptr(t) + ", ptr " + strtab.ref(si) +
                 ", i64 " + std::to_string(e->sval.size()) + ")");
            return t;
        }
        case ExprKind::Name:
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
            if (e->op == 0) // and: evaluate rhs only if lhs truthy
                emit("br i1 " + b + ", label %" + Leval + ", label %" + Lend);
            else // or
                emit("br i1 " + b + ", label %" + Lend + ", label %" + Leval);
            terminated = true;
            start_block(Leval);
            int save2 = temp_top;
            int bslot = gen_expr(e->b.get());
            emit("call void @kami_copy(ptr " + slot_ptr(t) + ", ptr " + slot_ptr(bslot) + ")");
            temp_top = save2;
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
        case ExprKind::ListLit: {
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
        case ExprKind::Call: {
            const Expr* callee = e->a.get();
            int save = temp_top;
            std::vector<int> slots;
            for (auto& a : e->args) slots.push_back(gen_expr(a.get()));
            if (callee->res == Res::UserFunc) {
                fill_argbuf(slots);
                temp_top = save;
                int t = alloc_temp();
                const Stmt* f = mod.functions[(size_t)callee->res_idx];
                emit("call void @u_" + f->name + "(ptr " + slot_ptr(t) + ", ptr %argbuf)");
                return t;
            }
            if (callee->res == Res::BuiltinFunc) {
                fill_argbuf(slots);
                temp_top = save;
                int t = alloc_temp();
                emit("call void @kami_builtin(i64 " + std::to_string(callee->res_idx) + ", ptr " +
                     slot_ptr(t) + ", ptr %argbuf, i64 " + std::to_string(slots.size()) + ")");
                return t;
            }
            // dynamic call through a value
            int c = gen_expr(callee);
            std::string cp = slot_ptr(c);
            fill_argbuf(slots);
            temp_top = save;
            int t = alloc_temp();
            emit("call void @kami_call_value(ptr " + slot_ptr(t) + ", ptr " + cp +
                 ", ptr %argbuf, i64 " + std::to_string(slots.size()) + ")");
            return t;
        }
        case ExprKind::MethodCall: {
            int save = temp_top;
            int base = gen_expr(e->a.get());
            std::string basep = slot_ptr(base);
            std::vector<int> slots;
            for (auto& a : e->args) slots.push_back(gen_expr(a.get()));
            fill_argbuf(slots);
            temp_top = save;
            int t = alloc_temp();
            int si = strtab.intern(e->sval);
            emit("call void @kami_method(ptr " + slot_ptr(t) + ", ptr " + basep + ", ptr " +
                 strtab.ref(si) + ", ptr %argbuf, i64 " + std::to_string(slots.size()) + ")");
            return t;
        }
        case ExprKind::Attr:
            throw CompileError(e->line, "internal error: unresolved attribute in codegen");
        }
        throw CompileError(e->line, "internal error: bad expression kind");
    }

    // ---- statements ----
    void gen_stmts(const std::vector<StmtPtr>& body) {
        for (auto& s : body) {
            if (terminated) break; // trivially dead code elimination
            gen_stmt(s.get());
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
        case StmtKind::If: {
            int c = gen_expr(s->e1.get());
            std::string b = truthy(c);
            temp_top = save;
            std::string Lthen = newlabel();
            std::string Lelse = s->orelse.empty() ? "" : newlabel();
            std::string Lend = newlabel();
            emit("br i1 " + b + ", label %" + Lthen + ", label %" +
                 (Lelse.empty() ? Lend : Lelse));
            terminated = true;
            start_block(Lthen);
            gen_stmts(s->body);
            if (!terminated) emit("br label %" + Lend);
            terminated = true;
            if (!Lelse.empty()) {
                start_block(Lelse);
                gen_stmts(s->orelse);
                if (!terminated) emit("br label %" + Lend);
                terminated = true;
            }
            start_block(Lend);
            break;
        }
        case StmtKind::While: {
            std::string Lcond = newlabel(), Lbody = newlabel(), Lend = newlabel();
            start_block(Lcond);
            int c = gen_expr(s->e1.get());
            std::string b = truthy(c);
            temp_top = save;
            emit("br i1 " + b + ", label %" + Lbody + ", label %" + Lend);
            terminated = true;
            start_block(Lbody);
            loops.push_back({Lcond, Lend});
            gen_stmts(s->body);
            loops.pop_back();
            if (!terminated) emit("br label %" + Lcond);
            terminated = true;
            start_block(Lend);
            break;
        }
        case StmtKind::For:
            gen_for(s, save);
            break;
        case StmtKind::FuncDef:
            break; // emitted separately
        case StmtKind::Return: {
            if (s->e1) {
                int v = gen_expr(s->e1.get());
                emit("call void @kami_copy(ptr %ret, ptr " + slot_ptr(v) + ")");
            } else {
                emit("call void @kami_make_none(ptr %ret)");
            }
            emit("call void @kami_frame_pop()");
            emit("ret void");
            terminated = true;
            break;
        }
        case StmtKind::Break:
            emit("br label %" + loops.back().second);
            terminated = true;
            break;
        case StmtKind::Continue:
            emit("br label %" + loops.back().first);
            terminated = true;
            break;
        case StmtKind::Pass:
        case StmtKind::Import:
            break;
        }
        temp_top = save;
    }

    static bool is_range_call(const Expr* e) {
        return e->kind == ExprKind::Call && e->a->res == Res::BuiltinFunc &&
               e->a->res_idx == KB_RANGE;
    }

    void gen_for(const Stmt* s, int save) {
        if (is_range_call(s->e1.get())) {
            // for i in range(...): counting loop — the list is never materialized
            const auto& args = s->e1->args;
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
            // hidden counter keeps its own slot so user code may reassign the loop var
            int s_i = alloc_temp();
            emit("call void @kami_copy(ptr " + slot_ptr(s_i) + ", ptr " + slot_ptr(s_start) +
                 ")");
            std::string Lcond = newlabel(), Lbody = newlabel(), Lstep = newlabel(),
                        Lend = newlabel();
            start_block(Lcond);
            std::string c = r();
            emit(c + " = call i32 @kami_range_cond(ptr " + slot_ptr(s_i) + ", ptr " +
                 slot_ptr(s_stop) + ", ptr " + slot_ptr(s_step) + ")");
            std::string b = r();
            emit(b + " = icmp ne i32 " + c + ", 0");
            emit("br i1 " + b + ", label %" + Lbody + ", label %" + Lend);
            terminated = true;
            start_block(Lbody);
            assign_var(s->target_res, s->target_idx, s_i);
            loops.push_back({Lstep, Lend});
            gen_stmts(s->body);
            loops.pop_back();
            start_block(Lstep);
            emit("call void @kami_binop(i64 " + std::to_string((int)KOP_ADD) + ", ptr " +
                 slot_ptr(s_i) + ", ptr " + slot_ptr(s_i) + ", ptr " + slot_ptr(s_step) + ")");
            emit("br label %" + Lcond);
            terminated = true;
            start_block(Lend);
            temp_top = save;
            return;
        }
        // generic: iterate a list or string by index
        int s_seq = gen_expr(s->e1.get());
        int s_idx = alloc_temp();
        emit("call void @kami_make_int(ptr " + slot_ptr(s_idx) + ", i64 0)");
        int s_one = alloc_temp();
        emit("call void @kami_make_int(ptr " + slot_ptr(s_one) + ", i64 1)");
        std::string Lcond = newlabel(), Lbody = newlabel(), Lstep = newlabel(),
                    Lend = newlabel();
        start_block(Lcond);
        std::string c = r();
        emit(c + " = call i32 @kami_iter_cond(ptr " + slot_ptr(s_seq) + ", ptr " +
             slot_ptr(s_idx) + ")");
        std::string b = r();
        emit(b + " = icmp ne i32 " + c + ", 0");
        emit("br i1 " + b + ", label %" + Lbody + ", label %" + Lend);
        terminated = true;
        start_block(Lbody);
        {
            int save2 = temp_top;
            int elem = alloc_temp();
            emit("call void @kami_iter_get(ptr " + slot_ptr(elem) + ", ptr " + slot_ptr(s_seq) +
                 ", ptr " + slot_ptr(s_idx) + ")");
            assign_var(s->target_res, s->target_idx, elem);
            temp_top = save2;
        }
        loops.push_back({Lstep, Lend});
        gen_stmts(s->body);
        loops.pop_back();
        start_block(Lstep);
        emit("call void @kami_binop(i64 " + std::to_string((int)KOP_ADD) + ", ptr " +
             slot_ptr(s_idx) + ", ptr " + slot_ptr(s_idx) + ", ptr " + slot_ptr(s_one) + ")");
        emit("br label %" + Lcond);
        terminated = true;
        start_block(Lend);
        temp_top = save;
    }

    // ---- whole function ----
    std::string finish(const std::string& fn_name, const std::vector<std::string>& params) {
        int total = nlocals + max_temps;
        if (total < 1) total = 1;
        std::string out;
        out += "define void @" + fn_name + "(ptr %ret, ptr %argv) {\n";
        out += "entry:\n";
        out += "  %frame = alloca %kv, i64 " + std::to_string(total) + ", align 8\n";
        out += "  %argbuf = alloca ptr, i64 " + std::to_string(max_call_args) + ", align 8\n";
        out += "  call void @kami_frame_push(ptr %frame, i64 " + std::to_string(total) + ")\n";
        for (size_t i = 0; i < params.size(); i++) {
            std::string pi = "%parg" + std::to_string(i);
            std::string ai = "%aarg" + std::to_string(i);
            std::string si = "%sarg" + std::to_string(i);
            out += "  " + pi + " = getelementptr inbounds ptr, ptr %argv, i64 " +
                   std::to_string(i) + "\n";
            out += "  " + ai + " = load ptr, ptr " + pi + ", align 8\n";
            out += "  " + si + " = getelementptr inbounds %kv, ptr %frame, i64 " +
                   std::to_string(i) + "\n";
            out += "  call void @kami_copy(ptr " + si + ", ptr " + ai + ")\n";
        }
        out += body;
        if (!terminated) {
            out += "  call void @kami_make_none(ptr %ret)\n";
            out += "  call void @kami_frame_pop()\n";
            out += "  ret void\n";
        }
        out += "}\n\n";
        return out;
    }
};

const char* RUNTIME_DECLS = R"(declare void @kami_rt_init()
declare void @kami_rt_shutdown()
declare void @kami_globals_init(i64)
declare void @kami_global_get(ptr, i64)
declare void @kami_global_set(i64, ptr)
declare void @kami_global_make_func(i64, ptr, i64, ptr)
declare void @kami_frame_push(ptr, i64)
declare void @kami_frame_pop()
declare void @kami_copy(ptr, ptr)
declare void @kami_make_none(ptr)
declare void @kami_make_bool(ptr, i64)
declare void @kami_make_int(ptr, i64)
declare void @kami_make_float(ptr, double)
declare void @kami_make_str(ptr, ptr, i64)
declare void @kami_make_list(ptr, ptr, i64)
declare void @kami_make_map(ptr)
declare i32 @kami_truthy(ptr)
declare void @kami_binop(i64, ptr, ptr, ptr)
declare void @kami_unop(i64, ptr, ptr)
declare void @kami_index_get(ptr, ptr, ptr)
declare void @kami_index_set(ptr, ptr, ptr)
declare void @kami_call_value(ptr, ptr, ptr, i64)
declare void @kami_method(ptr, ptr, ptr, ptr, i64)
declare void @kami_builtin(i64, ptr, ptr, i64)
declare i32 @kami_range_cond(ptr, ptr, ptr)
declare i32 @kami_iter_cond(ptr, ptr)
declare void @kami_iter_get(ptr, ptr, ptr)

)";

} // namespace

std::string codegen(const Module& m, const std::string& source_name) {
    StrTable strtab;
    std::string fns;

    // user functions
    for (const Stmt* f : m.functions) {
        FnGen g(m, strtab);
        g.nlocals = f->nlocals;
        g.gen_stmts(f->body);
        fns += g.finish("u_" + f->name, f->params);
    }

    // module body
    FnGen g(m, strtab);
    g.is_module = true;
    g.nlocals = 0;
    g.gen_stmts(m.body);
    fns += g.finish("kamipy_module", {});

    // main
    std::string main_fn;
    main_fn += "define i32 @main(i32 %argc, ptr %cargv) {\nentry:\n";
    main_fn += "  call void @kami_rt_init()\n";
    main_fn += "  call void @kami_globals_init(i64 " + std::to_string(m.nglobals) + ")\n";
    for (const Stmt* f : m.functions) {
        int si = strtab.intern(f->name);
        main_fn += "  call void @kami_global_make_func(i64 " + std::to_string(f->global_idx) +
                   ", ptr @u_" + f->name + ", i64 " + std::to_string(f->params.size()) +
                   ", ptr " + strtab.ref(si) + ")\n";
    }
    main_fn += "  %mret = alloca %kv, align 8\n";
    main_fn += "  call void @kamipy_module(ptr %mret, ptr null)\n";
    main_fn += "  call void @kami_rt_shutdown()\n";
    main_fn += "  ret i32 0\n}\n";

    // assemble
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
    out += fns;
    out += main_fn;
    return out;
}

} // namespace kami
