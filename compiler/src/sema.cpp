#include "sema.h"

#include "../../runtime/include/kami_builtins.h"
#include "../../runtime/include/kami_runtime.h"

#include <cmath>
#include <limits>
#include <set>
#include <unordered_map>

namespace kami {

namespace {

struct BuiltinSig {
    int64_t id;
    int min_args;
    int max_args;
};

static const std::unordered_map<std::string, BuiltinSig>& builtins() {
    static const std::unordered_map<std::string, BuiltinSig> b = {
        {"print", {KB_PRINT, 0, 16}}, {"len", {KB_LEN, 1, 1}},
        {"str", {KB_STR, 1, 1}},      {"int", {KB_INT, 1, 2}},
        {"float", {KB_FLOAT, 1, 1}},  {"abs", {KB_ABS, 1, 1}},
        {"min", {KB_MIN, 1, 16}},     {"max", {KB_MAX, 1, 16}},
        {"ord", {KB_ORD, 1, 1}},      {"chr", {KB_CHR, 1, 1}},
        {"type", {KB_TYPE, 1, 1}},    {"range", {KB_RANGE, 1, 3}},
        {"sum", {KB_SUM, 1, 2}},      {"sorted", {KB_SORTED, 1, 1}},
        {"reversed", {KB_REVERSED, 1, 1}}, {"enumerate", {KB_ENUMERATE, 1, 2}},
        {"zip", {KB_ZIP, 2, 2}},      {"bool", {KB_BOOL, 1, 1}},
        {"round", {KB_ROUND, 1, 2}},  {"input", {KB_INPUT, 0, 1}},
        {"pow", {KB_POW, 2, 3}},      {"all", {KB_ALL, 1, 1}},
        {"any", {KB_ANY, 1, 1}},      {"bin", {KB_BIN, 1, 1}},
        {"hex", {KB_HEX, 1, 1}},      {"oct", {KB_OCT, 1, 1}},
        {"list", {KB_LIST, 0, 1}},    {"dict", {KB_DICT, 0, 0}},
        {"tuple", {KB_TUPLE, 0, 1}},  {"isinstance", {KB_ISINSTANCE, 2, 2}},
        {"format", {KB_FORMAT, 1, 2}}, {"divmod", {KB_DIVMOD, 2, 2}},
        {"exit", {KB_SYS_EXIT, 0, 1}}, {"quit", {KB_SYS_EXIT, 0, 1}},
        {"set", {KB_SET, 0, 1}}, {"open", {KB_OPEN, 1, 2}},
        {"map", {KB_MAP, 2, 2}}, {"filter", {KB_FILTER, 2, 2}},
    };
    return b;
}

static const std::unordered_map<std::string, std::unordered_map<std::string, BuiltinSig>>&
modules() {
    static const std::unordered_map<std::string, std::unordered_map<std::string, BuiltinSig>>
        m = {
            {"math",
             {{"sqrt", {KB_MATH_SQRT, 1, 1}}, {"sin", {KB_MATH_SIN, 1, 1}},
              {"cos", {KB_MATH_COS, 1, 1}},   {"tan", {KB_MATH_TAN, 1, 1}},
              {"exp", {KB_MATH_EXP, 1, 1}},   {"log", {KB_MATH_LOG, 1, 2}},
              {"pow", {KB_MATH_POW, 2, 2}},   {"floor", {KB_MATH_FLOOR, 1, 1}},
              {"ceil", {KB_MATH_CEIL, 1, 1}}, {"fabs", {KB_MATH_FABS, 1, 1}},
              {"factorial", {KB_MATH_FACTORIAL, 1, 1}}, {"gcd", {KB_MATH_GCD, 0, 16}},
              {"isqrt", {KB_MATH_ISQRT, 1, 1}}, {"hypot", {KB_MATH_HYPOT, 2, 16}},
              {"log2", {KB_MATH_LOG2, 1, 1}}, {"log10", {KB_MATH_LOG10, 1, 1}},
              {"atan", {KB_MATH_ATAN, 1, 1}}, {"asin", {KB_MATH_ASIN, 1, 1}},
              {"acos", {KB_MATH_ACOS, 1, 1}}, {"atan2", {KB_MATH_ATAN2, 2, 2}},
              {"degrees", {KB_MATH_DEGREES, 1, 1}}, {"radians", {KB_MATH_RADIANS, 1, 1}},
              {"trunc", {KB_MATH_TRUNC, 1, 1}}, {"isnan", {KB_MATH_ISNAN, 1, 1}},
              {"isinf", {KB_MATH_ISINF, 1, 1}}}},
            {"time", {{"time", {KB_TIME_TIME, 0, 0}}, {"sleep", {KB_TIME_SLEEP, 1, 1}},
                      {"monotonic", {KB_TIME_MONOTONIC, 0, 0}},
                      {"perf_counter", {KB_TIME_PERF_COUNTER, 0, 0}}}},
            {"random",
             {{"random", {KB_RANDOM_RANDOM, 0, 0}},
              {"randint", {KB_RANDOM_RANDINT, 2, 2}},
              {"seed", {KB_RANDOM_SEED, 0, 1}},
              {"randrange", {KB_RANDOM_RANDRANGE, 1, 3}},
              {"choice", {KB_RANDOM_CHOICE, 1, 1}},
              {"shuffle", {KB_RANDOM_SHUFFLE, 1, 1}},
              {"uniform", {KB_RANDOM_UNIFORM, 2, 2}},
              {"sample", {KB_RANDOM_SAMPLE, 2, 2}},
              {"choices", {KB_RANDOM_CHOICES, 1, 2}}}},
            {"threading",
             {{"spawn", {KB_THREAD_SPAWN, 1, 9}}, {"join", {KB_THREAD_JOIN, 1, 1}}}},
            {"sys", {{"exit", {KB_SYS_EXIT, 0, 1}}}},
            {"doctest", {{"testmod", {KB_NOOP, 0, 2}}}},
            // _kami — the raw OS layer the Python stdlib is written against.
            // Every entry is a thin C-ABI forward to libc (see
            // runtime/src/syscalls.cpp); nothing here implements a module.
            {"_kami",
             {{"fd_open", {KB_SYS_FD_OPEN, 2, 2}},
              {"fd_read", {KB_SYS_FD_READ, 2, 2}},
              {"fd_write", {KB_SYS_FD_WRITE, 2, 2}},
              {"fd_close", {KB_SYS_FD_CLOSE, 1, 1}},
              {"fd_seek", {KB_SYS_FD_SEEK, 3, 3}},
              {"stat", {KB_SYS_STAT, 1, 1}},
              {"filesize", {KB_SYS_FILESIZE, 1, 1}},
              {"listdir", {KB_SYS_LISTDIR, 1, 1}},
              {"mkdir", {KB_SYS_MKDIR, 1, 1}},
              {"rmdir", {KB_SYS_RMDIR, 1, 1}},
              {"unlink", {KB_SYS_UNLINK, 1, 1}},
              {"rename", {KB_SYS_RENAME, 2, 2}},
              {"getcwd", {KB_SYS_GETCWD, 0, 0}},
              {"chdir", {KB_SYS_CHDIR, 1, 1}},
              {"getenv", {KB_SYS_GETENV, 1, 1}},
              {"system", {KB_SYS_SYSTEM, 1, 1}},
              {"getpid", {KB_SYS_GETPID, 0, 0}},
              {"errmsg", {KB_SYS_ERRMSG, 0, 0}},
              {"platform", {KB_SYS_PLATFORM, 0, 0}},
              {"localtime", {KB_SYS_LOCALTIME, 1, 1}},
              {"sock_open", {KB_SOCK_OPEN, 0, 0}},
              {"sock_connect", {KB_SOCK_CONNECT, 3, 3}},
              {"sock_bind", {KB_SOCK_BIND, 3, 3}},
              {"sock_listen", {KB_SOCK_LISTEN, 2, 2}},
              {"sock_accept", {KB_SOCK_ACCEPT, 1, 1}},
              {"sock_send", {KB_SOCK_SEND, 2, 2}},
              {"sock_recv", {KB_SOCK_RECV, 2, 2}},
              {"sock_close", {KB_SOCK_CLOSE, 1, 1}},
              {"sock_timeout", {KB_SOCK_TIMEOUT, 2, 2}}}},
        };
    return m;
}

// Imports that are accepted and ignored (annotation-only / test helpers).
static bool noop_module(const std::string& name) {
    return name == "typing" || name == "__future__" || name == "abc" ||
           name == "dataclasses" || name == "collections.abc";
}

// module constants
struct ModConst {
    int kind; // 0=float, 1=str, 2=int
    double f;
    const char* s;
    int64_t i;
};
static bool module_const(const std::string& mod, const std::string& attr, ModConst& out) {
    if (mod == "math" && attr == "pi") { out = {0, 3.14159265358979323846, "", 0}; return true; }
    if (mod == "math" && attr == "e") { out = {0, 2.71828182845904523536, "", 0}; return true; }
    if (mod == "math" && attr == "inf") {
        // NOTE: do not use a literal like 1e999 here — MSVC rejects it with
        // "error C2177: constant too big" and the whole compiler fails to build.
        out = {0, std::numeric_limits<double>::infinity(), "", 0};
        return true;
    }
    if (mod == "math" && attr == "tau") { out = {0, 6.28318530717958647692, "", 0}; return true; }
    if (mod == "math" && attr == "nan") {
        out = {0, std::numeric_limits<double>::quiet_NaN(), "", 0};
        return true;
    }
    if (mod == "sys" && attr == "maxsize") { out = {2, 0, "", 9223372036854775807LL}; return true; }
    if (mod == "sys" && attr == "platform") {
#ifdef _WIN32
        out = {1, 0, "win32", 0};
#elif defined(__APPLE__)
        out = {1, 0, "darwin", 0};
#else
        out = {1, 0, "linux", 0};
#endif
        return true;
    }
    if (mod == "sys" && (attr == "maxint")) { out = {2, 0, "", 9223372036854775807LL}; return true; }
    return false;
}

struct FuncEntry {
    Stmt* def;
    int index;
    int64_t global;
};

struct ClassEntry {
    Stmt* def;
    int64_t global;
    std::unordered_map<std::string, Stmt*> methods;
};

struct Sema {
    Module& mod;
    std::unordered_map<std::string, FuncEntry> funcs;
    std::unordered_map<std::string, ClassEntry> classes;
    std::unordered_map<std::string, int64_t> globals;
    std::unordered_map<std::string, std::string> imports;      // alias → native module
    std::unordered_map<std::string, BuiltinSig> from_imports;  // name → builtin
    std::unordered_map<std::string, ModConst> from_consts;     // name → const value
    std::unordered_map<std::string, std::string> user_imports; // alias → translated module
    // "from re import match as m" → {"m": "re__match"}: the alias refers to a
    // namespaced global of a translated module.
    std::unordered_map<std::string, std::string> user_renames;

    int try_depth = 0; // imports inside try/except may fail softly
    Stmt* cur_func = nullptr;
    std::unordered_map<std::string, int64_t> locals;
    std::set<std::string> global_decls; // 'global x' names in current function

    // closure support: capture state of the CURRENT function + a stack of
    // enclosing frames (only the immediately-enclosing one is consulted).
    bool cur_is_closure = false;
    std::unordered_map<std::string, int64_t> cur_capture_map; // name -> capture idx
    std::vector<std::pair<int, int64_t>> cur_capture_src;     // (kind,idx) in enclosing terms
    struct Frame {
        Stmt* def;
        std::unordered_map<std::string, int64_t> locals;
        std::set<std::string> global_decls;
        bool is_closure;
        std::unordered_map<std::string, int64_t> capture_map;
        std::vector<std::pair<int, int64_t>> capture_src;
    };
    std::vector<Frame> encl;
    int synth_counter = 0;

    explicit Sema(Module& m) : mod(m) {}

    int64_t add_capture(const std::string& name, int kind, int64_t idx) {
        auto it = cur_capture_map.find(name);
        if (it != cur_capture_map.end()) return it->second;
        int64_t ci = (int64_t)cur_capture_src.size();
        cur_capture_map[name] = ci;
        cur_capture_src.push_back({kind, idx});
        return ci;
    }

    // Returns true and marks e as a capture if `name` can be captured from the
    // immediately-enclosing function frame.
    bool try_capture(Expr* e, const std::string& name) {
        if (encl.empty()) return false;
        Frame& E = encl.back();
        auto itl = E.locals.find(name);
        if (itl != E.locals.end() && !E.global_decls.count(name)) {
            e->res = Res::Capture;
            e->res_idx = add_capture(name, 1, itl->second);
            return true;
        }
        if (E.is_closure) {
            auto itc = E.capture_map.find(name);
            if (itc != E.capture_map.end()) {
                e->res = Res::Capture;
                e->res_idx = add_capture(name, 3, itc->second);
                return true;
            }
        }
        return false;
    }

    [[noreturn]] static void err(int line, const std::string& m) { throw CompileError(line, m); }

    // "re__findall" → "re.findall" for diagnostics: users never typed the
    // namespaced spelling, so they should never read it either.
    std::string pretty(const std::string& name) const {
        for (const auto& [modname, info] : mod.bundled) {
            if (name.compare(0, info.prefix.size(), info.prefix) == 0)
                return modname + "." + name.substr(info.prefix.size());
        }
        return name;
    }

    int64_t global_slot(const std::string& name) {
        auto it = globals.find(name);
        if (it != globals.end()) return it->second;
        int64_t idx = (int64_t)globals.size();
        globals.emplace(name, idx);
        return idx;
    }

    int64_t local_slot(const std::string& name) {
        auto it = locals.find(name);
        if (it != locals.end()) return it->second;
        int64_t idx = (int64_t)locals.size();
        locals.emplace(name, idx);
        return idx;
    }

    std::string module_list() const {
        std::string where;
        for (const auto& d : mod.search_path) where += (where.empty() ? "" : ", ") + d;
        return "math, time, random, threading, sys, doctest are built in; os, os.path, "
               "json, re, logging, socket, requests, string ship as Python source in "
               "stdlib/ — searched: " + (where.empty() ? "<nothing>" : where);
    }

    void register_import(Stmt* s) {
        const std::string& modname = s->name;
        if (noop_module(modname)) {
            for (auto& extra : s->body) register_import(extra.get());
            return;
        }
        if (mod.bundled.count(modname)) {
            // A .py module translated into this program by the bundler.
            user_imports[s->alias.empty() ? modname : s->alias] = modname;
            // "import os.path" also binds the package name "os".
            if (s->alias.empty()) {
                size_t dot = modname.find('.');
                if (dot != std::string::npos)
                    user_imports.emplace(modname.substr(0, dot), modname.substr(0, dot));
            }
            for (auto& extra : s->body) register_import(extra.get());
            return;
        }
        if (modname.find('.') != std::string::npos || !modules().count(modname)) {
            if (try_depth > 0) return; // try: import X / except ImportError: pass
            err(s->line, "unknown module '" + modname + "' (" + module_list() + ")");
        }
        imports[s->alias.empty() ? modname : s->alias] = modname;
        for (auto& extra : s->body) register_import(extra.get());
    }

    void register_from_import(Stmt* s) {
        const std::string& modname = s->name;
        // `from . import mod1, mod2` — each imported name is a sibling module.
        if (s->relative && modname.empty()) {
            for (auto& [n, alias] : s->import_names) {
                if (mod.bundled.count(n)) user_imports[alias] = n;
                else if (try_depth == 0 && !noop_module(n))
                    err(s->line, "cannot find local module '" + n +
                                     ".py' for 'from . import " + n + "'");
            }
            return;
        }
        if (s->star) {
            // `from X import *`: bring every module-level name of the translated
            // module into scope under its own spelling.
            auto sit = mod.bundled.find(modname);
            if (sit != mod.bundled.end())
                for (const auto& n : sit->second.exports)
                    user_renames[n] = sit->second.prefix + n;
            return;
        }
        if (noop_module(modname)) return; // names are annotation-only
        auto bit = mod.bundled.find(modname);
        if (bit != mod.bundled.end()) {
            for (auto& [n, alias] : s->import_names) {
                // "from os import path" — a submodule, not a name
                if (mod.bundled.count(modname + "." + n)) {
                    user_imports[alias] = modname + "." + n;
                    continue;
                }
                if (!bit->second.exports.count(n))
                    err(s->line, "cannot import name '" + n + "' from '" + modname + "' (" +
                                     bit->second.path + ")");
                user_renames[alias] = bit->second.prefix + n;
            }
            return;
        }
        if (modname.find('.') != std::string::npos || !modules().count(modname)) {
            if (try_depth > 0) return;
            err(s->line, "unknown module '" + modname + "' (" + module_list() + ")");
        }
        auto& tbl = modules().at(modname);
        for (auto& [n, alias] : s->import_names) {
            ModConst cv;
            if (module_const(modname, n, cv)) {
                from_consts[alias] = cv;
                continue;
            }
            auto it = tbl.find(n);
            if (it == tbl.end())
                err(s->line, "module '" + modname + "' has no name '" + n + "'");
            from_imports[alias] = it->second;
        }
    }

    // ---- pass A: collect module-level names ----
    void register_funcdef(Stmt* s, const std::string& symbol, bool bind_global) {
        s->alias = symbol; // codegen symbol name
        s->func_index = (int)mod.functions.size();
        mod.functions.push_back(s);
        if (bind_global) {
            s->global_idx = global_slot(s->name);
            funcs[s->name] = {s, s->func_index, s->global_idx};
        }
    }

    void collect_module() {
        for (auto& sp : mod.body) {
            Stmt* s = sp.get();
            switch (s->kind) {
            case StmtKind::FuncDef:
                if (funcs.count(s->name)) err(s->line, "function '" + s->name + "' redefined");
                register_funcdef(s, s->name, true);
                break;
            case StmtKind::ClassDef: {
                if (classes.count(s->name)) err(s->line, "class '" + s->name + "' redefined");
                s->global_idx = global_slot(s->name);
                mod.classes.push_back(s);
                ClassEntry ce;
                ce.def = s;
                ce.global = s->global_idx;
                for (auto& msp : s->body) {
                    if (msp->kind == StmtKind::FuncDef) {
                        Stmt* m = msp.get();
                        register_funcdef(m, s->name + "__" + m->name, false);
                        ce.methods[m->name] = m;
                    }
                }
                classes[s->name] = std::move(ce);
                break;
            }
            case StmtKind::Assign: global_slot(s->name); break;
            case StmtKind::MultiAssign:
                for (auto& t : s->targets)
                    if (t->kind == ExprKind::Name) global_slot(t->sval);
                break;
            case StmtKind::For:
                for (auto& n : s->params) global_slot(n);
                collect_assigned(s->body, true);
                collect_assigned(s->orelse, true);
                break;
            case StmtKind::If:
                collect_assigned(s->body, true);
                collect_assigned(s->orelse, true);
                break;
            case StmtKind::While:
                collect_assigned(s->body, true);
                collect_assigned(s->orelse, true);
                break;
            case StmtKind::Try:
                try_depth++;
                collect_assigned(s->body, true);
                try_depth--;
                for (auto& h : s->handlers) {
                    if (!h.as_name.empty()) global_slot(h.as_name);
                    collect_assigned(h.body, true);
                }
                collect_assigned(s->orelse, true);
                collect_assigned(s->final_body, true);
                break;
            case StmtKind::Import: register_import(s); break;
            case StmtKind::FromImport: register_from_import(s); break;
            default: break;
            }
        }
    }

    void collect_assigned(std::vector<StmtPtr>& body, bool as_globals) {
        auto slot = [&](const std::string& n) {
            if (as_globals) global_slot(n);
            else if (!global_decls.count(n)) local_slot(n);
        };
        for (auto& sp : body) {
            Stmt* s = sp.get();
            switch (s->kind) {
            case StmtKind::Assign: slot(s->name); break;
            case StmtKind::Import: register_import(s); break;
            case StmtKind::FromImport: register_from_import(s); break;
            case StmtKind::MultiAssign:
                for (auto& t : s->targets)
                    if (t->kind == ExprKind::Name) slot(t->sval);
                break;
            case StmtKind::For:
                for (auto& n : s->params) slot(n);
                collect_assigned(s->body, as_globals);
                collect_assigned(s->orelse, as_globals);
                break;
            case StmtKind::If:
                collect_assigned(s->body, as_globals);
                collect_assigned(s->orelse, as_globals);
                break;
            case StmtKind::While:
                collect_assigned(s->body, as_globals);
                collect_assigned(s->orelse, as_globals);
                break;
            case StmtKind::With:
                if (!s->name.empty()) slot(s->name);
                collect_assigned(s->body, as_globals);
                break;
            case StmtKind::Try:
                try_depth++;
                collect_assigned(s->body, as_globals);
                try_depth--;
                for (auto& h : s->handlers) {
                    if (!h.as_name.empty()) slot(h.as_name);
                    collect_assigned(h.body, as_globals);
                }
                collect_assigned(s->orelse, as_globals);
                collect_assigned(s->final_body, as_globals);
                break;
            case StmtKind::FuncDef:
                if (as_globals) global_slot(s->name);
                else if (!global_decls.count(s->name)) local_slot(s->name);
                break; // its body is a separate function scope
            case StmtKind::ClassDef:
                err(s->line, "classes may only be defined at module level");
            default: break;
            }
        }
    }

    void collect_global_decls(std::vector<StmtPtr>& body) {
        for (auto& sp : body) {
            Stmt* s = sp.get();
            if (s->kind == StmtKind::FuncDef) continue; // nested fn: its own scope
            if (s->kind == StmtKind::Global)
                for (auto& n : s->params) {
                    global_decls.insert(n);
                    global_slot(n);
                }
            collect_global_decls(s->body);
            collect_global_decls(s->orelse);
            collect_global_decls(s->final_body);
            for (auto& h : s->handlers) collect_global_decls(h.body);
        }
    }

    // ---- resolution ----
    void resolve_target_name(int& kind, int64_t& idx, const std::string& name) {
        if (cur_func && !global_decls.count(name)) {
            kind = 1;
            idx = local_slot(name);
        } else {
            kind = 2;
            idx = global_slot(name);
        }
    }

    void resolve_target(Stmt* s, const std::string& name) {
        int k;
        int64_t idx;
        resolve_target_name(k, idx, name);
        s->target_res = k == 1 ? Res::Local : Res::Global;
        s->target_idx = idx;
    }

    // "from json import dumps" binds the alias to a namespaced global; rewrite
    // the reference before ordinary resolution sees it. A local of the same name
    // still wins, exactly like a real module-level import.
    void apply_user_rename(Expr* e) {
        if (user_renames.empty()) return;
        if (cur_func && locals.count(e->sval) && !global_decls.count(e->sval)) return;
        auto it = user_renames.find(e->sval);
        if (it != user_renames.end()) e->sval = it->second;
    }

    void resolve_name(Expr* e) {
        apply_user_rename(e);
        const std::string& n = e->sval;
        if (cur_func && !global_decls.count(n)) {
            auto it = locals.find(n);
            if (it != locals.end()) {
                e->res = Res::Local;
                e->res_idx = it->second;
                return;
            }
        }
        if (cur_is_closure && cur_capture_map.count(n)) {
            e->res = Res::Capture;
            e->res_idx = cur_capture_map[n];
            return;
        }
        if (cur_func && try_capture(e, n)) return;
        auto g = globals.find(n);
        if (g != globals.end()) {
            e->res = Res::Global;
            e->res_idx = g->second;
            return;
        }
        auto fc = from_consts.find(n);
        if (fc != from_consts.end()) {
            apply_const(e, fc->second);
            return;
        }
        if (n == "__name__") {
            e->kind = ExprKind::StrLit;
            e->sval = "__main__";
            return;
        }
        if (n == "__file__") {
            e->kind = ExprKind::StrLit;
            e->sval = mod.source_path.empty() ? "<string>" : mod.source_path;
            return;
        }
        if (imports.count(n) || user_imports.count(n))
            err(e->line, "module '" + n + "' can only be used as '" + n + ".<name>'");
        auto fi2 = from_imports.find(n);
        if (fi2 != from_imports.end()) {
            e->res = Res::BuiltinFunc;
            e->res_idx = fi2->second.id;
            return; // first-class builtin reference
        }
        auto b2 = builtins().find(n);
        if (b2 != builtins().end()) {
            e->res = Res::BuiltinFunc;
            e->res_idx = b2->second.id;
            return;
        }
        err(e->line, "undefined variable '" + n + "'");
    }

    bool name_is_bound(const std::string& n) {
        if (cur_func && locals.count(n) && !global_decls.count(n)) return true;
        return globals.count(n) || funcs.count(n) || classes.count(n) ||
               builtins().count(n) || from_imports.count(n) || from_consts.count(n) ||
               imports.count(n) || user_imports.count(n) || user_renames.count(n);
    }

    // Rewrites call kwargs/defaults into plain positional args when the callee
    // signature is known (user function or class constructor).
    void apply_signature(Expr* e, Stmt* def, bool skip_self) {
        size_t nparams = def->params.size() - (skip_self ? 1 : 0);
        size_t ndefaults = def->defaults.size();
        size_t first_param = skip_self ? 1 : 0;
        std::vector<ExprPtr> final_args(nparams);
        if (e->args.size() > nparams)
            err(e->line, pretty(def->name) + "() takes " + std::to_string(nparams) +
                             " argument(s) but " + std::to_string(e->args.size()) +
                             " were given");
        for (size_t i = 0; i < e->args.size(); i++) final_args[i] = std::move(e->args[i]);
        for (auto& [kw, val] : e->kwargs) {
            bool found = false;
            for (size_t i = 0; i < nparams; i++) {
                if (def->params[first_param + i] == kw) {
                    if (final_args[i])
                        err(e->line,
                            pretty(def->name) + "() got multiple values for '" + kw + "'");
                    final_args[i] = std::move(val);
                    found = true;
                    break;
                }
            }
            if (!found)
                err(e->line,
                    pretty(def->name) + "() got an unexpected keyword argument '" + kw + "'");
        }
        e->kwargs.clear();
        // Trailing holes that have defaults are NOT passed: the callee's
        // prologue evaluates the default expression in its own (definition)
        // scope — this is what makes non-literal defaults like
        // `thread_type=ThreadType.USER` work correctly.
        size_t pass_n = nparams;
        while (pass_n > 0 && !final_args[pass_n - 1] && pass_n - 1 + ndefaults >= nparams)
            pass_n--;
        for (size_t i = 0; i < pass_n; i++) {
            if (final_args[i]) continue;
            size_t di = i + ndefaults;
            if (di >= nparams) { // default exists (defaults align to the tail)
                const Expr* d = def->defaults[di - nparams].get();
                if (!is_literal_default(d))
                    err(e->line, def->name + "(): parameter '" +
                                     def->params[first_param + i] +
                                     "' has a non-literal default and cannot be "
                                     "skipped when later arguments are given — pass "
                                     "it explicitly");
                final_args[i] = clone_literal(d);
            } else {
                err(e->line, pretty(def->name) + "() missing required argument '" +
                                 def->params[first_param + i] + "'");
            }
        }
        final_args.resize(pass_n);
        e->args = std::move(final_args);
    }

    static bool is_literal_default(const Expr* e) {
        switch (e->kind) {
        case ExprKind::IntLit:
        case ExprKind::FloatLit:
        case ExprKind::StrLit:
        case ExprKind::BoolLit:
        case ExprKind::NoneLit: return true;
        case ExprKind::Unary: return e->op == KUOP_NEG && is_literal_default(e->a.get());
        case ExprKind::ListLit: return e->args.empty();
        case ExprKind::MapLit: return e->pairs.empty();
        default: return false;
        }
    }

    static void apply_const(Expr* e, const ModConst& cv) {
        if (cv.kind == 1) {
            e->kind = ExprKind::StrLit;
            e->sval = cv.s;
        } else if (cv.kind == 2) {
            e->kind = ExprKind::IntLit;
            e->ival = cv.i;
        } else {
            e->kind = ExprKind::FloatLit;
            e->fval = cv.f;
        }
    }

    static ExprPtr clone_literal(const Expr* e) {
        auto c = std::make_unique<Expr>();
        c->kind = e->kind;
        c->line = e->line;
        c->ival = e->ival;
        c->fval = e->fval;
        c->sval = e->sval;
        c->op = e->op;
        c->res = e->res;
        c->res_idx = e->res_idx;
        if (e->a) c->a = clone_literal(e->a.get());
        if (e->b) c->b = clone_literal(e->b.get());
        for (auto& a : e->args) c->args.push_back(clone_literal(a.get()));
        return c;
    }

    // The dotted module a name chain denotes ("os", "os.path"), or "" when the
    // expression is not a module reference at all.
    std::string module_ref(const Expr* e) {
        if (e->kind == ExprKind::Name) {
            auto it = user_imports.find(e->sval);
            if (it != user_imports.end() && !name_shadowed(e->sval)) return it->second;
            return "";
        }
        if (e->kind == ExprKind::Attr) {
            std::string base = module_ref(e->a.get());
            if (base.empty()) return "";
            std::string dotted = base + "." + e->sval;
            if (mod.bundled.count(dotted)) return dotted;
            return "";
        }
        return "";
    }

    // "re.match" → the global "re__match" of the translated module.
    void bind_module_attr(Expr* e, const std::string& modname, const std::string& attr) {
        const BundledModule& info = mod.bundled.at(modname);
        if (!info.exports.count(attr))
            err(e->line, "module '" + modname + "' has no attribute '" + attr + "' (" +
                             info.path + ")");
        e->sval = info.prefix + attr;
    }

    // The only native module that takes keyword arguments is the no-op
    // doctest.testmod(verbose=..., ...): accept and discard them.
    bool rewrite_module_kwargs(Expr* e, const std::string& mod, const std::string& fn) {
        if (mod == "doctest" && fn == "testmod") {
            e->kwargs.clear();
            e->args.clear();
            return true;
        }
        return false;
    }

    void resolve_sorted_kwargs(Expr* e) {
        // sorted(iterable, key=?, reverse=?) → positional [iterable, key|None, reverse|False]
        ExprPtr key, reverse;
        for (auto& [kw, val] : e->kwargs) {
            if (kw == "key") key = std::move(val);
            else if (kw == "reverse") reverse = std::move(val);
            else err(e->line, "sorted() got an unexpected keyword argument '" + kw + "'");
        }
        e->kwargs.clear();
        auto none = [&]() {
            auto n = std::make_unique<Expr>();
            n->kind = ExprKind::NoneLit;
            n->line = e->line;
            return n;
        };
        auto fals = [&]() {
            auto n = std::make_unique<Expr>();
            n->kind = ExprKind::BoolLit;
            n->line = e->line;
            n->ival = 0;
            return n;
        };
        while (e->args.size() < 1) e->args.push_back(none());
        e->args.push_back(key ? std::move(key) : none());
        e->args.push_back(reverse ? std::move(reverse) : fals());
    }

    void resolve_print_kwargs(Expr* e) {
        ExprPtr sep, end;
        for (auto& [kw, val] : e->kwargs) {
            if (kw == "sep") sep = std::move(val);
            else if (kw == "end") end = std::move(val);
            else if (kw == "flush") continue; // accepted and ignored
            else err(e->line, "print() got an unexpected keyword argument '" + kw + "'");
        }
        e->kwargs.clear();
        if (!sep) {
            sep = std::make_unique<Expr>();
            sep->kind = ExprKind::StrLit;
            sep->line = e->line;
            sep->sval = " ";
        }
        if (!end) {
            end = std::make_unique<Expr>();
            end->kind = ExprKind::StrLit;
            end->line = e->line;
            end->sval = "\n";
        }
        std::vector<ExprPtr> na;
        na.push_back(std::move(sep));
        na.push_back(std::move(end));
        for (auto& a : e->args) na.push_back(std::move(a));
        e->args = std::move(na);
        e->a->res_idx = KB_PRINT_EX;
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
        case ExprKind::IfExp:
            resolve_expr(e->a.get());
            resolve_expr(e->b.get());
            resolve_expr(e->c.get());
            return;
        case ExprKind::Index:
            resolve_expr(e->a.get());
            resolve_expr(e->b.get());
            return;
        case ExprKind::Slice:
            resolve_expr(e->a.get());
            for (auto& p : e->args)
                if (p) resolve_expr(p.get());
            return;
        case ExprKind::Attr: {
            // Translated module: "re.DOTALL" → the global "re__DOTALL".
            std::string bmod = module_ref(e->a.get());
            if (!bmod.empty()) {
                if (mod.bundled.count(bmod + "." + e->sval))
                    err(e->line, "module '" + bmod + "." + e->sval +
                                     "' can only be used as '" + bmod + "." + e->sval +
                                     ".<name>'");
                std::string attr = e->sval;
                e->kind = ExprKind::Name;
                e->a.reset();
                bind_module_attr(e, bmod, attr);
                resolve_expr(e);
                return;
            }
            if (e->a->kind == ExprKind::Name && imports.count(e->a->sval) &&
                !name_shadowed(e->a->sval)) {
                const std::string& modname = imports.at(e->a->sval);
                ModConst cv;
                if (module_const(modname, e->sval, cv)) {
                    apply_const(e, cv);
                    e->a.reset();
                    return;
                }
                if (modname == "sys" && e->sval == "argv") {
                    // sys.argv → zero-arg builtin call
                    e->kind = ExprKind::Call;
                    auto callee = std::make_unique<Expr>();
                    callee->kind = ExprKind::Name;
                    callee->line = e->line;
                    callee->sval = "sys.argv";
                    callee->res = Res::BuiltinFunc;
                    callee->res_idx = KB_SYS_ARGV;
                    e->a = std::move(callee);
                    e->sval.clear();
                    return;
                }
                err(e->line, "module '" + modname + "' has no constant '" + e->sval + "'");
            }
            resolve_expr(e->a.get()); // generic attribute access (objects/classes)
            return;
        }
        case ExprKind::Call: {
            // isinstance(x, int) / isinstance(x, (int, float)): type names are
            // rewritten to string literals before normal name resolution.
            if (e->a->kind == ExprKind::Name && e->a->sval == "isinstance" &&
                !name_shadowed("isinstance") && e->args.size() == 2) {
                auto rewrite_type = [&](ExprPtr& t) {
                    static const std::set<std::string> tn = {"int",  "float", "str",
                                                             "bool", "list",  "dict",
                                                             "tuple"};
                    if (t->kind == ExprKind::Name && tn.count(t->sval) &&
                        !name_shadowed(t->sval)) {
                        t->kind = ExprKind::StrLit;
                    }
                };
                if (e->args[1]->kind == ExprKind::ListLit) {
                    for (auto& t : e->args[1]->args) rewrite_type(t);
                } else {
                    rewrite_type(e->args[1]);
                }
            }
            Expr* callee = e->a.get();
            if (callee->kind == ExprKind::Name) apply_user_rename(callee);
            auto resolve_args = [&]() {
                for (auto& a : e->args) resolve_expr(a.get());
                for (auto& kv : e->kwargs) resolve_expr(kv.second.get());
            };
            if (callee->kind == ExprKind::Name) {
                const std::string& n = callee->sval;
                bool local_shadow = cur_func && locals.count(n) && !global_decls.count(n);
                if (!local_shadow) {
                    auto f = funcs.find(n);
                    if (f != funcs.end()) {
                        if (!f->second.def->decorators.empty()) {
                            // decorated: must call the (possibly wrapped) global value
                            resolve_args();
                            callee->res = Res::Global;
                            callee->res_idx = f->second.global;
                            return;
                        }
                        apply_signature(e, f->second.def, false);
                        resolve_args();
                        callee->res = Res::UserFunc;
                        callee->res_idx = f->second.index;
                        return;
                    }
                    auto c = classes.find(n);
                    if (c != classes.end()) {
                        auto init = c->second.methods.find("__init__");
                        if (init != c->second.methods.end())
                            apply_signature(e, init->second, true);
                        else if (!e->args.empty() || !e->kwargs.empty())
                            err(e->line, n + "() takes no arguments");
                        resolve_args();
                        callee->res = Res::Global;
                        callee->res_idx = c->second.global;
                        return; // dynamic call: runtime instantiates
                    }
                    auto fi = from_imports.find(n);
                    if (fi != from_imports.end() && !globals.count(n)) {
                        resolve_args();
                        check_builtin_call(e, n, fi->second);
                        callee->res = Res::BuiltinFunc;
                        callee->res_idx = fi->second.id;
                        return;
                    }
                    auto b = builtins().find(n);
                    if (b != builtins().end() && !globals.count(n)) {
                        resolve_args();
                        check_builtin_call(e, n, b->second);
                        callee->res = Res::BuiltinFunc;
                        callee->res_idx = b->second.id;
                        if (n == "print" && !e->kwargs.empty()) resolve_print_kwargs(e);
                        else if (n == "sorted" && !e->kwargs.empty()) resolve_sorted_kwargs(e);
                        else if (!e->kwargs.empty())
                            err(e->line, n + "() does not accept keyword arguments");
                        return;
                    }
                }
            }
            resolve_args();
            if (!e->kwargs.empty())
                err(e->line, "keyword arguments are only supported when calling functions "
                             "and classes defined in this file");
            resolve_expr(callee);
            return;
        }
        case ExprKind::MethodCall: {
            // Translated module: "re.findall(a, b)" → "re__findall(a, b)".
            // Re-resolving as a plain Call keeps kwargs/default arguments working.
            std::string bmod = module_ref(e->a.get());
            if (!bmod.empty()) {
                auto callee = std::make_unique<Expr>();
                callee->kind = ExprKind::Name;
                callee->line = e->line;
                callee->sval = e->sval;
                bind_module_attr(callee.get(), bmod, e->sval);
                e->kind = ExprKind::Call;
                e->a = std::move(callee);
                e->sval.clear();
                resolve_expr(e);
                return;
            }
            for (auto& a : e->args) resolve_expr(a.get());
            for (auto& kv : e->kwargs) resolve_expr(kv.second.get());
            if (e->a->kind == ExprKind::Name && imports.count(e->a->sval) &&
                !name_shadowed(e->a->sval)) {
                const std::string& modname = imports.at(e->a->sval);
                auto& mm = modules().at(modname);
                auto it = mm.find(e->sval);
                if (it == mm.end())
                    err(e->line, "module '" + modname + "' has no function '" + e->sval + "'");
                BuiltinSig sig = it->second;
                if ((int)e->args.size() < sig.min_args || (int)e->args.size() > sig.max_args)
                    err(e->line, modname + "." + e->sval + "() got " +
                                     std::to_string(e->args.size()) + " argument(s)");
                if (!e->kwargs.empty() && !rewrite_module_kwargs(e, modname, e->sval))
                    err(e->line, modname + "." + e->sval +
                                     "() does not accept keyword arguments");
                e->kind = ExprKind::Call;
                auto callee = std::make_unique<Expr>();
                callee->kind = ExprKind::Name;
                callee->line = e->line;
                callee->sval = modname + "." + e->sval;
                callee->res = Res::BuiltinFunc;
                callee->res_idx = sig.id;
                e->a = std::move(callee);
                return;
            }
            if (!e->kwargs.empty()) {
                // ClassName.method(self, x=..) — signature known at compile time
                bool local_shadow = cur_func && locals.count(e->a->sval) &&
                                    !global_decls.count(e->a->sval);
                if (e->a->kind == ExprKind::Name && classes.count(e->a->sval) &&
                    !local_shadow) {
                    auto& ce = classes.at(e->a->sval);
                    auto mit = ce.methods.find(e->sval);
                    if (mit == ce.methods.end())
                        err(e->line, "class '" + e->a->sval + "' has no method '" + e->sval +
                                         "'");
                    apply_signature(e, mit->second, false);
                } else {
                    // Dynamic receiver (e.g. a module that soft-failed to import
                    // inside try/except, or an arbitrary object). Don't hard-fail
                    // the whole build: defer to a catchable runtime error so
                    // guarded code paths still compile.
                    e->kind = ExprKind::Call;
                    e->args.clear();
                    e->kwargs.clear();
                    auto callee = std::make_unique<Expr>();
                    callee->kind = ExprKind::Name;
                    callee->line = e->line;
                    callee->sval = "kwargs-unsupported";
                    callee->res = Res::BuiltinFunc;
                    callee->res_idx = KB_KWARGS_UNSUPPORTED;
                    e->a = std::move(callee);
                    e->sval.clear();
                    return;
                }
            }
            resolve_expr(e->a.get());
            return;
        }
        case ExprKind::ListLit:
        case ExprKind::SetLit:
            for (auto& a : e->args) resolve_expr(a.get());
            return;
        case ExprKind::Lambda: {
            Stmt* fn = make_lambda_func(e);
            resolve_function(fn, /*as_closure=*/true);
            e->kind = ExprKind::Closure;
            e->res_idx = fn->func_index;
            e->params.clear();
            e->a.reset();
            return;
        }
        case ExprKind::Closure:
            return;
        case ExprKind::MapLit:
            for (auto& p : e->pairs) {
                resolve_expr(p.first.get());
                resolve_expr(p.second.get());
            }
            return;
        case ExprKind::Starred:
            resolve_expr(e->a.get());
            return;
        case ExprKind::ListComp:
        case ExprKind::SetComp:
        case ExprKind::MapComp: {
            // Clauses are evaluated left-to-right; each clause's iterable sees
            // targets bound by earlier clauses.
            for (auto& cl : e->clauses) {
                resolve_expr(cl.iter.get());
                for (auto& n : cl.targets) {
                    int k;
                    int64_t idx;
                    resolve_target_name(k, idx, n);
                    cl.tkind.push_back(k);
                    cl.tidx.push_back(idx);
                }
                for (auto& c : cl.conds) resolve_expr(c.get());
            }
            if (e->kind == ExprKind::MapComp) {
                resolve_expr(e->pairs[0].first.get());
                resolve_expr(e->pairs[0].second.get());
            } else {
                resolve_expr(e->a.get());
            }
            return;
        }
        }
    }

    bool name_shadowed(const std::string& n) {
        return (cur_func && locals.count(n) && !global_decls.count(n)) || globals.count(n);
    }

    void check_builtin_call(Expr* e, const std::string& n, const BuiltinSig& sig) {
        int total = (int)e->args.size();
        if (total < sig.min_args || total > sig.max_args)
            err(e->line, n + "() got " + std::to_string(total) + " argument(s)");
    }

    // ---- constant folding ----
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
            if (fb == 0.0) return;
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
        case KOP_POW:
            if (both_int && ib >= 0 && ib < 63) {
                int64_t r = 1;
                for (int64_t i = 0; i < ib; i++) r *= ia;
                set_int(r);
            } else {
                set_float(std::pow(fa, fb));
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
        case StmtKind::AttrAssign:
            resolve_expr(s->e1.get());
            resolve_expr(s->e3.get());
            return;
        case StmtKind::MultiAssign: {
            for (auto& v : s->values) resolve_expr(v.get());
            for (auto& t : s->targets) {
                if (t->kind == ExprKind::Name) {
                    int k;
                    int64_t idx;
                    resolve_target_name(k, idx, t->sval);
                    t->res = k == 1 ? Res::Local : Res::Global;
                    t->res_idx = idx;
                } else if (t->kind == ExprKind::Index) {
                    resolve_expr(t->a.get());
                    resolve_expr(t->b.get());
                } else if (t->kind == ExprKind::Attr) {
                    resolve_expr(t->a.get());
                }
            }
            return;
        }
        case StmtKind::If:
            resolve_expr(s->e1.get());
            resolve_stmts(s->body);
            resolve_stmts(s->orelse);
            return;
        case StmtKind::While:
            resolve_expr(s->e1.get());
            resolve_stmts(s->body);
            resolve_stmts(s->orelse);
            return;
        case StmtKind::For: {
            resolve_expr(s->e1.get());
            for (auto& n : s->params) {
                int k;
                int64_t idx;
                resolve_target_name(k, idx, n);
                s->multi_tkind.push_back(k);
                s->multi_tidx.push_back(idx);
            }
            s->target_res = s->multi_tkind[0] == 1 ? Res::Local : Res::Global;
            s->target_idx = s->multi_tidx[0];
            resolve_stmts(s->body);
            resolve_stmts(s->orelse);
            return;
        }
        case StmtKind::FuncDef:
            if (cur_func) {
                // nested function -> becomes a closure value bound to a local
                if (!s->decorators.empty())
                    err(s->line, "decorators on nested functions are not supported");
                s->alias = "nested_" + std::to_string(synth_counter++) + "_" + s->name;
                s->func_index = (int)mod.functions.size();
                s->global_idx = -1;
                mod.functions.push_back(s);
                resolve_function(s, /*as_closure=*/true);
                int k;
                int64_t idx;
                resolve_target_name(k, idx, s->name);
                s->target_res = k == 1 ? Res::Local : Res::Global;
                s->target_idx = idx;
            } else {
                resolve_function(s, /*as_closure=*/false);
                for (auto& d : s->decorators) resolve_expr(d.get());
            }
            return;
        case StmtKind::ClassDef:
            // resolve methods; class attribute assigns resolved as class-attr sets
            for (auto& msp : s->body) {
                if (msp->kind == StmtKind::FuncDef) {
                    Stmt* m = msp.get();
                    for (auto& d : m->decorators) {
                        if (!(d->kind == ExprKind::Name &&
                              (d->sval == "staticmethod" || d->sval == "classmethod" ||
                               d->sval == "abstractmethod" || d->sval == "property" ||
                               d->sval == "override")))
                            err(m->line, "unsupported method decorator (only "
                                         "@staticmethod/@classmethod/@property/@abstractmethod "
                                         "are recognized, and are treated as no-ops)");
                        if (d->sval == "staticmethod" || d->sval == "classmethod")
                            err(m->line, "@staticmethod/@classmethod are not supported yet");
                    }
                    resolve_function(m, /*as_closure=*/false);
                } else if (msp->kind == StmtKind::Assign) {
                    resolve_expr(msp->e1.get());
                }
            }
            for (auto& d : s->decorators) resolve_expr(d.get());
            if (!s->alias.empty() && !classes.count(s->alias))
                err(s->line, "unknown base class '" + s->alias + "'");
            return;
        case StmtKind::Return:
            if (s->e1) resolve_expr(s->e1.get());
            return;
        case StmtKind::Raise: {
            if (s->raise_mode == 1) return;
            if (s->raise_mode == 2) { // AssertionError from assert
                if (s->e1) resolve_expr(s->e1.get());
                return;
            }
            Expr* ex = s->e1.get();
            // raise ValueError("msg") / raise RuntimeError — error-type names are
            // not variables; recognize them structurally.
            if (ex->kind == ExprKind::Call && ex->a->kind == ExprKind::Name &&
                !name_is_bound(ex->a->sval)) {
                s->raise_mode = 2;
                s->name = ex->a->sval;
                if (ex->args.size() > 1)
                    err(s->line, "raise: at most one exception argument is supported");
                if (!ex->args.empty()) {
                    s->e1 = std::move(ex->args[0]);
                    resolve_expr(s->e1.get());
                } else {
                    s->e1.reset();
                }
                return;
            }
            if (ex->kind == ExprKind::Name && !name_is_bound(ex->sval)) {
                s->raise_mode = 2;
                s->name = ex->sval;
                s->e1.reset();
                return;
            }
            resolve_expr(ex);
            return;
        }
        case StmtKind::Try: {
            resolve_stmts(s->body);
            for (auto& h : s->handlers) {
                if (!h.as_name.empty()) {
                    int k;
                    int64_t idx;
                    resolve_target_name(k, idx, h.as_name);
                    h.as_kind = k;
                    h.as_idx = idx;
                }
                resolve_stmts(h.body);
            }
            resolve_stmts(s->orelse);
            resolve_stmts(s->final_body);
            return;
        }
        case StmtKind::Break:
        case StmtKind::Continue:
        case StmtKind::Pass:
        case StmtKind::Import:
        case StmtKind::FromImport:
        case StmtKind::Global: return;
        case StmtKind::Del:
            for (auto& t : s->targets) resolve_expr(t.get());
            return;
        case StmtKind::With: {
            resolve_expr(s->e1.get());
            if (!s->name.empty()) {
                int k;
                int64_t idx;
                resolve_target_name(k, idx, s->name);
                s->target_res = k == 1 ? Res::Local : Res::Global;
                s->target_idx = idx;
            }
            resolve_stmts(s->body);
            return;
        }
        }
    }

    void resolve_function(Stmt* s, bool as_closure) {
        // defaults are evaluated in the DEFINING (enclosing) scope
        for (auto& d : s->defaults) resolve_expr(d.get());
        // push current frame
        Frame f;
        f.def = cur_func;
        f.locals = std::move(locals);
        f.global_decls = std::move(global_decls);
        f.is_closure = cur_is_closure;
        f.capture_map = std::move(cur_capture_map);
        f.capture_src = std::move(cur_capture_src);
        encl.push_back(std::move(f));
        // new frame
        cur_func = s;
        locals.clear();
        global_decls.clear();
        cur_is_closure = as_closure;
        cur_capture_map.clear();
        cur_capture_src.clear();
        collect_global_decls(s->body);
        for (auto& p : s->params) {
            if (locals.count(p)) err(s->line, "duplicate parameter '" + p + "'");
            if (global_decls.count(p)) err(s->line, "parameter '" + p + "' declared global");
            local_slot(p);
        }
        collect_assigned(s->body, false);
        resolve_stmts(s->body);
        s->nlocals = (int)locals.size();
        s->is_closure = as_closure;
        s->ncaptures = (int)cur_capture_src.size();
        // export capture sources onto the Stmt for codegen (enclosing terms)
        s->multi_tkind.clear();
        s->multi_tidx.clear();
        for (auto& cs : cur_capture_src) {
            s->multi_tkind.push_back(cs.first);
            s->multi_tidx.push_back(cs.second);
        }
        // restore enclosing frame
        Frame& e = encl.back();
        cur_func = e.def;
        locals = std::move(e.locals);
        global_decls = std::move(e.global_decls);
        cur_is_closure = e.is_closure;
        cur_capture_map = std::move(e.capture_map);
        cur_capture_src = std::move(e.capture_src);
        encl.pop_back();
    }

    // Build a synthetic function for a lambda: body = 'return <expr>'.
    Stmt* make_lambda_func(Expr* lam) {
        auto fn = std::make_unique<Stmt>();
        fn->kind = StmtKind::FuncDef;
        fn->line = lam->line;
        fn->name = "<lambda>";
        fn->params = lam->params;
        fn->alias = "lambda_" + std::to_string(synth_counter++);
        auto ret = std::make_unique<Stmt>();
        ret->kind = StmtKind::Return;
        ret->line = lam->line;
        ret->e1 = std::move(lam->a);
        fn->body.push_back(std::move(ret));
        fn->func_index = (int)mod.functions.size();
        fn->global_idx = -1;
        Stmt* raw = fn.get();
        mod.functions.push_back(raw);
        mod.synth.push_back(std::move(fn));
        return raw;
    }
};

} // namespace

void analyze(Module& m) {
    Sema s(m);
    s.collect_module();
    for (auto& sp : m.body) s.resolve_stmt(sp.get());
    m.nglobals = (int64_t)s.globals.size();
}

bool known_builtin_module(const std::string& name) {
    return noop_module(name) || modules().count(name) != 0;
}


} // namespace kami
