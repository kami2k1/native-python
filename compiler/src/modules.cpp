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

std::string path_to_utf8(const fs::path& p) {
#ifdef _WIN32
    std::wstring ws = p.wstring();
    if (ws.empty()) return "";
    int len = WideCharToMultiByte(CP_UTF8, 0, ws.c_str(), (int)ws.size(), NULL, 0, NULL, NULL);
    if (len <= 0) return "";
    std::string out(len, '\0');
    WideCharToMultiByte(CP_UTF8, 0, ws.c_str(), (int)ws.size(), &out[0], len, NULL, NULL);
    return out;
#else
    return p.string();
#endif
}

std::string read_source(const fs::path& p) {
    std::ifstream in(p, std::ios::binary);
    if (!in) throw std::runtime_error("cannot open " + path_to_utf8(p));
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
    out.push_back({canon, version_rank(path_to_utf8(canon.filename())), priority});
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
            if (path_to_utf8(e.filename()).rfind("python3", 0) == 0)
                push_candidate(out, e, priority, true);
}

// Every subdirectory of `parent` whose name starts with `prefix` (case
// insensitive) — our stand-in for the glob patterns "Python3*" / "python3.*".
void push_versioned_children(std::vector<Candidate>& out, const fs::path& parent,
                             const std::string& prefix, const fs::path& suffix, int priority) {
    for (const fs::path& e : list_dir(parent)) {
        std::string lower = path_to_utf8(e.filename());
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

namespace {

// site-packages / dist-packages directories that belong to the discovered
// stdlib roots. A stdlib root looks like:
//   Windows:  C:\...\Python312\Lib          → Lib\site-packages
//   POSIX:    /usr/lib/python3.11           → .../python3.11/site-packages,
//             plus Debian's dist-packages twins in /usr/local and /usr/lib.
std::vector<fs::path> discover_site_packages() {
    std::vector<fs::path> out;
    std::error_code ec;
    auto add = [&](const fs::path& d) {
        if (d.empty() || !fs::is_directory(d, ec)) return;
        fs::path canon = fs::weakly_canonical(d, ec);
        if (ec) canon = d;
        for (const auto& e : out)
            if (e == canon) return;
        out.push_back(canon);
    };
    if (const char* env = getenv("KAMIPY_SITE_PACKAGES"))
        for (const std::string& d : split_path_list(env)) add(d);
    for (const fs::path& lib : find_system_python_stdlib()) {
        add(lib / "site-packages");  // Windows + venvs
        add(lib / "dist-packages");  // Debian system python
        std::string base = path_to_utf8(lib.filename()); // "python3.11" / "Lib"
        if (base.rfind("python3", 0) == 0) {
            // Debian: pip installs into /usr/local/lib/python3.X/dist-packages
            add(fs::path("/usr/local/lib") / base / "dist-packages");
            add(fs::path("/usr/local/lib") / base / "site-packages");
            add(fs::path("/usr/lib/python3/dist-packages"));
            // user site: ~/.local/lib/python3.X/site-packages
            if (const char* home = getenv("HOME"))
                add(fs::path(home) / ".local" / "lib" / base / "site-packages");
        } else {
            // Windows user site: %APPDATA%\Python\Python3XX\site-packages
            std::string ver = path_to_utf8(lib.parent_path().filename()); // Python312
            if (const char* appdata = getenv("APPDATA"); appdata && !ver.empty())
                add(fs::path(appdata) / "Python" / ver / "site-packages");
        }
    }
    return out;
}

} // namespace

const std::vector<fs::path>& find_site_packages() {
    static const std::vector<fs::path> dirs = discover_site_packages();
    return dirs;
}

bool site_packages_has(const std::string& dotted) {
    std::error_code ec;
    std::string top = dotted.substr(0, dotted.find('.'));
    for (const fs::path& dir : find_site_packages()) {
        if (fs::is_regular_file(dir / (top + ".py"), ec)) return true;
        if (fs::is_directory(dir / top, ec) &&
            (fs::exists(dir / top / "__init__.py", ec) ||
             fs::exists(dir / top / "__init__.pyi", ec)))
            return true;
        // compiled C extension: top.cpython-311-x86_64-linux-gnu.so / top.pyd
        for (const fs::path& e : list_dir(dir)) {
            std::string fn = path_to_utf8(e.filename());
            if (fn.rfind(top + ".", 0) != 0) continue;
            if (fn.size() > 3 && (fn.compare(fn.size() - 3, 3, ".so") == 0 ||
                                  fn.compare(fn.size() - 4, 4, ".pyd") == 0))
                return true;
        }
    }
    return false;
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
    // Third-party packages (site-packages/dist-packages): pure-Python sources
    // are compile candidates; whatever cannot be compiled is bridged through
    // the embedded CPython interpreter instead (see sema's pyext fallback).
    if (pol.use_system_python)
        for (const fs::path& dir : find_site_packages()) {
            ResolvedModule sp;
            if (find_in_dir(dir, dotted, sp.path)) {
                sp.origin = ModuleOrigin::SitePackages;
                out.push_back(sp);
                break;
            }
        }
    return out;
}


// ---------------------------------------------------------------------------
// Symbol mangling for library modules
// ---------------------------------------------------------------------------
// A bundled module's statements are spliced into the program, so its top-level
// names land in the same global namespace as the user's. For a *library* module
// that is unacceptable: `logging.py` defines `error`, `info` and `root`, and any
// program that happens to assign to `error` would silently overwrite the
// library. So every top-level binding of a library module is renamed with a
// module prefix ("re" -> std_re_Pattern, std_re_search, ...) and sema maps
// `re.search` back onto the mangled symbol. Project files keep the flat
// namespace, which is what makes `from utils import helper` work unchanged.

namespace {

void collect_bindings(const std::vector<StmtPtr>& body, std::set<std::string>& out) {
    for (const auto& sp : body) {
        Stmt* s = sp.get();
        switch (s->kind) {
        case StmtKind::FuncDef:
        case StmtKind::ClassDef: out.insert(s->name); break;
        case StmtKind::Assign:
            if (!s->name.empty()) out.insert(s->name);
            break;
        case StmtKind::MultiAssign:
            for (auto& t : s->targets)
                if (t->kind == ExprKind::Name) out.insert(t->sval);
            break;
        case StmtKind::For:
            for (auto& p : s->params) out.insert(p);
            break;
        default: break;
        }
        // Module-level control flow can bind names too (`if X: A = 1`).
        if (s->kind == StmtKind::If || s->kind == StmtKind::While ||
            s->kind == StmtKind::For || s->kind == StmtKind::Try ||
            s->kind == StmtKind::With) {
            collect_bindings(s->body, out);
            collect_bindings(s->orelse, out);
            collect_bindings(s->final_body, out);
            for (auto& h : s->handlers) collect_bindings(h.body, out);
        }
    }
}

struct Mangler {
    const std::set<std::string>& names;
    const std::string& prefix;

    void rename(std::string& n) const {
        if (names.count(n)) n = prefix + n;
    }

    void expr(Expr* e) const {
        if (!e) return;
        // Attribute and method names are *not* renamed: `m.group()` must keep
        // calling `group`, because methods are dispatched by name at runtime.
        if (e->kind == ExprKind::Name) rename(e->sval);
        for (auto& p : e->params) rename(p);
        expr(e->a.get());
        expr(e->b.get());
        expr(e->c.get());
        for (auto& a : e->args) expr(a.get());
        for (auto& kv : e->kwargs) expr(kv.second.get());
        for (auto& pr : e->pairs) {
            expr(pr.first.get());
            expr(pr.second.get());
        }
        for (auto& c : e->clauses) {
            for (auto& t : c.targets) rename(t);
            expr(c.iter.get());
            for (auto& cond : c.conds) expr(cond.get());
        }
    }

    void stmts(std::vector<StmtPtr>& body, bool in_class = false) const {
        for (auto& sp : body) stmt(sp.get(), in_class);
    }

    void stmt(Stmt* s, bool in_class = false) const {
        if (!s) return;
        switch (s->kind) {
        case StmtKind::Import:
        case StmtKind::FromImport: return; // module paths are not identifiers
        case StmtKind::FuncDef:
            // A method keeps its own name; only module-level functions are
            // mangled.
            if (!in_class) rename(s->name);
            break;
        case StmtKind::ClassDef:
            rename(s->name);
            rename(s->alias); // base class
            break;
        case StmtKind::Assign:
        case StmtKind::For:
        case StmtKind::Global: rename(s->name); break;
        default: break;
        }
        if (s->kind == StmtKind::For || s->kind == StmtKind::Global ||
            s->kind == StmtKind::FuncDef)
            for (auto& p : s->params) rename(p);
        expr(s->e1.get());
        expr(s->e2.get());
        expr(s->e3.get());
        for (auto& d : s->defaults) expr(d.get());
        for (auto& d : s->decorators) expr(d.get());
        for (auto& t : s->targets) expr(t.get());
        for (auto& v : s->values) expr(v.get());
        stmts(s->body, s->kind == StmtKind::ClassDef);
        stmts(s->orelse);
        stmts(s->final_body);
        for (auto& h : s->handlers) {
            rename(h.as_name);
            stmts(h.body);
        }
    }
};

} // namespace

// Prefix for a library module: "re" -> "std_re_", "os.path" -> "std_os_path_".
std::string library_prefix(const std::string& dotted) {
    std::string prefix = "std_" + dotted + "_";
    for (char& ch : prefix)
        if (ch == '.') ch = '_';
    return prefix;
}

std::string mangled_library_name(const std::string& prefix, const std::string& name) {
    // must mirror mangle_module's dunder exclusion below
    if (name.size() > 4 && name.compare(0, 2, "__") == 0) return name;
    return prefix + name;
}

void mangle_module(std::vector<StmtPtr>& body, const std::string& prefix) {
    std::set<std::string> names;
    collect_bindings(body, names);
    // Dunders such as __all__ are provided or ignored by the compiler.
    for (auto it = names.begin(); it != names.end();) {
        if (it->size() > 4 && it->compare(0, 2, "__") == 0) it = names.erase(it);
        else ++it;
    }
    if (names.empty()) return;
    Mangler m{names, prefix};
    m.stmts(body);
}

// ---------------------------------------------------------------------------
// Bundling
// ---------------------------------------------------------------------------

namespace {

// Like collect_import_names, but ignores imports inside try/except: those are
// guarded and may legitimately fail at compile time.
void collect_unguarded_import_names(Stmt* s, std::vector<std::string>& out) {
    switch (s->kind) {
    case StmtKind::Import:
        out.push_back(s->name);
        for (auto& extra : s->body) collect_unguarded_import_names(extra.get(), out);
        return;
    case StmtKind::FromImport:
        if (!s->name.empty()) out.push_back(s->name);
        if (s->relative && s->name.empty())
            for (auto& [n, alias] : s->import_names) out.push_back(n);
        return;
    case StmtKind::Try:
        return; // guarded
    default: break;
    }
    for (auto& c : s->body) collect_unguarded_import_names(c.get(), out);
    for (auto& c : s->orelse) collect_unguarded_import_names(c.get(), out);
    for (auto& c : s->final_body) collect_unguarded_import_names(c.get(), out);
}

void collect_import_names(Stmt* s, std::vector<std::string>& out) {
    switch (s->kind) {
    case StmtKind::Import:
        out.push_back(s->name);
        for (auto& extra : s->body) collect_import_names(extra.get(), out);
        return;
    case StmtKind::FromImport:
        if (!s->name.empty()) {
            out.push_back(s->name);
            // `from pkg import sub` may name sibling MODULES, not symbols:
            // offer the dotted candidates too (unresolvable ones are ignored).
            for (auto& [n, alias] : s->import_names) out.push_back(s->name + "." + n);
        }
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

// The package that relative imports of module `dotted` (loaded from `path`)
// resolve against: the module itself for a package __init__, its parent
// otherwise ("concurrent.futures" for concurrent/futures/thread.py).
std::string package_of(const std::string& dotted, const fs::path& path) {
    if (path.filename() == "__init__.py") return dotted;
    size_t dot = dotted.rfind('.');
    return dot == std::string::npos ? std::string() : dotted.substr(0, dot);
}

// Rewrite single-dot relative imports into absolute ones so both the bundler
// and sema see plain dotted module names:
//   from .mod import X   →  from pkg.mod import X
//   from . import a, b   →  from pkg import a, b
void absolutize_relative_imports(std::vector<StmtPtr>& body, const std::string& pkg) {
    for (auto& sp : body) {
        Stmt* s = sp.get();
        if (s->kind == StmtKind::FromImport && s->relative && !pkg.empty()) {
            s->name = s->name.empty() ? pkg : pkg + "." + s->name;
            s->relative = false;
        }
        absolutize_relative_imports(s->body, pkg);
        absolutize_relative_imports(s->orelse, pkg);
        absolutize_relative_imports(s->final_body, pkg);
        for (auto& h : s->handlers) absolutize_relative_imports(h.body, pkg);
    }
}

void collect_from_import_stmts(std::vector<StmtPtr>& body, std::vector<Stmt*>& out) {
    for (auto& sp : body) {
        Stmt* s = sp.get();
        if (s->kind == StmtKind::FromImport && !s->name.empty() && !s->star)
            out.push_back(s);
        collect_from_import_stmts(s->body, out);
        collect_from_import_stmts(s->orelse, out);
        collect_from_import_stmts(s->final_body, out);
        for (auto& h : s->handlers) collect_from_import_stmts(h.body, out);
    }
}

// PEP 562 lazy loaders: CPython packages defer submodule imports with
//     def __getattr__(name):
//         if name == 'ThreadPoolExecutor':
//             from .thread import ThreadPoolExecutor as te
//             ...
// An AOT compiler has no module-attribute hook, so the deferred imports are
// hoisted to guarded top-level imports (try: from pkg.thread import ... )
// and the function is dropped. Modules that fail to compile (e.g.
// concurrent.futures.process → multiprocessing) are skipped like any other
// guarded import; the rest become real, eagerly-bundled definitions.
void hoist_lazy_getattr(Module& sub, const std::string& modname,
                        std::map<std::string, std::string>& exports) {
    for (size_t i = 0; i < sub.body.size(); i++) {
        Stmt* s = sub.body[i].get();
        if (s->kind != StmtKind::FuncDef || s->name != "__getattr__") continue;
        std::vector<Stmt*> lazy;
        collect_from_import_stmts(s->body, lazy);
        std::vector<StmtPtr> hoisted;
        for (Stmt* fi : lazy) {
            auto imp = std::make_unique<Stmt>();
            imp->kind = StmtKind::FromImport;
            imp->line = fi->line;
            imp->name = fi->name;
            // bind under the ORIGINAL names — `as pe` aliases exist only for
            // the function-local dance the hoisting replaces
            for (auto& [n, alias] : fi->import_names) {
                imp->import_names.emplace_back(n, n);
                exports[modname + "." + n] =
                    mangled_library_name(library_prefix(fi->name), n);
            }
            auto guard = std::make_unique<Stmt>();
            guard->kind = StmtKind::Try;
            guard->line = fi->line;
            guard->body.push_back(std::move(imp));
            ExceptClause h;
            auto pass = std::make_unique<Stmt>();
            pass->kind = StmtKind::Pass;
            pass->line = fi->line;
            h.body.push_back(std::move(pass));
            guard->handlers.push_back(std::move(h));
            hoisted.push_back(std::move(guard));
        }
        sub.body.erase(sub.body.begin() + (long)i);
        for (auto& g : hoisted) sub.body.insert(sub.body.begin() + (long)i++, std::move(g));
        return;
    }
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
        case ModuleOrigin::SitePackages: return "site-packages";
        default: return "native";
        }
    }

    void load(const std::string& name) {
        if (mod.user_modules.count(name) || skipped.count(name)) return;
        std::vector<ResolvedModule> cands = resolve_module(name, pol);
        if (cands.empty()) return;                                  // sema reports it
        if (cands.front().origin == ModuleOrigin::Native) return;    // runtime primitive
        if (loading.count(name)) {
            // Library packages import their own submodules while the package
            // __init__ is still loading (concurrent.futures ⇄ .thread): the
            // names bind lazily at runtime, so the cycle is benign. Only
            // project-file cycles are real errors.
            if (cands.front().origin == ModuleOrigin::Project)
                throw std::runtime_error(
                    "circular import between local modules involving '" + name + "'");
            return;
        }
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
                    if (found.origin == ModuleOrigin::SitePackages) {
                        // Third-party source we cannot compile: the import is
                        // bridged through the embedded CPython at runtime
                        // (sema's pyext fallback) — no warning needed.
                        if (pol.verbose)
                            fprintf(stderr,
                                    "kamipy: site-packages copy of '%s' is not compilable "
                                    "(%s:%d: %s) — bridging through CPython\n",
                                    name.c_str(), found.path.string().c_str(), e.line,
                                    e.what());
                        return;
                    }
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
            // Relative imports become absolute against the module's package,
            // and PEP 562 lazy loaders become guarded eager imports — both
            // BEFORE resolvability checks and dependency loading.
            {
                std::string pkg = package_of(name, found.path);
                absolutize_relative_imports(sub.body, pkg);
                if (found.origin != ModuleOrigin::Project)
                    hoist_lazy_getattr(sub, name, mod.module_exports);
            }
            // A library copy is only usable when its own (unguarded) imports
            // resolve too — CPython packages use relative imports we cannot
            // bundle (json/__init__.py -> .decoder). Fall back while we can.
            // site-packages sources additionally must probe clean even as the
            // last candidate: a third-party package that cannot be compiled
            // whole is bridged through the embedded CPython instead (sema
            // routes the import to kami_pyext_import).
            if ((!last || found.origin == ModuleOrigin::SitePackages) &&
                found.origin != ModuleOrigin::Project && !imports_resolvable(sub)) {
                if (pol.verbose)
                    fprintf(stderr,
                            "kamipy: %s copy of '%s' has unresolvable imports — "
                            "falling back\n",
                            origin_name(found.origin), name.c_str());
                continue;
            }
            if (pol.verbose)
                fprintf(stderr, "kamipy: import %s <- %s (%s)\n", name.c_str(),
                        found.path.string().c_str(), origin_name(found.origin));
            process(sub.body); // dependencies of the dependency come first
            if (found.origin != ModuleOrigin::Project) {
                // Library code shares the program's global namespace, so keep
                // its top-level names out of the user's way.
                std::string prefix = library_prefix(name);
                mangle_module(sub.body, prefix);
                mod.module_prefix[name] = prefix;
                // Record package re-exports: `from concurrent.futures._base
                // import Future` at the top of a package __init__ makes
                // `concurrent.futures.Future` refer to the _base symbol.
                for (auto& sp : sub.body) {
                    if (sp->kind != StmtKind::FromImport || sp->name.empty() || sp->star)
                        continue;
                    if (!mod.module_prefix.count(sp->name)) continue;
                    for (auto& [n, alias] : sp->import_names)
                        mod.module_exports[name + "." + alias] =
                            mangled_library_name(mod.module_prefix[sp->name], n);
                }
            }
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

    // Can every unguarded import of `sub` be satisfied somehow?
    bool imports_resolvable(Module& sub) {
        std::vector<std::string> names;
        for (auto& sp : sub.body) collect_unguarded_import_names(sp.get(), names);
        for (auto& n : names) {
            if (mod.user_modules.count(n) || skipped.count(n)) continue;
            if (pyext_module_name(n)) continue; // CPython C-extension bridge
            if (!probe(n)) return false;
        }
        return true;
    }

    // Dry-run: can module `name` (transitively) be bundled — i.e. does some
    // candidate parse AND have (unguarded) imports that probe true as well?
    // Without this, a package whose submodule fails deep in the chain
    // (json/__init__ → json.encoder) would be committed and then break,
    // instead of falling back to the next candidate (the bundled pylib).
    std::map<std::string, int> probe_cache; // 1 ok, -1 bad
    std::set<std::string> probing;          // cycles count as ok

    bool probe(const std::string& name) {
        if (mod.user_modules.count(name) || loading.count(name) || probing.count(name))
            return true;
        auto it = probe_cache.find(name);
        if (it != probe_cache.end()) return it->second > 0;
        probing.insert(name);
        bool ok = probe_uncached(name);
        probing.erase(name);
        probe_cache[name] = ok ? 1 : -1;
        return ok;
    }

    bool probe_uncached(const std::string& name) {
        std::vector<ResolvedModule> cands = resolve_module(name, pol);
        if (cands.empty()) return false;
        for (const ResolvedModule& cand : cands) {
            if (cand.origin == ModuleOrigin::Native) return true;
            Module sub;
            try {
                sub = parse(lex(read_source(cand.path)));
            } catch (CompileError&) {
                continue; // try the next candidate
            } catch (std::exception&) {
                continue;
            }
            std::string pkg = package_of(name, cand.path);
            absolutize_relative_imports(sub.body, pkg);
            if (cand.origin != ModuleOrigin::Project) {
                std::map<std::string, std::string> scratch_exports;
                hoist_lazy_getattr(sub, name, scratch_exports);
            }
            std::vector<std::string> names;
            for (auto& sp : sub.body) collect_unguarded_import_names(sp.get(), names);
            bool deps_ok = true;
            for (auto& n : names) {
                if (n == name) continue;
                if (skipped.count(n) || pyext_module_name(n)) continue;
                if (!probe(n)) {
                    deps_ok = false;
                    break;
                }
            }
            if (deps_ok) return true;
        }
        return false;
    }

    std::set<std::string> skipped;
};

} // namespace

void bundle_imported_modules(Module& mod, const ImportPolicy& pol) {
    // What the shipped pylib has to offer, so sema can name it when an import
    // cannot be resolved at all.
    std::error_code ec;
    for (const fs::path& dir : pol.pylib_dirs) {
        for (const fs::path& p : list_dir(dir)) {
            if (p.extension() == ".py") mod.stdlib_available.insert(p.stem().string());
            else if (fs::exists(p / "__init__.py", ec))
                mod.stdlib_available.insert(p.filename().string());
        }
    }
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
