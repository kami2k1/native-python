// The import layer: turns "import re" into translated Python source.
//
// KamiPython does not reimplement the standard library in C++ — it compiles the
// stdlib's Python source together with the user's program. This file finds that
// source, gives every module its own namespace, and splices it into the module
// being compiled (dependencies first) so the rest of the pipeline only ever
// sees one AST.
#include "modules.h"

#include "lexer.h"
#include "parser.h"
#include "sema.h"

#include "../../runtime/include/kami_runtime.h"

#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <set>
#include <sstream>
#include <unordered_map>

namespace fs = std::filesystem;

namespace kami {

namespace {

std::string read_file(const fs::path& path) {
    std::ifstream in(path, std::ios::binary);
    if (!in) throw std::runtime_error("cannot open " + path.string());
    std::ostringstream ss;
    ss << in.rdbuf();
    return ss.str();
}

// "os.path" → "os_path__": the namespace prefix every module-level name of that
// module is renamed with. '.' is not valid in a Python identifier, so a mangled
// name can only collide with a user global that already looks mangled.
std::string namespace_prefix(const std::string& module) {
    std::string s = module;
    for (char& c : s)
        if (c == '.') c = '_';
    return s + "__";
}

// ---------------------------------------------------------------- name sets

void collect_comp_targets(const Expr* e, std::set<std::string>& out);

void collect_comp_targets_list(const std::vector<ExprPtr>& es, std::set<std::string>& out) {
    for (const auto& e : es)
        if (e) collect_comp_targets(e.get(), out);
}

// Comprehension loop variables bind in the *enclosing* scope, so they belong to
// the same name set as ordinary assignments. Lambda bodies are their own scope.
void collect_comp_targets(const Expr* e, std::set<std::string>& out) {
    if (e->kind == ExprKind::Lambda) return;
    if (e->kind == ExprKind::ListComp || e->kind == ExprKind::SetComp ||
        e->kind == ExprKind::MapComp) {
        for (const auto& n : e->params) out.insert(n);
        for (const auto& c : e->clauses)
            for (const auto& n : c.targets) out.insert(n);
    }
    if (e->a) collect_comp_targets(e->a.get(), out);
    if (e->b) collect_comp_targets(e->b.get(), out);
    if (e->c) collect_comp_targets(e->c.get(), out);
    collect_comp_targets_list(e->args, out);
    for (const auto& kv : e->kwargs) collect_comp_targets(kv.second.get(), out);
    for (const auto& p : e->pairs) {
        if (p.first) collect_comp_targets(p.first.get(), out);
        if (p.second) collect_comp_targets(p.second.get(), out);
    }
    for (const auto& c : e->clauses) {
        if (c.iter) collect_comp_targets(c.iter.get(), out);
        collect_comp_targets_list(c.conds, out);
    }
}

// Every name bound by `body`, without descending into nested function bodies
// (their locals are a separate scope) or class bodies (those are attributes).
// `global_names` collects `global x` declarations, which bind module-level.
void collect_bindings(const std::vector<StmtPtr>& body, std::set<std::string>& out,
                      std::set<std::string>& global_names) {
    for (const auto& sp : body) {
        const Stmt* s = sp.get();
        switch (s->kind) {
        case StmtKind::Assign: out.insert(s->name); break;
        case StmtKind::MultiAssign:
            for (const auto& t : s->targets)
                if (t->kind == ExprKind::Name) out.insert(t->sval);
            break;
        case StmtKind::For:
            for (const auto& n : s->params) out.insert(n);
            break;
        case StmtKind::With:
            if (!s->name.empty()) out.insert(s->name);
            break;
        case StmtKind::FuncDef:
        case StmtKind::ClassDef: out.insert(s->name); break;
        case StmtKind::Global:
            for (const auto& n : s->params) global_names.insert(n);
            break;
        default: break;
        }
        // expressions can bind comprehension variables
        for (const Expr* e : {s->e1.get(), s->e2.get(), s->e3.get()})
            if (e) collect_comp_targets(e, out);
        collect_comp_targets_list(s->values, out);
        collect_comp_targets_list(s->targets, out);
        if (s->kind == StmtKind::FuncDef || s->kind == StmtKind::ClassDef) continue;
        collect_bindings(s->body, out, global_names);
        collect_bindings(s->orelse, out, global_names);
        collect_bindings(s->final_body, out, global_names);
        for (const auto& h : s->handlers) {
            if (!h.as_name.empty()) out.insert(h.as_name);
            collect_bindings(h.body, out, global_names);
        }
    }
}

// ---------------------------------------------------------------- renamer
// Rewrites every reference to a module-level name of the module being bundled
// into its namespaced spelling. Function-local names shadow module names, so
// each function scope pushes a "shield" set that suppresses renaming.

struct Renamer {
    const std::unordered_map<std::string, std::string>& names;
    std::vector<std::set<std::string>> shields;

    bool shielded(const std::string& n) const {
        for (const auto& s : shields)
            if (s.count(n)) return true;
        return false;
    }

    void rename(std::string& n) const {
        if (shielded(n)) return;
        auto it = names.find(n);
        if (it != names.end()) n = it->second;
    }

    void expr(Expr* e) {
        if (!e) return;
        switch (e->kind) {
        case ExprKind::Name: rename(e->sval); break;
        case ExprKind::Lambda: {
            std::set<std::string> scope(e->params.begin(), e->params.end());
            shields.push_back(std::move(scope));
            expr(e->a.get());
            shields.pop_back();
            return;
        }
        case ExprKind::ListComp:
        case ExprKind::SetComp:
        case ExprKind::MapComp:
            for (auto& n : e->params) rename(n);
            for (auto& c : e->clauses)
                for (auto& n : c.targets) rename(n);
            break;
        default: break;
        }
        // Attr / MethodCall keep sval as the *attribute* name — never renamed.
        expr(e->a.get());
        expr(e->b.get());
        expr(e->c.get());
        for (auto& a : e->args) expr(a.get());
        for (auto& kv : e->kwargs) expr(kv.second.get());
        for (auto& p : e->pairs) {
            expr(p.first.get());
            expr(p.second.get());
        }
        for (auto& c : e->clauses) {
            expr(c.iter.get());
            for (auto& cond : c.conds) expr(cond.get());
        }
    }

    void block(std::vector<StmtPtr>& body) {
        for (auto& sp : body) stmt(sp.get());
    }

    void function(Stmt* fn) {
        for (auto& d : fn->defaults) expr(d.get()); // evaluated in the outer scope
        std::set<std::string> scope(fn->params.begin(), fn->params.end());
        std::set<std::string> declared_global;
        collect_bindings(fn->body, scope, declared_global);
        for (const auto& g : declared_global) scope.erase(g);
        shields.push_back(std::move(scope));
        block(fn->body);
        shields.pop_back();
    }

    void stmt(Stmt* s) {
        switch (s->kind) {
        case StmtKind::Assign:
            rename(s->name);
            expr(s->e1.get());
            return;
        case StmtKind::For:
            for (auto& n : s->params) rename(n);
            expr(s->e1.get());
            block(s->body);
            return;
        case StmtKind::With:
            rename(s->name);
            expr(s->e1.get());
            block(s->body);
            return;
        case StmtKind::Global:
            for (auto& n : s->params) rename(n);
            return;
        case StmtKind::FuncDef:
            for (auto& d : s->decorators) expr(d.get());
            rename(s->name);
            function(s);
            return;
        case StmtKind::ClassDef:
            for (auto& d : s->decorators) expr(d.get());
            rename(s->name);
            rename(s->alias); // base class
            for (auto& msp : s->body) {
                if (msp->kind == StmtKind::FuncDef) function(msp.get());
                else expr(msp->e1.get()); // class attribute: value only
            }
            return;
        case StmtKind::Import:
        case StmtKind::FromImport: return; // module names live in their own namespace
        case StmtKind::Raise:
            // `raise ValueError("x")`: the type name is not a variable, but it is
            // also never a module-level binding, so the generic walk is safe.
            expr(s->e1.get());
            return;
        case StmtKind::Try:
            block(s->body);
            for (auto& h : s->handlers) {
                rename(h.as_name);
                block(h.body);
            }
            block(s->orelse);
            block(s->final_body);
            return;
        default: break;
        }
        expr(s->e1.get());
        expr(s->e2.get());
        expr(s->e3.get());
        for (auto& t : s->targets) expr(t.get());
        for (auto& v : s->values) expr(v.get());
        block(s->body);
        block(s->orelse);
        block(s->final_body);
        for (auto& h : s->handlers) block(h.body);
    }
};

// Each translated module owns a slice of the line-number space so that a
// diagnostic can be attributed to its source file after splicing.
const int kLineSpan = 1000000;

void shift_lines(Expr* e, int delta);

void shift_lines(Stmt* s, int delta) {
    if (!s) return;
    s->line += delta;
    for (Expr* e : {s->e1.get(), s->e2.get(), s->e3.get()}) shift_lines(e, delta);
    for (auto& x : s->defaults) shift_lines(x.get(), delta);
    for (auto& x : s->targets) shift_lines(x.get(), delta);
    for (auto& x : s->values) shift_lines(x.get(), delta);
    for (auto& x : s->decorators) shift_lines(x.get(), delta);
    for (auto& x : s->body) shift_lines(x.get(), delta);
    for (auto& x : s->orelse) shift_lines(x.get(), delta);
    for (auto& x : s->final_body) shift_lines(x.get(), delta);
    for (auto& h : s->handlers)
        for (auto& x : h.body) shift_lines(x.get(), delta);
}

void shift_lines(Expr* e, int delta) {
    if (!e) return;
    e->line += delta;
    shift_lines(e->a.get(), delta);
    shift_lines(e->b.get(), delta);
    shift_lines(e->c.get(), delta);
    for (auto& x : e->args) shift_lines(x.get(), delta);
    for (auto& kv : e->kwargs) shift_lines(kv.second.get(), delta);
    for (auto& p : e->pairs) {
        shift_lines(p.first.get(), delta);
        shift_lines(p.second.get(), delta);
    }
    for (auto& c : e->clauses) {
        shift_lines(c.iter.get(), delta);
        for (auto& cond : c.conds) shift_lines(cond.get(), delta);
    }
}

// ---------------------------------------------------------------- bundler

void collect_import_names(const Stmt* s, std::vector<std::string>& out) {
    switch (s->kind) {
    case StmtKind::Import:
        out.push_back(s->name);
        for (const auto& extra : s->body) collect_import_names(extra.get(), out);
        return;
    case StmtKind::FromImport:
        if (!s->name.empty()) out.push_back(s->name);
        // `from . import a, b` — the imported names are sibling modules.
        if (s->relative && s->name.empty())
            for (const auto& [n, alias] : s->import_names) out.push_back(n);
        return;
    default: break;
    }
    for (const auto& c : s->body) collect_import_names(c.get(), out);
    for (const auto& c : s->orelse) collect_import_names(c.get(), out);
    for (const auto& h : s->handlers)
        for (const auto& c : h.body) collect_import_names(c.get(), out);
    for (const auto& c : s->final_body) collect_import_names(c.get(), out);
}

// True for  if __name__ == "__main__": ...  — an imported module is not the main
// program, so its guard block is dropped (CPython semantics).
bool is_main_guard(const Stmt* s) {
    if (s->kind != StmtKind::If || !s->e1) return false;
    const Expr* c = s->e1.get();
    if (c->kind != ExprKind::Binary || c->op != KOP_EQ) return false;
    auto is_dunder_name = [](const Expr* x) {
        return x->kind == ExprKind::Name && x->sval == "__name__";
    };
    auto is_main_str = [](const Expr* x) {
        return x->kind == ExprKind::StrLit && x->sval == "__main__";
    };
    const Expr* l = c->a.get();
    const Expr* r = c->b.get();
    return (is_dunder_name(l) && is_main_str(r)) || (is_dunder_name(r) && is_main_str(l));
}

struct Bundler {
    std::vector<fs::path> dirs; // input directory first, then the stdlib
    std::set<std::string> loading;
    std::vector<StmtPtr> prelude;
    Module* mod = nullptr;

    // "pkg.mod" → <dir>/pkg/mod.py
    bool find_source(const std::string& name, fs::path& out) const {
        std::string rel = name;
        for (char& ch : rel)
            if (ch == '.') ch = '/';
        for (const fs::path& dir : dirs) {
            fs::path cand = dir / (rel + ".py");
            std::error_code ec;
            if (fs::exists(cand, ec)) {
                out = cand;
                return true;
            }
        }
        return false;
    }

    void scan(const std::vector<StmtPtr>& body) {
        std::vector<std::string> names;
        for (const auto& sp : body) collect_import_names(sp.get(), names);
        for (const auto& n : names) load(n);
    }

    void load(const std::string& name) {
        if (mod->bundled.count(name) || known_builtin_module(name)) return;
        fs::path file;
        if (!find_source(name, file)) return; // sema reports the unknown module
        if (loading.count(name))
            throw std::runtime_error("circular import involving module '" + name + "'");
        loading.insert(name);

        Module sub;
        try {
            sub = parse(lex(read_file(file)));
        } catch (const CompileError& e) {
            throw CompileError(e.line, e.what(), file.string());
        }
        scan(sub.body); // dependencies of the dependency come first

        BundledModule info;
        info.path = file.string();
        info.prefix = namespace_prefix(name);
        info.line_offset = kLineSpan * (int)(mod->bundled.size() + 1);
        for (auto& sp : sub.body) shift_lines(sp.get(), info.line_offset);
        std::set<std::string> global_decls;
        collect_bindings(sub.body, info.exports, global_decls);
        std::unordered_map<std::string, std::string> renames;
        for (const auto& n : info.exports) renames[n] = info.prefix + n;
        Renamer r{renames, {}};
        r.block(sub.body);

        for (auto& sp : sub.body) {
            if (is_main_guard(sp.get())) continue;
            prelude.push_back(std::move(sp));
        }
        loading.erase(name);
        mod->bundled.emplace(name, std::move(info));
    }
};

} // namespace

std::vector<std::string> stdlib_search_dirs(const std::string& argv0) {
    if (const char* env = getenv("KAMIPY_STDLIB")) {
        if (!fs::is_directory(env))
            throw std::runtime_error(std::string("KAMIPY_STDLIB is not a directory: ") + env);
        return {env};
    }
    fs::path dir;
#ifndef _WIN32
    std::error_code ec;
    fs::path exe = fs::read_symlink("/proc/self/exe", ec);
    if (!ec) dir = exe.parent_path();
#endif
    if (dir.empty()) {
        fs::path a(argv0);
        dir = a.has_parent_path() ? fs::absolute(a).parent_path() : fs::current_path();
    }
    fs::path up1 = dir.parent_path();
    fs::path up2 = up1.parent_path();
    std::vector<std::string> out;
    // Covers a build tree (build/kamipy, build/Release/kamipy.exe) and an
    // installed layout (<prefix>/bin/kamipy + <prefix>/lib/kamipy/stdlib).
    for (const fs::path& cand : {dir / "stdlib", up1 / "stdlib", up2 / "stdlib",
                                 up1 / "lib" / "kamipy" / "stdlib",
                                 up2 / "lib" / "kamipy" / "stdlib",
                                 up1 / "share" / "kamipy" / "stdlib"}) {
        std::error_code ec;
        if (fs::is_directory(cand, ec)) out.push_back(cand.string());
    }
    return out;
}

CompileError locate_error(const Module& mod, const CompileError& e) {
    if (!e.file.empty() || e.line < kLineSpan) return e;
    for (const auto& [name, info] : mod.bundled) {
        if (info.line_offset == 0) continue;
        if (e.line >= info.line_offset && e.line < info.line_offset + kLineSpan)
            return CompileError(e.line - info.line_offset, e.what(), info.path);
    }
    return e;
}

void bundle_modules(Module& mod, const std::string& input_path, const std::string& argv0) {
    Bundler b;
    b.mod = &mod;
    b.dirs.push_back(fs::absolute(input_path).parent_path());
    for (const auto& d : stdlib_search_dirs(argv0)) b.dirs.push_back(d);
    for (const auto& d : b.dirs) mod.search_path.push_back(d.string());
    b.scan(mod.body);
    if (b.prelude.empty()) return;
    std::vector<StmtPtr> merged;
    merged.reserve(b.prelude.size() + mod.body.size());
    for (auto& sp : b.prelude) merged.push_back(std::move(sp));
    for (auto& sp : mod.body) merged.push_back(std::move(sp));
    mod.body = std::move(merged);
}

} // namespace kami
