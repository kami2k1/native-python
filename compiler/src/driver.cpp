#include "driver.h"

#include "codegen.h"
#include "lexer.h"
#include "parser.h"
#include "sema.h"

#include "../../runtime/include/kami_runtime.h"

#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <set>
#include <sstream>
#include <vector>

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <process.h>
#else
#include <sys/wait.h>
#include <unistd.h>
#endif

namespace fs = std::filesystem;

namespace kami {

int run_process(const std::vector<std::string>& args) {
#ifdef _WIN32
    // _spawnvp joins arguments with spaces and no quoting, so quote any
    // argument that contains whitespace (e.g. "C:\Program Files\LLVM\...").
    std::vector<std::string> quoted;
    quoted.reserve(args.size());
    for (const auto& a : args) {
        if (a.find_first_of(" \t") != std::string::npos && a.front() != '"')
            quoted.push_back("\"" + a + "\"");
        else
            quoted.push_back(a);
    }
    std::vector<const char*> argv;
    for (auto& a : quoted) argv.push_back(a.c_str());
    argv.push_back(nullptr);
    intptr_t rc = _spawnvp(_P_WAIT, args[0].c_str(), argv.data());
    return (int)rc;
#else
    std::vector<char*> argv;
    for (auto& a : args) argv.push_back(const_cast<char*>(a.c_str()));
    argv.push_back(nullptr);
    pid_t pid = fork();
    if (pid < 0) throw std::runtime_error("fork failed");
    if (pid == 0) {
        execvp(argv[0], argv.data());
        _exit(127);
    }
    int status = 0;
    waitpid(pid, &status, 0);
    if (WIFEXITED(status)) return WEXITSTATUS(status);
    return 128;
#endif
}

static fs::path self_dir(const std::string& argv0) {
#ifdef _WIN32
    // argv[0] is just "kamipy" when launched via PATH — ask the OS for the
    // real executable location so kamirt.lib is found from any directory.
    char buf[MAX_PATH];
    DWORD n = GetModuleFileNameA(nullptr, buf, MAX_PATH);
    if (n > 0 && n < MAX_PATH) return fs::path(buf).parent_path();
#else
    std::error_code ec;
    fs::path p = fs::read_symlink("/proc/self/exe", ec);
    if (!ec) return p.parent_path();
#endif
    fs::path a(argv0);
    if (a.has_parent_path()) return fs::absolute(a).parent_path();
    return fs::current_path();
}

std::string find_runtime_lib(const std::string& argv0) {
    if (const char* env = getenv("KAMIPY_RT_LIB")) {
        if (fs::exists(env)) return env;
        throw std::runtime_error(std::string("KAMIPY_RT_LIB points to a missing file: ") + env);
    }
#ifdef _WIN32
    const char* libname = "kamirt.lib";
#else
    const char* libname = "libkamirt.a";
#endif
    fs::path dir = self_dir(argv0);
    for (const fs::path& cand :
         {dir / libname, dir / "lib" / libname, dir.parent_path() / "lib" / libname,
          dir / "runtime" / libname}) {
        if (fs::exists(cand)) return cand.string();
    }
    throw std::runtime_error(
        std::string("cannot find ") + libname +
        " next to the kamipy binary (set KAMIPY_RT_LIB to override)");
}

static std::string read_file(const std::string& path) {
    std::ifstream in(path, std::ios::binary);
    if (!in) throw std::runtime_error("cannot open " + path);
    std::ostringstream ss;
    ss << in.rdbuf();
    return ss.str();
}

// ---- local module bundling -------------------------------------------------
// "import utils" where utils.py sits next to the input file: the module's
// source is parsed and its top-level statements are spliced in front of the
// main program (dependency-first), so the whole thing compiles into one
// native executable. Sema then resolves "utils.x" as plain "x".

static void collect_import_names(Stmt* s, std::vector<std::string>& out) {
    switch (s->kind) {
    case StmtKind::Import:
        out.push_back(s->name);
        for (auto& extra : s->body) collect_import_names(extra.get(), out);
        return;
    case StmtKind::FromImport: out.push_back(s->name); return;
    default: break;
    }
    for (auto& c : s->body) collect_import_names(c.get(), out);
    for (auto& c : s->orelse) collect_import_names(c.get(), out);
    for (auto& h : s->handlers)
        for (auto& c : h.body) collect_import_names(c.get(), out);
    for (auto& c : s->final_body) collect_import_names(c.get(), out);
}

// True for the idiom  if __name__ == "__main__": ...  — bundled modules are
// not the main program, so their guard blocks are dropped (CPython semantics).
static bool is_main_guard(const Stmt* s) {
    if (s->kind != StmtKind::If || !s->e1) return false;
    const Expr* c = s->e1.get();
    if (c->kind != ExprKind::Binary || c->op != KOP_EQ) return false;
    const Expr* l = c->a.get();
    const Expr* r = c->b.get();
    auto is_name_dunder = [](const Expr* x) {
        return x->kind == ExprKind::Name && x->sval == "__name__";
    };
    auto is_main_str = [](const Expr* x) {
        return x->kind == ExprKind::StrLit && x->sval == "__main__";
    };
    return (is_name_dunder(l) && is_main_str(r)) || (is_name_dunder(r) && is_main_str(l));
}

namespace {
struct Bundler {
    fs::path base_dir;
    std::set<std::string> loading; // cycle guard
    std::set<std::string> loaded;
    std::vector<StmtPtr> prelude;

    // "pkg.mod" → base_dir/pkg/mod.py
    fs::path module_path(const std::string& name) const {
        std::string rel = name;
        for (char& ch : rel)
            if (ch == '.') ch = '/';
        return base_dir / (rel + ".py");
    }

    void process(std::vector<StmtPtr>& body) {
        std::vector<std::string> names;
        for (auto& sp : body) collect_import_names(sp.get(), names);
        for (auto& n : names) load(n);
    }

    void load(const std::string& name) {
        if (loaded.count(name) || known_builtin_module(name)) return;
        fs::path file = module_path(name);
        std::error_code ec;
        if (!fs::exists(file, ec)) return; // sema reports unknown module (or soft-fails in try)
        if (loading.count(name))
            throw std::runtime_error("circular import between local modules involving '" +
                                     name + "'");
        loading.insert(name);
        Module sub;
        try {
            sub = parse(lex(read_file(file.string())));
        } catch (CompileError& e) {
            throw std::runtime_error(file.string() + ":" + std::to_string(e.line) +
                                     ": error: " + e.what());
        }
        process(sub.body); // dependencies of the dependency come first
        for (auto& sp : sub.body) {
            if (is_main_guard(sp.get())) continue; // not the main program
            prelude.push_back(std::move(sp));
        }
        loading.erase(name);
        loaded.insert(name);
    }
};
} // namespace

static void bundle_local_modules(Module& mod, const fs::path& input) {
    Bundler b;
    b.base_dir = fs::absolute(input).parent_path();
    b.process(mod.body);
    if (b.loaded.empty()) return;
    std::vector<StmtPtr> merged;
    merged.reserve(b.prelude.size() + mod.body.size());
    for (auto& sp : b.prelude) merged.push_back(std::move(sp));
    for (auto& sp : mod.body) merged.push_back(std::move(sp));
    mod.body = std::move(merged);
    mod.user_modules = std::move(b.loaded);
}

std::string build(const BuildOptions& opts) {
    std::string src = read_file(opts.input);

    Module mod = parse(lex(src));
    bundle_local_modules(mod, opts.input);
    analyze(mod);
    if (opts.emit_ast) {
        fputs(dump_module(mod).c_str(), stdout);
        return "";
    }
    std::string ir = codegen(mod, opts.input);

    fs::path out = opts.output.empty() ? fs::path(opts.input).stem() : fs::path(opts.output);
#ifdef _WIN32
    if (out.extension().empty()) out += ".exe";
#endif
    fs::path ll = out;
    ll += ".ll";
    {
        std::ofstream f(ll, std::ios::binary);
        if (!f) throw std::runtime_error("cannot write " + ll.string());
        f << ir;
    }

    std::string cxx = getenv("KAMIPY_CXX") ? getenv("KAMIPY_CXX") : "clang++";
    std::string rtlib = find_runtime_lib(opts.argv0);
    std::vector<std::string> cmd = {cxx,
                                    "-O" + std::to_string(opts.opt_level),
                                    ll.string(),
                                    rtlib,
                                    "-o",
                                    out.string()};
#ifndef _WIN32
    cmd.push_back("-pthread");
    cmd.push_back("-lm");
#ifdef KAMI_RT_NEEDS_OPENSSL
    cmd.push_back("-lssl");
    cmd.push_back("-lcrypto");
#endif
    cmd.push_back("-Wl,--gc-sections");
#else
    cmd.push_back("-lws2_32");
    cmd.push_back("-lwinhttp");
#endif
    // clang warns about override of module-less IR opt flags; keep output clean:
    cmd.push_back("-Wno-override-module");
    int rc = run_process(cmd);
    if (!opts.emit_llvm) {
        std::error_code ec;
        fs::remove(ll, ec);
    }
    if (rc != 0)
        throw std::runtime_error("linking failed (clang++ exited with code " +
                                 std::to_string(rc) + ")");
    return out.string();
}

} // namespace kami
