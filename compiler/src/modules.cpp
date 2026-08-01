#include "modules.h"

#include "cext.h"
#include "lexer.h"
#include "parser.h"
#include "sema.h"

#include "../../runtime/include/kami_runtime.h"

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <set>
#include <sstream>

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#endif

namespace fs = std::filesystem;

namespace kami {

namespace {

std::string read_source(const fs::path& p) {
    std::ifstream in(p, std::ios::binary);
    if (!in) throw std::runtime_error("cannot open " + p.string());
    std::ostringstream ss;
    ss << in.rdbuf();
    return ss.str();
}

// ---------------------------------------------------------------------------
// System Python discovery
// ---------------------------------------------------------------------------

// A directory is a plausible import root for the standard library if it holds
// one of the modules every CPython install has. Without this check a stray
// "C:\Python3-backup\Lib" or an empty prefix would poison the search path.
bool looks_like_stdlib(const fs::path& dir) {
    std::error_code ec;
    if (!fs::is_directory(dir, ec)) return false;
    for (const char* marker : {"os.py", "json/__init__.py", "types.py"})
        if (fs::exists(dir / marker, ec)) return true;
    return false;
}

// "python3.11" → 311, "Python313" → 313, "3.9" → 309. Bigger is newer, so the
// discovered roots can be ordered newest-first with a plain sort.
int version_rank(const std::string& s) {
    int major = -1, minor = 0;
    for (size_t i = 0; i < s.size(); i++) {
        if (!isdigit((unsigned char)s[i])) continue;
        major = s[i] - '0';
        size_t j = i + 1;
        if (j < s.size() && s[j] == '.') j++;
        std::string rest;
        while (j < s.size() && isdigit((unsigned char)s[j])) rest += s[j++];
        if (!rest.empty()) minor = atoi(rest.c_str());
        break;
    }
    if (major < 0) return 0;
    return major * 100 + minor;
}

struct Candidate {
    fs::path dir;
    int rank;      // Python version, 0 when unknown
    int priority;  // lower wins: explicit env beats a filesystem scan
};

void push_candidate(std::vector<Candidate>& out, const fs::path& dir, int priority,
                    bool require_marker) {
    std::error_code ec;
    if (dir.empty() || !fs::is_directory(dir, ec)) return;
    if (require_marker && !looks_like_stdlib(dir)) return;
    fs::path canon = fs::weakly_canonical(dir, ec);
    if (ec) canon = dir;
    for (const auto& c : out)
        if (c.dir == canon) return;
    out.push_back({canon, version_rank(canon.filename().string()), priority});
}

// Directory listing that never throws. Scanning C:\\ or /usr/lib can hit an
// unreadable entry, and std::filesystem's iterator increment throws unless the
// error is swallowed here.
std::vector<fs::path> list_dir(const fs::path& parent) {
    std::vector<fs::path> out;
    std::error_code ec;
    if (!fs::is_directory(parent, ec)) return out;
    fs::directory_iterator it(parent, fs::directory_options::skip_permission_denied, ec);
    if (ec) return out;
    for (const fs::directory_iterator end; it != end;) {
        out.push_back(it->path());
        it.increment(ec);
        if (ec) break;
    }
    return out;
}

// Split $PYTHONPATH-style lists (';' on Windows, ':' elsewhere).
std::vector<std::string> split_path_list(const std::string& s) {
#ifdef _WIN32
    const char sep = ';';
#else
    const char sep = ':';
#endif
    std::vector<std::string> out;
    std::string cur;
    for (char c : s) {
        if (c == sep) {
            if (!cur.empty()) out.push_back(cur);
            cur.clear();
        } else {
            cur += c;
        }
    }
    if (!cur.empty()) out.push_back(cur);
    return out;
}

// <prefix>/Lib on Windows, <prefix>/lib/python3.X elsewhere.
void push_prefix(std::vector<Candidate>& out, const fs::path& prefix, int priority) {
    push_candidate(out, prefix / "Lib", priority, true); // Windows layout
    for (const char* libdir : {"lib", "lib64"})
        for (const fs::path& e : list_dir(prefix / libdir))
            if (e.filename().string().rfind("python3", 0) == 0)
                push_candidate(out, e, priority, true);
}

// Every subdirectory of `parent` whose name starts with `prefix` (case
// insensitive) — our stand-in for the glob patterns "Python3*" / "python3.*".
void push_versioned_children(std::vector<Candidate>& out, const fs::path& parent,
                             const std::string& prefix, const fs::path& suffix, int priority) {
    for (const fs::path& e : list_dir(parent)) {
        std::string lower = e.filename().string();
        std::transform(lower.begin(), lower.end(), lower.begin(),
                       [](unsigned char c) { return (char)tolower(c); });
        if (lower.rfind(prefix, 0) != 0) continue;
        push_candidate(out, suffix.empty() ? e : e / suffix, priority, true);
    }
}

#ifdef _WIN32
// HKCU/HKLM \ Software \ Python \ PythonCore \ <version> \ InstallPath is what
// the official installer writes; it is the only reliable way to find a Python
// that was installed outside the default location.
void push_registry_pythons(std::vector<Candidate>& out, int priority) {
    struct Root {
        HKEY key;
        const char* subkey;
    };
    const Root roots[] = {
        {HKEY_CURRENT_USER, "Software\\Python\\PythonCore"},
        {HKEY_LOCAL_MACHINE, "Software\\Python\\PythonCore"},
        {HKEY_LOCAL_MACHINE, "Software\\Wow6432Node\\Python\\PythonCore"},
    };
    for (const Root& root : roots) {
        HKEY core;
        if (RegOpenKeyExA(root.key, root.subkey, 0, KEY_READ, &core) != ERROR_SUCCESS) continue;
        for (DWORD i = 0;; i++) {
            char ver[256];
            DWORD vlen = sizeof ver;
            if (RegEnumKeyExA(core, i, ver, &vlen, nullptr, nullptr, nullptr, nullptr) !=
                ERROR_SUCCESS)
                break;
            std::string path = std::string(ver) + "\\InstallPath";
            HKEY ip;
            if (RegOpenKeyExA(core, path.c_str(), 0, KEY_READ, &ip) != ERROR_SUCCESS) continue;
            char buf[MAX_PATH];
            DWORD blen = sizeof buf, type = 0;
            // The key's default (unnamed) value holds the install prefix, e.g.
            // "C:\\Users\\me\\AppData\\Local\\Programs\\Python\\Python313\\".
            if (RegQueryValueExA(ip, nullptr, nullptr, &type, (LPBYTE)buf, &blen) ==
                    ERROR_SUCCESS &&
                (type == REG_SZ || type == REG_EXPAND_SZ)) {
                std::string prefix(buf, blen > 0 ? blen - 1 : 0);
                while (!prefix.empty() && (prefix.back() == '\0' || prefix.back() == '\\'))
                    prefix.pop_back();
                if (!prefix.empty()) push_prefix(out, fs::path(prefix), priority);
            }
            RegCloseKey(ip);
        }
        RegCloseKey(core);
    }
}
#endif

// The interpreter on $PATH tells us about virtualenvs, pyenv and conda, which
// no amount of guessing at fixed install prefixes would ever find.
void push_pythons_on_path(std::vector<Candidate>& out, int priority) {
    const char* path = getenv("PATH");
    if (!path) return;
#ifdef _WIN32
    const char* names[] = {"python.exe", "python3.exe"};
#else
    const char* names[] = {"python3", "python"};
#endif
    std::error_code ec;
    for (const std::string& dir : split_path_list(path)) {
        for (const char* n : names) {
            fs::path exe = fs::path(dir) / n;
            if (!fs::exists(exe, ec)) continue;
            // <prefix>/bin/python3 → <prefix>;  <prefix>\python.exe → <prefix>
            fs::path prefix = fs::path(dir).filename() == "bin" ? fs::path(dir).parent_path()
                                                                : fs::path(dir);
            push_prefix(out, prefix, priority);
        }
    }
}

std::vector<fs::path> discover_system_python() {
    std::vector<Candidate> cands;

    // 1. explicit override — always wins.
    if (const char* env = getenv("KAMIPY_PYTHON_STDLIB"))
        for (const std::string& d : split_path_list(env)) push_candidate(cands, d, 0, false);
    // 2. PYTHONHOME names the prefix of an installation.
    if (const char* home = getenv("PYTHONHOME"))
        for (const std::string& d : split_path_list(home)) push_prefix(cands, d, 1);
    // 3. PYTHONPATH entries are import roots as-is (no stdlib marker required).
    if (const char* pp = getenv("PYTHONPATH"))
        for (const std::string& d : split_path_list(pp)) push_candidate(cands, d, 2, false);
    // 4. whatever interpreter the shell would run.
    push_pythons_on_path(cands, 3);

    // 5. platform scan.
#ifdef _WIN32
    push_registry_pythons(cands, 4);
    if (const char* lad = getenv("LOCALAPPDATA"))
        push_versioned_children(cands, fs::path(lad) / "Programs" / "Python", "python3", "Lib", 5);
    for (const char* drive : {"C:\\", "D:\\"}) {
        push_versioned_children(cands, drive, "python3", "Lib", 5);
        push_versioned_children(cands, fs::path(drive) / "Program Files", "python3", "Lib", 5);
        push_versioned_children(cands, fs::path(drive) / "Program Files (x86)", "python3", "Lib",
                                5);
    }
#else
    for (const char* prefix : {"/usr", "/usr/local", "/opt/homebrew", "/opt/local",
                               "/Library/Frameworks/Python.framework/Versions/Current"})
        push_prefix(cands, prefix, 5);
    // macOS framework builds keep one prefix per version.
    push_versioned_children(cands, "/Library/Frameworks/Python.framework/Versions", "3", "lib", 6);
    if (const char* home = getenv("HOME")) {
        push_versioned_children(cands, fs::path(home) / ".pyenv" / "versions", "3", "lib", 6);
        push_prefix(cands, fs::path(home) / ".local", 6);
    }
#endif

    // Best first: explicit sources before scans, newest Python before oldest.
    std::stable_sort(cands.begin(), cands.end(), [](const Candidate& a, const Candidate& b) {
        if (a.priority != b.priority) return a.priority < b.priority;
        return a.rank > b.rank;
    });
    std::vector<fs::path> out;
    out.reserve(cands.size());
    for (const auto& c : cands) out.push_back(c.dir);
    return out;
}

} // namespace

const std::vector<fs::path>& find_system_python_stdlib() {
    static const std::vector<fs::path> dirs = discover_system_python();
    return dirs;
}

std::vector<fs::path> bundled_pylib_dirs(const fs::path& bindir) {
    std::vector<fs::path> dirs;
    if (const char* env = getenv("KAMIPY_PYLIB")) dirs.push_back(env);
    dirs.push_back(bindir / "pylib");
    dirs.push_back(bindir.parent_path() / "pylib");
    dirs.push_back(bindir / ".." / "runtime" / "pylib");
    dirs.push_back(bindir.parent_path() / "runtime" / "pylib");
    return dirs;
}

// ---------------------------------------------------------------------------
// Resolution
// ---------------------------------------------------------------------------

namespace {

// "pkg.mod" → "pkg/mod": both a module file and a package directory are tried.
fs::path dotted_to_rel(const std::string& dotted) {
    std::string rel = dotted;
    for (char& ch : rel)
        if (ch == '.') ch = '/';
    return fs::path(rel);
}

bool find_in_dir(const fs::path& dir, const std::string& dotted, fs::path& out) {
    std::error_code ec;
    fs::path rel = dotted_to_rel(dotted);
    fs::path file = dir / rel;
    file += ".py";
    if (fs::is_regular_file(file, ec)) {
        out = file;
        return true;
    }
    fs::path init = dir / rel / "__init__.py";
    if (fs::is_regular_file(init, ec)) {
        out = init;
        return true;
    }
    return false;
}

} // namespace

std::vector<ResolvedModule> resolve_module(const std::string& dotted, const ImportPolicy& pol) {
    std::vector<ResolvedModule> out;
    ResolvedModule r;
    if (find_in_dir(pol.project_dir, dotted, r.path)) {
        r.origin = ModuleOrigin::Project;
        out.push_back(r); // a project file is authoritative: no fallbacks
        return out;
    }
    // A module the runtime already provides natively beats CPython's source:
    // `import json` stays a C-ABI primitive call instead of compiling the whole
    // json package.
    if (known_builtin_module(dotted) || is_cext_module(dotted) || is_ffi_module(dotted)) {
        out.push_back({ModuleOrigin::Native, {}});
        return out;
    }
    if (pol.use_system_python)
        for (const fs::path& dir : find_system_python_stdlib()) {
            ResolvedModule sys;
            if (find_in_dir(dir, dotted, sys.path)) {
                sys.origin = ModuleOrigin::SystemPython;
                out.push_back(sys);
                break; // the first (newest) installation wins
            }
        }
    for (const fs::path& dir : pol.pylib_dirs) {
        ResolvedModule lib;
        if (find_in_dir(dir, dotted, lib.path)) {
            lib.origin = ModuleOrigin::Pylib;
            out.push_back(lib);
            break;
        }
    }
    return out;
}

// ---------------------------------------------------------------------------
// Bundling
// ---------------------------------------------------------------------------

namespace {

void collect_import_names(Stmt* s, std::vector<std::string>& out) {
    switch (s->kind) {
    case StmtKind::Import:
        out.push_back(s->name);
        for (auto& extra : s->body) collect_import_names(extra.get(), out);
        return;
    case StmtKind::FromImport:
        if (!s->name.empty()) out.push_back(s->name);
        // `from . import a, b` — the imported names are sibling modules.
        if (s->relative && s->name.empty())
            for (auto& [n, alias] : s->import_names) out.push_back(n);
        return;
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
bool is_main_guard(const Stmt* s) {
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

struct Bundler {
    const ImportPolicy& pol;
    Module& mod;
    std::set<std::string> loading; // cycle guard
    std::vector<StmtPtr> prelude;

    explicit Bundler(Module& m, const ImportPolicy& p) : pol(p), mod(m) {}

    void process(std::vector<StmtPtr>& body) {
        std::vector<std::string> names;
        for (auto& sp : body) collect_import_names(sp.get(), names);
        for (auto& n : names) load(n);
    }

    static const char* origin_name(ModuleOrigin o) {
        switch (o) {
        case ModuleOrigin::Project: return "project";
        case ModuleOrigin::Pylib: return "bundled pylib";
        case ModuleOrigin::SystemPython: return "system Python";
        default: return "native";
        }
    }

    void load(const std::string& name) {
        if (mod.user_modules.count(name) || skipped.count(name)) return;
        std::vector<ResolvedModule> cands = resolve_module(name, pol);
        if (cands.empty()) return;                                  // sema reports it
        if (cands.front().origin == ModuleOrigin::Native) return;    // runtime primitive
        if (loading.count(name))
            throw std::runtime_error("circular import between local modules involving '" + name +
                                     "'");
        loading.insert(name);
        // Try each source in turn. CPython's own sources use far more syntax
        // than we compile, so a parse failure is not fatal while a fallback
        // remains: that is exactly what the shipped pylib is for.
        for (size_t i = 0; i < cands.size(); i++) {
            const ResolvedModule& found = cands[i];
            bool last = i + 1 == cands.size();
            Module sub;
            try {
                sub = parse(lex(read_source(found.path)));
            } catch (CompileError& e) {
                if (found.origin == ModuleOrigin::Project || last) {
                    loading.erase(name);
                    if (found.origin != ModuleOrigin::Project) {
                        // No fallback left. Say precisely what was rejected and
                        // carry on: the program may guard the import with
                        // try/except, and if not, sema names the module anyway.
                        fprintf(stderr, "kamipy: warning: skipping %s module '%s' (%s:%d: %s)\n",
                                origin_name(found.origin), name.c_str(),
                                found.path.string().c_str(), e.line, e.what());
                        skipped.insert(name);
                        return;
                    }
                    throw std::runtime_error(found.path.string() + ":" + std::to_string(e.line) +
                                             ": error: " + e.what());
                }
                if (pol.verbose)
                    fprintf(stderr, "kamipy: %s copy of '%s' is not compilable (%s:%d: %s) — "
                                    "falling back\n",
                            origin_name(found.origin), name.c_str(),
                            found.path.string().c_str(), e.line, e.what());
                continue;
            }
            if (pol.verbose)
                fprintf(stderr, "kamipy: import %s <- %s (%s)\n", name.c_str(),
                        found.path.string().c_str(), origin_name(found.origin));
            process(sub.body); // dependencies of the dependency come first
            for (auto& sp : sub.body) {
                if (is_main_guard(sp.get())) continue; // not the main program
                prelude.push_back(std::move(sp));
            }
            loading.erase(name);
            mod.user_modules.insert(name);
            if (found.origin == ModuleOrigin::SystemPython) mod.system_modules.insert(name);
            return;
        }
        loading.erase(name);
    }

    std::set<std::string> skipped;
};

} // namespace

void bundle_imported_modules(Module& mod, const ImportPolicy& pol) {
    Bundler b(mod, pol);
    b.process(mod.body);
    if (b.prelude.empty()) return;
    std::vector<StmtPtr> merged;
    merged.reserve(b.prelude.size() + mod.body.size());
    for (auto& sp : b.prelude) merged.push_back(std::move(sp));
    for (auto& sp : mod.body) merged.push_back(std::move(sp));
    mod.body = std::move(merged);
}

} // namespace kami
