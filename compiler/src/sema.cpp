#include "sema.h"

#include "cext.h"

#include "../../runtime/include/kami_builtins.h"
#include "../../runtime/include/kami_runtime.h"

#include <cmath>
#include <cstring>
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
        {"zip", {KB_ZIP, 1, 16}},     {"bool", {KB_BOOL, 1, 1}},
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
        {"repr", {KB_REPR, 1, 1}}, {"callable", {KB_CALLABLE, 1, 1}},
        {"next", {KB_NEXT, 1, 2}}, {"iter", {KB_ITER, 1, 1}},
        {"hasattr", {KB_HASATTR, 2, 2}}, {"getattr", {KB_GETATTR, 2, 3}},
        {"id", {KB_ID, 1, 1}}, {"object", {KB_OBJECT, 0, 0}},
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
                      {"perf_counter", {KB_TIME_PERF_COUNTER, 0, 0}},
                      {"localtime", {KB_TIME_LOCALTIME, 0, 1}},
                      {"gmtime", {KB_TIME_GMTIME, 0, 1}},
                      {"strftime", {KB_TIME_STRFTIME, 1, 2}}}},
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
             {{"spawn", {KB_THREAD_SPAWN, 1, 9}}, {"join", {KB_THREAD_JOIN, 1, 1}},
              {"Lock", {KB_THREAD_LOCK, 0, 0}}, {"RLock", {KB_THREAD_RLOCK, 0, 0}},
              {"Condition", {KB_THREAD_CONDITION, 0, 1}},
              {"Event", {KB_THREAD_EVENT, 0, 0}},
              {"Semaphore", {KB_THREAD_SEMAPHORE, 0, 1}},
              {"BoundedSemaphore", {KB_THREAD_SEMAPHORE, 0, 1}},
              {"Thread", {KB_THREAD_THREAD, 0, 4}},
              {"current_thread", {KB_THREAD_CURRENT, 0, 0}},
              {"_register_atexit", {KB_THREAD_ATEXIT, 1, 9}}}},
            {"collections",
             {{"namedtuple", {KB_NAMEDTUPLE, 2, 3}},
              {"deque", {KB_DEQUE, 0, 1}},
              {"OrderedDict", {KB_DICT, 0, 0}}}},
            {"weakref",
             {{"ref", {KB_WEAKREF_REF, 1, 2}},
              {"WeakKeyDictionary", {KB_DICT, 0, 0}},
              {"WeakValueDictionary", {KB_DICT, 0, 0}},
              {"WeakSet", {KB_SET, 0, 0}}}},
            {"sys", {{"exit", {KB_SYS_EXIT, 0, 1}}}},
            {"doctest", {{"testmod", {KB_NOOP, 0, 2}}}},
            {"string", {}},
            {"os",
             {{"getcwd", {KB_OS_GETCWD, 0, 0}}, {"listdir", {KB_OS_LISTDIR, 0, 1}},
              {"remove", {KB_OS_REMOVE, 1, 1}}, {"unlink", {KB_OS_REMOVE, 1, 1}},
              {"mkdir", {KB_OS_MKDIR, 1, 1}},   {"makedirs", {KB_OS_MAKEDIRS, 1, 2}},
              {"rmdir", {KB_OS_RMDIR, 1, 1}},   {"rename", {KB_OS_RENAME, 2, 2}},
              {"system", {KB_OS_SYSTEM, 1, 1}}, {"getenv", {KB_OS_GETENV, 1, 2}},
              {"walk", {KB_OS_WALK, 1, 1}},     {"chdir", {KB_OS_CHDIR, 1, 1}},
              {"getpid", {KB_OS_GETPID, 0, 0}}, {"urandom", {KB_OS_URANDOM, 1, 1}},
              {"cpu_count", {KB_OS_CPU_COUNT, 0, 0}},
              {"process_cpu_count", {KB_OS_CPU_COUNT, 0, 0}},
              {"register_at_fork", {KB_NOOP, 0, 3}}}},
            {"os.path",
             {{"exists", {KB_OSP_EXISTS, 1, 1}}, {"isfile", {KB_OSP_ISFILE, 1, 1}},
              {"isdir", {KB_OSP_ISDIR, 1, 1}},   {"join", {KB_OSP_JOIN, 1, 16}},
              {"basename", {KB_OSP_BASENAME, 1, 1}}, {"dirname", {KB_OSP_DIRNAME, 1, 1}},
              {"getsize", {KB_OSP_GETSIZE, 1, 1}}, {"abspath", {KB_OSP_ABSPATH, 1, 1}},
              {"expanduser", {KB_OSP_EXPANDUSER, 1, 1}},
              {"splitext", {KB_OSP_SPLITEXT, 1, 1}}, {"split", {KB_OSP_SPLIT, 1, 1}},
              {"isabs", {KB_OSP_ISABS, 1, 1}}}},
            {"socket", {{"socket", {KB_SOCKET_SOCKET, 0, 2}}}},
        };
    return m;
}

// Base classes like `class MyError(Exception)`: exceptions are string-matched
// at runtime, so inheriting from a builtin exception adds no behavior. Any
// unknown base that *looks like* a builtin exception is accepted and dropped.
static bool builtin_exception_name(const std::string& n) {
    static const std::set<std::string> exact = {
        "Exception", "BaseException", "StopIteration", "StopAsyncIteration",
        "GeneratorExit", "KeyboardInterrupt", "SystemExit",
    };
    if (exact.count(n)) return true;
    auto ends_with = [&](const char* suf) {
        size_t l = strlen(suf);
        return n.size() >= l && n.compare(n.size() - l, l, suf) == 0;
    };
    return ends_with("Error") || ends_with("Exception") || ends_with("Warning");
}

// Imports that are accepted and ignored (annotation-only / test helpers).
static bool noop_module(const std::string& name) {
    return name == "typing" || name == "__future__" || name == "abc" ||
           name == "dataclasses" || name == "collections.abc" || name == "types";
}

// CPython C extension modules bridged through the embedded C-API layer
// (pycapi.cpp): the compiled program loads the machine's real libpython at
// runtime and lets CPython's import machinery link the original .so/.pyd.
// Extend with KAMIPY_PYEXT=mod1,mod2 at compile time.
static bool pyext_module(const std::string& name) {
    static const std::set<std::string> known = {
        "_hashlib",  "_ssl",       "_sqlite3",     "zlib",     "_bz2",
        "_lzma",     "_zoneinfo",  "unicodedata",  "pyexpat",  "_elementtree",
        "_decimal",  "_json",      "_csv",         "_pickle",  "_datetime",
        "_multiprocessing", "select", "termios",   "readline", "_curses",
        "_uuid",     "_lsprof",    "audioop",      "_crypt",   "mmap",
    };
    if (known.count(name)) return true;
    if (const char* extra = getenv("KAMIPY_PYEXT")) {
        std::string s = extra;
        size_t p = 0;
        while (p <= s.size()) {
            size_t c = s.find(',', p);
            std::string tok = s.substr(p, c == std::string::npos ? c : c - p);
            if (tok == name) return true;
            if (c == std::string::npos) break;
            p = c + 1;
        }
    }
    return false;
}

// Modules whose members are C functions rather than Python ones: the C
// extensions CPython's own library is built on, plus the two FFI front ends.
static bool ffi_module(const std::string& name) { return is_ffi_module(name); }
static bool cabi_module(const std::string& name) {
    return is_cext_module(name) || ffi_module(name);
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
    if (mod == "os" && attr == "name") {
#ifdef _WIN32
        out = {1, 0, "nt", 0};
#else
        out = {1, 0, "posix", 0};
#endif
        return true;
    }
    if (mod == "os" && attr == "sep") {
#ifdef _WIN32
        out = {1, 0, "\\", 0};
#else
        out = {1, 0, "/", 0};
#endif
        return true;
    }
    if (mod == "os" && attr == "linesep") {
#ifdef _WIN32
        out = {1, 0, "\r\n", 0};
#else
        out = {1, 0, "\n", 0};
#endif
        return true;
    }
    if (mod == "socket") {
        if (attr == "AF_INET") { out = {2, 0, "", 2}; return true; }
        if (attr == "SOCK_STREAM") { out = {2, 0, "", 1}; return true; }
        if (attr == "SOCK_DGRAM") { out = {2, 0, "", 2}; return true; }
        if (attr == "SOL_SOCKET") { out = {2, 0, "", 1}; return true; }
        if (attr == "SO_REUSEADDR") { out = {2, 0, "", 2}; return true; }
    }
    if (mod == "string") {
        if (attr == "ascii_lowercase") { out = {1, 0, "abcdefghijklmnopqrstuvwxyz", 0}; return true; }
        if (attr == "ascii_uppercase") { out = {1, 0, "ABCDEFGHIJKLMNOPQRSTUVWXYZ", 0}; return true; }
        if (attr == "ascii_letters") {
            out = {1, 0, "abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ", 0};
            return true;
        }
        if (attr == "digits") { out = {1, 0, "0123456789", 0}; return true; }
        if (attr == "punctuation") {
            out = {1, 0, "!\"#$%&'()*+,-./:;<=>?@[\\]^_`{|}~", 0};
            return true;
        }
        if (attr == "whitespace") { out = {1, 0, " \t\n\r\x0b\x0c", 0}; return true; }
        if (attr == "printable") {
            out = {1, 0,
                   "0123456789abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ"
                   "!\"#$%&'()*+,-./:;<=>?@[\\]^_`{|}~ \t\n\r\x0b\x0c",
                   0};
            return true;
        }
        if (attr == "hexdigits") { out = {1, 0, "0123456789abcdefABCDEF", 0}; return true; }
        if (attr == "octdigits") { out = {1, 0, "01234567", 0}; return true; }
    }
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
    std::unordered_map<std::string, std::string> imports;      // alias → module
    std::unordered_map<std::string, BuiltinSig> from_imports;  // name → builtin
    std::unordered_map<std::string, ModConst> from_consts;     // name → const value
    std::unordered_map<std::string, std::string> user_imports; // alias → bundled local module
    // `from re import search` on a mangled pylib module: alias → mangled global.
    std::unordered_map<std::string, std::string> bundled_names;

    // ---- C-ABI binding state (see cext.h) ----
    std::unordered_map<std::string, CPrimitive> from_natives; // `from _math import sqrt`
    std::unordered_map<std::string, std::string> ffi_names;   // alias → "ctypes.CDLL", "cffi.FFI"
    std::unordered_map<std::string, std::string> clibs;       // alias → link library
    std::set<std::string> ffi_objects;                        // cffi FFI() instances
    std::unordered_map<std::string, std::string> cdef_sigs;   // cdef()'d prototypes
    std::unordered_map<std::string, char> cfunc_ret;          // "lib.fn" → restype code
    std::unordered_map<std::string, std::string> cfunc_args;  // "lib.fn" → argtype codes

    int try_depth = 0; // imports inside try/except may fail softly
    Stmt* cur_func = nullptr;
    const Stmt* cur_class = nullptr; // ClassDef whose methods are being resolved
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

    int64_t global_slot(const std::string& name) {
        auto it = globals.find(name);
        if (it != globals.end()) return it->second;
        int64_t idx = (int64_t)globals.size();
        globals.emplace(name, idx);
        return idx;
    }

    // Symbol prefix of a bundled module ("" for local .py files, which share
    // the program's flat namespace).
    const std::string& module_prefix(const std::string& modname) const {
        static const std::string none;
        auto it = mod.module_prefix.find(modname);
        return it == mod.module_prefix.end() ? none : it->second;
    }

    int64_t local_slot(const std::string& name) {
        auto it = locals.find(name);
        if (it != locals.end()) return it->second;
        int64_t idx = (int64_t)locals.size();
        locals.emplace(name, idx);
        return idx;
    }

    // A module that is neither native, nor in the project, nor found in the
    // shipped pylib or any Python installation on this machine. Say what *is*
    // importable, and what to do about a third-party package.
    std::string unknown_module_msg(const std::string& modname) const {
        std::set<std::string> avail;
        for (auto& [name, _] : modules())
            if (name.find('.') == std::string::npos) avail.insert(name);
        for (auto& name : mod.stdlib_available) avail.insert(name);
        std::string list;
        for (auto& name : avail) list += (list.empty() ? "" : ", ") + name;
        return "unknown module '" + modname + "'\n" +
               "  available: " + list + "\n" +
               "  to use a pure-Python package, put its source (" + modname +
               ".py, or " + modname + "/__init__.py) next to your input file and it "
               "will be compiled in ('kamipy paths' lists every directory searched, "
               "including the auto-discovered Python installations);\n"
               "  packages that need a CPython C extension (pyautogui, numpy, PIL, "
               "flask, ...) cannot be compiled ahead of time yet.";
    }

    void register_import(Stmt* s) {
        const std::string& modname = s->name;
        if (noop_module(modname)) {
            for (auto& extra : s->body) register_import(extra.get());
            return;
        }
        if (mod.user_modules.count(modname)) {
            // Local .py module bundled by the driver: its top-level code is
            // already spliced into this module. "alias.x" resolves to "x".
            user_imports[s->alias.empty() ? modname : s->alias] = modname;
            for (auto& extra : s->body) register_import(extra.get());
            return;
        }
        if (cabi_module(modname)) {
            imports[s->alias.empty() ? modname : s->alias] = modname;
            for (auto& extra : s->body) register_import(extra.get());
            return;
        }
        if (pyext_module(modname)) {
            // Bridged CPython extension: the module becomes an ordinary global
            // holding a KT_PYOBJ; codegen emits the runtime import here.
            s->pyext = true;
            s->global_idx = global_slot(s->alias.empty() ? modname : s->alias);
            for (auto& extra : s->body) register_import(extra.get());
            return;
        }
        if (modname.find('.') != std::string::npos || !modules().count(modname)) {
            if (try_depth > 0) return; // try: import X / except ImportError: pass
            err(s->line, unknown_module_msg(modname));
        }
        imports[s->alias.empty() ? modname : s->alias] = modname;
        for (auto& extra : s->body) register_import(extra.get());
    }

    void register_from_import(Stmt* s) {
        const std::string& modname = s->name;
        // `from . import mod1, mod2` — each imported name is a sibling module.
        if (s->relative && modname.empty()) {
            for (auto& [n, alias] : s->import_names) {
                if (mod.user_modules.count(n)) user_imports[alias] = n;
                else if (try_depth == 0 && !noop_module(n))
                    err(s->line, "cannot find local module '" + n +
                                     ".py' for 'from . import " + n + "'");
            }
            return;
        }
        if (s->star) {
            // `from X import *`: only meaningful for a bundled local module,
            // where its globals are already visible. Silently accept.
            return;
        }
        if (noop_module(modname)) return; // names are annotation-only
        if (mod.user_modules.count(modname)) {
            // Bundled module: its top-level names are already globals in this
            // program. A mangled pylib module needs an alias entry; for a local
            // .py file the name is already correct, so only plain (non-"as")
            // imports work there.
            const std::string& pfx = module_prefix(modname);
            for (auto& [n, alias] : s->import_names) {
                // `from concurrent.futures import _base` — a sibling MODULE
                if (mod.user_modules.count(modname + "." + n)) {
                    user_imports[alias] = modname + "." + n;
                    continue;
                }
                // package re-export recorded by the bundler
                auto xit = mod.module_exports.find(modname + "." + n);
                if (xit != mod.module_exports.end()) {
                    bundled_names[alias] = xit->second;
                    continue;
                }
                if (!pfx.empty()) bundled_names[alias] = pfx + n;
                else if (alias != n)
                    err(s->line, "'from " + modname + " import " + n + " as " + alias +
                                     "' — 'as' renames are not supported for bundled "
                                     "local modules yet; use the original name");
            }
            return;
        }
        if (is_cext_module(modname)) {
            // `from _math import sqrt` / `from posix import getcwd`
            for (auto& [n, alias] : s->import_names) {
                CPrimitive p;
                int64_t cv;
                if (cext_lookup(modname, n, p)) from_natives[alias] = p;
                else if (cext_const(modname, n, cv)) from_consts[alias] = {2, 0, "", cv};
                else if (try_depth == 0)
                    err(s->line, "C extension module '" + modname + "' has no member '" + n + "'");
            }
            return;
        }
        if (ffi_module(modname)) {
            // `from ctypes import CDLL` / `from cffi import FFI`
            for (auto& [n, alias] : s->import_names) ffi_names[alias] = modname + "." + n;
            return;
        }
        if (pyext_module(modname)) {
            // `from _hashlib import openssl_sha256`: each name becomes a
            // global bound to getattr(module, name) at this point at runtime.
            s->pyext = true;
            for (auto& [n, alias] : s->import_names)
                s->multi_tidx.push_back(global_slot(alias));
            return;
        }
        if (modname.find('.') != std::string::npos || !modules().count(modname)) {
            if (try_depth > 0) return;
            err(s->line, unknown_module_msg(modname));
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
                if (as_globals) {
                    // `try: from _queue import Empty / except: class Empty(...)`
                    // — a guarded module-level class definition (CPython stdlib
                    // fallback idiom). Register it like any top-level class.
                    if (classes.count(s->name))
                        err(s->line, "class '" + s->name + "' redefined");
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

    // `from json import dumps` where json is a mangled pylib module: rewrite the
    // alias onto the module's real (prefixed) global. Returns true if renamed.
    bool rebind_bundled(std::string& name) const {
        if (bundled_names.empty()) return false;
        auto bn = bundled_names.find(name);
        if (bn == bundled_names.end()) return false;
        if (cur_func && locals.count(name) && !global_decls.count(name)) return false;
        if (globals.count(name)) return false;
        name = bn->second;
        return true;
    }

    void resolve_name(Expr* e) {
        rebind_bundled(e->sval);
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
               imports.count(n) || user_imports.count(n) || bundled_names.count(n);
    }

    // Rewrites call kwargs/defaults into plain positional args when the callee
    // signature is known (user function or class constructor). Extra
    // positional arguments are packed into a *args tuple and unmatched keyword
    // arguments into a **kwargs dict when the callee declares them; the packed
    // values are passed as ordinary trailing arguments (slots params.size()
    // and params.size()+1).
    void apply_signature(Expr* e, Stmt* def, bool skip_self) {
        size_t nparams = def->params.size() - (skip_self ? 1 : 0);
        size_t ndefaults = def->defaults.size();
        size_t first_param = skip_self ? 1 : 0;
        bool hv = !def->vararg.empty(), hk = !def->kwarg.empty();
        // Keyword-only parameters cannot be filled positionally.
        size_t npos = nparams;
        if (def->kwonly >= 0) npos = (size_t)def->kwonly - first_param;
        std::vector<ExprPtr> final_args(nparams);
        std::vector<ExprPtr> extra_pos;
        std::vector<std::pair<ExprPtr, ExprPtr>> extra_kw;
        if (e->args.size() > npos) {
            if (!hv)
                err(e->line, def->name + "() takes " + std::to_string(npos) +
                                 " argument(s) but " + std::to_string(e->args.size()) +
                                 " were given");
            for (size_t i = npos; i < e->args.size(); i++)
                extra_pos.push_back(std::move(e->args[i]));
            e->args.resize(npos);
        }
        for (size_t i = 0; i < e->args.size(); i++) final_args[i] = std::move(e->args[i]);
        for (auto& [kw, val] : e->kwargs) {
            bool found = false;
            for (size_t i = 0; i < nparams; i++) {
                if (def->params[first_param + i] == kw) {
                    if (final_args[i])
                        err(e->line, def->name + "() got multiple values for '" + kw + "'");
                    final_args[i] = std::move(val);
                    found = true;
                    break;
                }
            }
            if (found) continue;
            if (hk) {
                auto k = std::make_unique<Expr>();
                k->kind = ExprKind::StrLit;
                k->line = e->line;
                k->sval = kw;
                extra_kw.emplace_back(std::move(k), std::move(val));
                continue;
            }
            err(e->line, def->name + "() got an unexpected keyword argument '" + kw + "'");
        }
        e->kwargs.clear();
        // Trailing holes that have defaults are NOT passed: the callee's
        // prologue evaluates the default expression in its own (definition)
        // scope — this is what makes non-literal defaults like
        // `thread_type=ThreadType.USER` work correctly. When packed *args /
        // **kwargs values must be passed after the fixed parameters, no holes
        // may remain, so every skipped default has to be a clonable literal.
        bool pass_packed = !extra_pos.empty() || !extra_kw.empty();
        size_t pass_n = nparams;
        if (!pass_packed) {
            while (pass_n > 0 && !final_args[pass_n - 1] && pass_n - 1 + ndefaults >= nparams &&
                   def->defaults[pass_n - 1 + ndefaults - nparams] != nullptr)
                pass_n--;
        }
        for (size_t i = 0; i < pass_n; i++) {
            if (final_args[i]) continue;
            size_t di = i + ndefaults;
            if (di >= nparams && def->defaults[di - nparams] != nullptr) {
                const Expr* d = def->defaults[di - nparams].get();
                if (!is_literal_default(d))
                    err(e->line, def->name + "(): parameter '" +
                                     def->params[first_param + i] +
                                     "' has a non-literal default and cannot be "
                                     "skipped when later arguments are given — pass "
                                     "it explicitly");
                final_args[i] = clone_literal(d);
            } else {
                err(e->line, def->name + "() missing required argument '" +
                                 def->params[first_param + i] + "'");
            }
        }
        final_args.resize(pass_n);
        if (pass_packed) {
            if (hv) { // *args tuple (always passed when anything packed follows)
                auto lst = std::make_unique<Expr>();
                lst->kind = ExprKind::ListLit;
                lst->line = e->line;
                lst->args = std::move(extra_pos);
                final_args.push_back(std::move(lst));
            }
            if (!extra_kw.empty()) { // **kwargs dict
                auto mp = std::make_unique<Expr>();
                mp->kind = ExprKind::MapLit;
                mp->line = e->line;
                mp->pairs = std::move(extra_kw);
                final_args.push_back(std::move(mp));
            }
        }
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

    // ---- C-ABI lowering ----------------------------------------------------

    // Remember a C function so codegen can `declare` it and the driver can pass
    // the right -l flag to the linker.
    void record_native(int line, const std::string& symbol, const std::string& csig,
                       const std::string& lib) {
        for (auto& n : mod.natives) {
            if (n.symbol != symbol) continue;
            if (n.csig != csig)
                err(line, "C symbol '" + symbol + "' is called with two different signatures ('" +
                              n.csig + "' and '" + csig + "')");
            if (!lib.empty()) mod.link_libs.insert(lib);
            return;
        }
        mod.natives.push_back({symbol, csig});
        if (!lib.empty()) mod.link_libs.insert(lib);
    }

    // Turn a Python-looking call into a direct C call: `call double @sqrt(...)`.
    void become_ccall(Expr* e, const std::string& symbol, const std::string& csig,
                      const std::string& lib) {
        if (!valid_csig(csig))
            err(e->line, "cannot bind '" + symbol + "': unsupported C signature '" + csig + "'");
        if (e->args.size() != csig.size() - 1)
            err(e->line, symbol + "() takes " + std::to_string(csig.size() - 1) +
                             " argument(s) through the C ABI, got " +
                             std::to_string(e->args.size()));
        if (!e->kwargs.empty())
            err(e->line, symbol + "(): C functions do not take keyword arguments");
        record_native(e->line, symbol, csig, lib);
        e->kind = ExprKind::CCall;
        e->sval = symbol;
        e->csig = csig;
        e->a.reset();
    }

    // `_math.sqrt(x)` / `posix.getcwd()`: either a runtime primitive or a plain
    // C call, depending on how the mapping table binds the member.
    void lower_cprimitive(Expr* e, const std::string& what, const CPrimitive& p) {
        if ((int)e->args.size() < p.min_args || (int)e->args.size() > p.max_args)
            err(e->line, what + "() got " + std::to_string(e->args.size()) + " argument(s)");
        if (p.builtin_id >= 0) {
            become_builtin_call(e, what, p.builtin_id);
            return;
        }
        become_ccall(e, p.symbol, p.csig, p.lib);
    }

    // With no explicit argtypes, ctypes passes ints as ints and (as a courtesy
    // KamiPython adds) float literals as doubles and strings as char*.
    static char guess_arg_code(const Expr* a) {
        switch (a->kind) {
        case ExprKind::FloatLit: return 'd';
        case ExprKind::StrLit: return 's';
        default: return 'i';
        }
    }

    // `lib.fn(...)` where `lib` came from ctypes.CDLL / ffi.dlopen.
    void lower_clib_call(Expr* e, const std::string& alias, const std::string& fname) {
        for (auto& a : e->args) resolve_expr(a.get());
        std::string key = alias + "." + fname;
        std::string csig;
        auto sit = cdef_sigs.find(fname);
        auto rit = cfunc_ret.find(key);
        auto ait = cfunc_args.find(key);
        if (rit == cfunc_ret.end() && ait == cfunc_args.end() && sit != cdef_sigs.end()) {
            csig = sit->second; // declared by cffi's cdef()
        } else {
            csig = std::string(1, rit != cfunc_ret.end() ? rit->second : 'i');
            if (ait != cfunc_args.end()) {
                csig += ait->second;
            } else {
                for (auto& a : e->args) csig += guess_arg_code(a.get());
            }
        }
        become_ccall(e, fname, csig, clibs.at(alias));
    }

    void become_builtin_call(Expr* e, const std::string& name, int64_t id) {
        e->kind = ExprKind::Call;
        auto callee = std::make_unique<Expr>();
        callee->kind = ExprKind::Name;
        callee->line = e->line;
        callee->sval = name;
        callee->res = Res::BuiltinFunc;
        callee->res_idx = id;
        e->a = std::move(callee);
    }

    // doctest.testmod(verbose=...) is the only remaining module call that must
    // swallow keyword arguments; everything else now lives in pylib/ as Python.
    bool rewrite_module_kwargs(Expr* e, const std::string& mod, const std::string& fn) {
        if (mod == "doctest" && fn == "testmod") {
            e->kwargs.clear();
            e->args.clear();
            return true;
        }
        if (mod == "os" && fn == "register_at_fork") { // fork hooks: no-op AOT
            e->kwargs.clear();
            e->args.clear();
            return true;
        }
        if (mod == "threading" && fn == "Thread") {
            // Thread(target=, args=, name=, daemon=) → positional
            // [target, args, name, daemon] for the KB_THREAD_THREAD builtin.
            ExprPtr target, args, name, daemon;
            if (!e->args.empty()) return false; // positional form not supported
            for (auto& [kw, val] : e->kwargs) {
                if (kw == "target") target = std::move(val);
                else if (kw == "args") args = std::move(val);
                else if (kw == "name") name = std::move(val);
                else if (kw == "daemon") daemon = std::move(val);
                else if (kw == "kwargs") return false;
            }
            e->kwargs.clear();
            auto none = [&]() {
                auto n = std::make_unique<Expr>();
                n->kind = ExprKind::NoneLit;
                n->line = e->line;
                return n;
            };
            e->args.push_back(target ? std::move(target) : none());
            e->args.push_back(args ? std::move(args) : none());
            e->args.push_back(name ? std::move(name) : none());
            e->args.push_back(daemon ? std::move(daemon) : none());
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
            // Bundled local module: "utils.CONSTANT" → plain name "CONSTANT"
            // (bundled modules share the program's global namespace). Dotted
            // packages ("concurrent.futures.FIRST_COMPLETED") flatten too.
            std::string chain = attr_chain_name(e->a.get());
            if (!chain.empty() && user_imports.count(chain) &&
                !name_shadowed(chain_root(chain))) {
                const std::string& modname = user_imports.at(chain);
                // package re-export: `concurrent.futures.Future` is really
                // `concurrent.futures._base.Future` (from-imported by the
                // package __init__) — the bundler recorded the mapping.
                auto xit = mod.module_exports.find(modname + "." + e->sval);
                std::string attr = xit != mod.module_exports.end()
                                       ? xit->second
                                       : module_prefix(modname) + e->sval;
                e->kind = ExprKind::Name;
                e->sval = attr;
                e->a.reset();
                resolve_expr(e);
                return;
            }
            if (e->a->kind == ExprKind::Name && imports.count(e->a->sval) &&
                !name_shadowed(e->a->sval)) {
                const std::string& modname = imports.at(e->a->sval);
                int64_t iv;
                if (cext_const(modname, e->sval, iv)) {
                    // `_socket.AF_INET`, `posix.O_RDONLY`: compile-time constants.
                    e->kind = ExprKind::IntLit;
                    e->ival = iv;
                    e->a.reset();
                    return;
                }
                if (ffi_module(modname))
                    err(e->line, modname + "." + e->sval +
                                     " is only supported in a library-handle assignment, a "
                                     "restype/argtypes assignment, or a call");
                ModConst cv;
                if (module_const(modname, e->sval, cv)) {
                    apply_const(e, cv);
                    e->a.reset();
                    return;
                }
                // sys.argv / sys.stdout / sys.stderr are computed values, not
                // constants: rewrite the attribute into a zero-argument builtin
                // call so codegen goes through the normal call path.
                int64_t zero_arg = -1;
                if (modname == "sys" && e->sval == "argv") zero_arg = KB_SYS_ARGV;
                else if (modname == "sys" && e->sval == "stdout") zero_arg = KB_SYS_STDOUT;
                else if (modname == "sys" && e->sval == "stderr") zero_arg = KB_SYS_STDERR;
                if (zero_arg >= 0) {
                    e->kind = ExprKind::Call;
                    auto callee = std::make_unique<Expr>();
                    callee->kind = ExprKind::Name;
                    callee->line = e->line;
                    callee->sval = "sys." + e->sval;
                    callee->res = Res::BuiltinFunc;
                    callee->res_idx = zero_arg;
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
            // Call-site unpacking `f(*a, **k)`: the argument shape is only
            // known at runtime, so lower to CallStar — the callee is evaluated
            // as a value, positionals are packed into a list (Starred items
            // extend it) and keywords into a dict, and kami_call_star matches
            // them against the callee's signature at runtime.
            {
                bool has_star = false, has_dstar = false;
                for (auto& a : e->args)
                    if (a->kind == ExprKind::Starred) has_star = true;
                for (auto& [kw, v] : e->kwargs)
                    if (kw.empty()) has_dstar = true;
                if (has_star || has_dstar) {
                    for (auto& a : e->args) resolve_expr(a.get());
                    for (auto& [kw, v] : e->kwargs) resolve_expr(v.get());
                    resolve_expr(e->a.get());
                    for (auto& [kw, v] : e->kwargs) {
                        ExprPtr key;
                        if (!kw.empty()) {
                            key = std::make_unique<Expr>();
                            key->kind = ExprKind::StrLit;
                            key->line = e->line;
                            key->sval = kw;
                        }
                        e->pairs.emplace_back(std::move(key), std::move(v));
                    }
                    e->kwargs.clear();
                    e->kind = ExprKind::CallStar;
                    return;
                }
            }
            // hasattr(<native module>, "name") — the answer is known at
            // compile time (module names are not values in this runtime):
            // fold to a boolean so `if hasattr(os, 'register_at_fork'):`
            // guards compile.
            if (e->a->kind == ExprKind::Name && e->a->sval == "hasattr" &&
                !name_shadowed("hasattr") && e->args.size() == 2 &&
                e->args[0]->kind == ExprKind::Name &&
                imports.count(e->args[0]->sval) &&
                !name_shadowed(e->args[0]->sval) &&
                e->args[1]->kind == ExprKind::StrLit) {
                const std::string& modname = imports.at(e->args[0]->sval);
                auto mit = modules().find(modname);
                if (mit != modules().end()) {
                    bool has = mit->second.count(e->args[1]->sval) != 0;
                    e->kind = ExprKind::BoolLit;
                    e->ival = has ? 1 : 0;
                    e->args.clear();
                    e->a.reset();
                    return;
                }
            }
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
            auto resolve_args = [&]() {
                for (auto& a : e->args) resolve_expr(a.get());
                for (auto& kv : e->kwargs) resolve_expr(kv.second.get());
            };
            if (callee->kind == ExprKind::Name) {
                rebind_bundled(callee->sval);
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
                        else if (!c->second.def->is_exception &&
                                 (!e->args.empty() || !e->kwargs.empty()))
                            err(e->line, n + "() takes no arguments");
                        resolve_args();
                        callee->res = Res::Global;
                        callee->res_idx = c->second.global;
                        return; // dynamic call: runtime instantiates
                    }
                    auto cn = from_natives.find(n);
                    if (cn != from_natives.end() && !globals.count(n)) {
                        resolve_args();
                        lower_cprimitive(e, n, cn->second);
                        return;
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
                        else if ((n == "min" || n == "max") && !e->kwargs.empty())
                            resolve_minmax_kwargs(e);
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
            // `super().m(args)` inside a method → BaseClass.m(self, args)
            // (single inheritance: the base is known statically).
            if (e->a->kind == ExprKind::Call && e->a->a &&
                e->a->a->kind == ExprKind::Name && e->a->a->sval == "super" &&
                !name_shadowed("super") && cur_class && cur_func &&
                !cur_func->params.empty()) {
                if (cur_class->alias.empty())
                    err(e->line, "super(): class '" + cur_class->name + "' has no base class");
                auto recv = std::make_unique<Expr>();
                recv->kind = ExprKind::Name;
                recv->line = e->line;
                recv->sval = cur_class->alias;
                auto selfe = std::make_unique<Expr>();
                selfe->kind = ExprKind::Name;
                selfe->line = e->line;
                selfe->sval = cur_func->params[0];
                e->a = std::move(recv);
                e->args.insert(e->args.begin(), std::move(selfe));
                resolve_expr(e);
                return;
            }
            // Bundled local module: "utils.helper(args)" → plain call "helper(args)".
            // Re-resolving as a Call keeps kwargs/default-argument support.
            // Dotted packages ("concurrent.futures.as_completed(...)") flatten too.
            {
                std::string chain = attr_chain_name(e->a.get());
                if (!chain.empty() && user_imports.count(chain) &&
                    !name_shadowed(chain_root(chain))) {
                    const std::string& modname = user_imports.at(chain);
                    auto xit = mod.module_exports.find(modname + "." + e->sval);
                    auto callee = std::make_unique<Expr>();
                    callee->kind = ExprKind::Name;
                    callee->line = e->line;
                    callee->sval = xit != mod.module_exports.end()
                                       ? xit->second
                                       : module_prefix(modname) + e->sval;
                    e->kind = ExprKind::Call;
                    e->a = std::move(callee);
                    e->sval.clear();
                    resolve_expr(e);
                    return;
                }
            }
            // obj.m(*a, **k): lower to a method CallStar — positionals packed
            // into a list, keywords into a dict, matched by the runtime.
            {
                bool has_star = false, has_dstar = false;
                for (auto& a : e->args)
                    if (a->kind == ExprKind::Starred) has_star = true;
                for (auto& [kw, v] : e->kwargs)
                    if (kw.empty()) has_dstar = true;
                if (has_star || has_dstar) {
                    for (auto& a : e->args) resolve_expr(a.get());
                    for (auto& [kw, v] : e->kwargs) resolve_expr(v.get());
                    resolve_expr(e->a.get()); // receiver
                    for (auto& [kw, v] : e->kwargs) {
                        ExprPtr key;
                        if (!kw.empty()) {
                            key = std::make_unique<Expr>();
                            key->kind = ExprKind::StrLit;
                            key->line = e->line;
                            key->sval = kw;
                        }
                        e->pairs.emplace_back(std::move(key), std::move(v));
                    }
                    e->kwargs.clear();
                    e->kind = ExprKind::CallStar; // sval keeps the method name
                    return;
                }
            }
            // os.path.<fn>(...) — nested-module call: base is Attr(os, "path").
            if (e->a->kind == ExprKind::Attr && e->a->a->kind == ExprKind::Name &&
                imports.count(e->a->a->sval) && imports.at(e->a->a->sval) == "os" &&
                e->a->sval == "path" && !name_shadowed(e->a->a->sval)) {
                for (auto& a : e->args) resolve_expr(a.get());
                if (!e->kwargs.empty())
                    err(e->line, "os.path." + e->sval + "() does not accept keyword arguments");
                auto& mm = modules().at("os.path");
                auto it = mm.find(e->sval);
                if (it == mm.end())
                    err(e->line, "module 'os.path' has no function '" + e->sval + "'");
                if ((int)e->args.size() < it->second.min_args ||
                    (int)e->args.size() > it->second.max_args)
                    err(e->line, "os.path." + e->sval + "() got " +
                                     std::to_string(e->args.size()) + " argument(s)");
                e->kind = ExprKind::Call;
                auto callee = std::make_unique<Expr>();
                callee->kind = ExprKind::Name;
                callee->line = e->line;
                callee->sval = "os.path." + e->sval;
                callee->res = Res::BuiltinFunc;
                callee->res_idx = it->second.id;
                e->a = std::move(callee);
                return;
            }
            // lib.fn(...) where lib is a ctypes/cffi library handle → C call.
            // No name_shadowed() guard: a name only becomes a handle by way of
            // an assignment the compiler consumed, so there is nothing to shadow.
            if (e->a->kind == ExprKind::Name && clibs.count(e->a->sval)) {
                lower_clib_call(e, e->a->sval, e->sval);
                return;
            }
            // ffi.cdef("double sqrt(double);") — compile-time declaration only.
            if (e->a->kind == ExprKind::Name && ffi_objects.count(e->a->sval)) {
                if (e->sval == "cdef" && e->args.size() == 1 &&
                    e->args[0]->kind == ExprKind::StrLit) {
                    parse_cdef(e->args[0]->sval, cdef_sigs);
                    e->args.clear();
                    e->kwargs.clear();
                    become_builtin_call(e, "ffi.cdef", KB_NOOP);
                    return;
                }
                err(e->line, "cffi: only ffi.cdef(<literal>) and `lib = ffi.dlopen(<literal>)` "
                             "are supported");
            }
            // _math.sqrt(...) / posix.getcwd(...) — C extension members.
            if (e->a->kind == ExprKind::Name && imports.count(e->a->sval) &&
                is_cext_module(imports.at(e->a->sval)) && !name_shadowed(e->a->sval)) {
                const std::string& modname = imports.at(e->a->sval);
                for (auto& a : e->args) resolve_expr(a.get());
                CPrimitive p;
                if (!cext_lookup(modname, e->sval, p))
                    err(e->line, "C extension module '" + modname + "' has no member '" + e->sval +
                                     "'");
                lower_cprimitive(e, modname + "." + e->sval, p);
                return;
            }
            for (auto& a : e->args) resolve_expr(a.get());
            for (auto& kv : e->kwargs) resolve_expr(kv.second.get());
            if (e->a->kind == ExprKind::Name && imports.count(e->a->sval) &&
                !name_shadowed(e->a->sval)) {
                const std::string& modname = imports.at(e->a->sval);
                if (ffi_module(modname))
                    err(e->line, modname + "." + e->sval +
                                     "(): expected `handle = " + modname +
                                     (modname == "ctypes" ? ".CDLL(<literal>)`" : ".FFI()`"));
                auto& mm = modules().at(modname);
                auto it = mm.find(e->sval);
                if (it == mm.end())
                    err(e->line, "module '" + modname + "' has no function '" + e->sval + "'");
                BuiltinSig sig = it->second;
                if ((int)e->args.size() < sig.min_args || (int)e->args.size() > sig.max_args)
                    err(e->line, modname + "." + e->sval + "() got " +
                                     std::to_string(e->args.size()) + " argument(s)");
                if (!e->kwargs.empty()) {
                    // requests.get(url, timeout=...) — map known kwargs positionally
                    if (!rewrite_module_kwargs(e, modname, e->sval))
                        err(e->line, modname + "." + e->sval +
                                         "() does not accept these keyword arguments");
                }
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
                    // Dynamic receiver (an object, a sync primitive, ...):
                    // lower to CallStar — kami_method_star matches the keyword
                    // dict against the callee signature at runtime, and native
                    // receivers (Condition.wait(timeout=...), Semaphore.
                    // acquire(timeout=...)) interpret it themselves. Errors
                    // stay catchable runtime errors.
                    for (auto& [kw, v] : e->kwargs) {
                        auto key = std::make_unique<Expr>();
                        key->kind = ExprKind::StrLit;
                        key->line = e->line;
                        key->sval = kw;
                        e->pairs.emplace_back(std::move(key), std::move(v));
                    }
                    e->kwargs.clear();
                    e->kind = ExprKind::CallStar; // sval keeps the method name
                    resolve_expr(e->a.get());
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
        case ExprKind::CCall:
            return; // produced by this pass, never fed back into it
        case ExprKind::MapLit:
            for (auto& p : e->pairs) {
                if (p.first) resolve_expr(p.first.get()); // null = **unpacking
                resolve_expr(p.second.get());
            }
            return;
        case ExprKind::Starred:
            resolve_expr(e->a.get());
            return;
        case ExprKind::Yield:
            if (e->a) resolve_expr(e->a.get());
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

    // "a.b.c" for a pure Name/Attr chain, "" otherwise — used to recognize
    // dotted module references like `concurrent.futures.as_completed`.
    static std::string attr_chain_name(const Expr* e) {
        if (e->kind == ExprKind::Name) return e->sval;
        if (e->kind == ExprKind::Attr) {
            std::string base = attr_chain_name(e->a.get());
            return base.empty() ? "" : base + "." + e->sval;
        }
        return "";
    }

    // First segment of a dotted name (shadowing is decided by the root name).
    static std::string chain_root(const std::string& dotted) {
        size_t dot = dotted.find('.');
        return dot == std::string::npos ? dotted : dotted.substr(0, dot);
    }

    void check_builtin_call(Expr* e, const std::string& n, const BuiltinSig& sig) {
        int total = (int)e->args.size();
        if (total < sig.min_args || total > sig.max_args)
            err(e->line, n + "() got " + std::to_string(total) + " argument(s)");
    }

    void resolve_minmax_kwargs(Expr* e) {
        // min/max(iterable, key=?, default=?) →
        //   KB_MIN_EX/KB_MAX_EX(iterable, key|None, has_default, default)
        if (e->args.size() != 1)
            err(e->line, "min()/max() keyword form takes a single iterable");
        ExprPtr key, dflt;
        bool has_default = false;
        for (auto& [kw, val] : e->kwargs) {
            if (kw == "key") key = std::move(val);
            else if (kw == "default") {
                dflt = std::move(val);
                has_default = true;
            } else
                err(e->line, "min()/max() got an unexpected keyword argument '" + kw + "'");
        }
        e->kwargs.clear();
        auto none = [&]() {
            auto n = std::make_unique<Expr>();
            n->kind = ExprKind::NoneLit;
            n->line = e->line;
            return n;
        };
        auto flag = std::make_unique<Expr>();
        flag->kind = ExprKind::BoolLit;
        flag->line = e->line;
        flag->ival = has_default ? 1 : 0;
        e->args.push_back(key ? std::move(key) : none());
        e->args.push_back(std::move(flag));
        e->args.push_back(dflt ? std::move(dflt) : none());
        e->a->res_idx = e->a->sval == "min" ? KB_MIN_EX : KB_MAX_EX;
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

    // ---- ctypes / cffi statement forms -------------------------------------

    // Name of the FFI entry point a call refers to: "ctypes.CDLL", "cffi.FFI",
    // "ctypes.cdll.LoadLibrary", or "" when the call is something else.
    std::string ffi_callee(const Expr* e) const {
        if (e->kind == ExprKind::Call && e->a && e->a->kind == ExprKind::Name) {
            auto it = ffi_names.find(e->a->sval); // `from cffi import FFI` → FFI()
            if (it != ffi_names.end()) return it->second;
            return "";
        }
        if (e->kind != ExprKind::MethodCall || !e->a) return "";
        if (e->a->kind == ExprKind::Name) {
            auto it = imports.find(e->a->sval);
            if (it != imports.end() && ffi_module(it->second)) return it->second + "." + e->sval;
            return "";
        }
        // ctypes.cdll.LoadLibrary("libm.so.6") / ctypes.windll.LoadLibrary(...)
        if (e->a->kind == ExprKind::Attr && e->a->a && e->a->a->kind == ExprKind::Name) {
            auto it = imports.find(e->a->a->sval);
            if (it != imports.end() && it->second == "ctypes")
                return "ctypes." + e->a->sval + "." + e->sval;
        }
        return "";
    }

    // Binds `lib = ctypes.CDLL("libm.so.6")`, `ffi = cffi.FFI()` and
    // `lib = ffi.dlopen("libm.so.6")`. Returns true when the statement was a
    // compile-time binding and produces no code.
    bool bind_ffi_assign(Stmt* s) {
        Expr* v = s->e1.get();
        // lib = ffi.dlopen("libm.so.6")
        if (v->kind == ExprKind::MethodCall && v->a && v->a->kind == ExprKind::Name &&
            ffi_objects.count(v->a->sval) && v->sval == "dlopen") {
            if (v->args.size() != 1 || v->args[0]->kind != ExprKind::StrLit)
                err(s->line, "ffi.dlopen() needs a string literal so the library can be linked "
                             "at build time");
            clibs[s->name] = derive_link_lib(v->args[0]->sval);
            s->kind = StmtKind::Pass;
            return true;
        }
        std::string callee = ffi_callee(v);
        if (callee.empty()) return false;
        if (callee == "cffi.FFI") {
            ffi_objects.insert(s->name);
            s->kind = StmtKind::Pass;
            return true;
        }
        bool is_dll = callee == "ctypes.CDLL" || callee == "ctypes.WinDLL" ||
                      callee == "ctypes.OleDLL" || callee == "ctypes.PyDLL" ||
                      callee == "ctypes.cdll.LoadLibrary" ||
                      callee == "ctypes.windll.LoadLibrary";
        if (!is_dll) return false;
        // CDLL(None) means "this process", i.e. libc — already linked in.
        if (v->args.size() == 1 && v->args[0]->kind == ExprKind::NoneLit) {
            clibs[s->name] = "";
            s->kind = StmtKind::Pass;
            return true;
        }
        if (v->args.empty() || v->args[0]->kind != ExprKind::StrLit)
            err(s->line, callee + "() needs a string literal so the library can be linked at "
                                  "build time");
        clibs[s->name] = derive_link_lib(v->args[0]->sval);
        s->kind = StmtKind::Pass;
        return true;
    }

    // Binds `lib.fn.restype = ctypes.c_double` and
    // `lib.fn.argtypes = [ctypes.c_double]`.
    bool bind_ffi_attr_assign(Stmt* s) {
        if (s->name != "restype" && s->name != "argtypes") return false;
        Expr* base = s->e1.get();
        if (base->kind != ExprKind::Attr || !base->a || base->a->kind != ExprKind::Name)
            return false;
        auto lib = clibs.find(base->a->sval);
        if (lib == clibs.end()) return false;
        std::string key = base->a->sval + "." + base->sval;
        // The right-hand side names ctypes types; spell them out as codes.
        auto type_name = [](const Expr* t) -> std::string {
            if (t->kind == ExprKind::Attr) return t->sval;
            if (t->kind == ExprKind::Name) return t->sval;
            if (t->kind == ExprKind::NoneLit) return "None";
            return "";
        };
        if (s->name == "restype") {
            char c = ctypes_type_code(type_name(s->e3.get()));
            if (!c) err(s->line, key + ".restype: unsupported ctypes type");
            cfunc_ret[key] = c;
        } else {
            if (s->e3->kind != ExprKind::ListLit)
                err(s->line, key + ".argtypes must be a list literal of ctypes types");
            std::string codes;
            for (auto& t : s->e3->args) {
                char c = ctypes_type_code(type_name(t.get()));
                if (!c || c == 'v') err(s->line, key + ".argtypes: unsupported ctypes type");
                codes += c;
            }
            cfunc_args[key] = codes;
        }
        s->kind = StmtKind::Pass;
        return true;
    }

    void resolve_stmt(Stmt* s) {
        switch (s->kind) {
        case StmtKind::ExprStmt: resolve_expr(s->e1.get()); return;
        case StmtKind::Assign:
            if (bind_ffi_assign(s)) return;
            resolve_expr(s->e1.get());
            resolve_target(s, s->name);
            return;
        case StmtKind::IndexAssign:
            resolve_expr(s->e1.get());
            resolve_expr(s->e2.get());
            resolve_expr(s->e3.get());
            return;
        case StmtKind::AttrAssign:
            if (bind_ffi_attr_assign(s)) return;
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
            // Dotted base (`class TPE(_base.Executor)`): resolve through the
            // bundled-module prefix so the mangled class name is found.
            if (!s->alias.empty()) {
                size_t dot = s->alias.find('.');
                if (dot != std::string::npos) {
                    std::string modalias = s->alias.substr(0, dot);
                    std::string cls = s->alias.substr(dot + 1);
                    if (user_imports.count(modalias))
                        s->alias = module_prefix(user_imports.at(modalias)) + cls;
                    else
                        s->alias = cls; // best effort (module not bundled)
                }
                // exception pedigree: direct builtin-exception base, or an
                // already-resolved exception class
                if (builtin_exception_name(s->alias)) s->is_exception = true;
                else if (classes.count(s->alias) &&
                         classes.at(s->alias).def->is_exception)
                    s->is_exception = true;
            }
            // resolve methods; class attribute assigns resolved as class-attr sets
            cur_class = s;
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
            cur_class = nullptr;
            for (auto& d : s->decorators) resolve_expr(d.get());
            if (!s->alias.empty() && !classes.count(s->alias)) {
                // `class E(Exception)` / `class E(ValueError)`: exceptions are
                // string-matched at runtime, so a builtin exception base adds
                // no behavior — treat the class as base-less instead of
                // rejecting real-world code.
                if (builtin_exception_name(s->alias)) s->alias.clear();
                else err(s->line, "unknown base class '" + s->alias + "'");
            }
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
            // raise UserError("msg") where UserError is an __init__-less
            // exception class (`class UserError(Exception): pass`): Exception
            // subclasses accept any message, and exceptions are matched by
            // name at runtime — raise "UserError: msg" directly.
            if (ex->kind == ExprKind::Call && ex->a->kind == ExprKind::Name &&
                classes.count(ex->a->sval) && ex->args.size() <= 1 &&
                ex->kwargs.empty()) {
                bool has_init = false;
                std::string cls = ex->a->sval;
                std::set<std::string> seen;
                while (classes.count(cls) && seen.insert(cls).second) {
                    if (classes.at(cls).methods.count("__init__")) {
                        has_init = true;
                        break;
                    }
                    cls = classes.at(cls).def->alias;
                }
                if (!has_init) {
                    s->raise_mode = 2;
                    s->name = ex->a->sval;
                    // library-module classes are mangled: report the source name
                    for (auto& [modname, prefix] : mod.module_prefix) {
                        if (s->name.rfind(prefix, 0) == 0) {
                            s->name = s->name.substr(prefix.size());
                            break;
                        }
                    }
                    if (!ex->args.empty()) {
                        s->e1 = std::move(ex->args[0]);
                        resolve_expr(s->e1.get());
                    } else {
                        s->e1.reset();
                    }
                    return;
                }
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
        // (null entries are "required keyword-only" holes — see parser)
        for (auto& d : s->defaults)
            if (d) resolve_expr(d.get());
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
        // *args tuple and **kwargs dict live in the slots directly after the
        // fixed parameters (params.size() and params.size()+1).
        for (const std::string* extra : {&s->vararg, &s->kwarg}) {
            if (extra->empty()) continue;
            if (locals.count(*extra)) err(s->line, "duplicate parameter '" + *extra + "'");
            if (global_decls.count(*extra))
                err(s->line, "parameter '" + *extra + "' declared global");
            local_slot(*extra);
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
        fn->vararg = lam->vararg;
        fn->kwarg = lam->kwarg;
        fn->kwonly = lam->kwonly;
        for (auto& d : lam->args) fn->defaults.push_back(std::move(d));
        lam->args.clear();
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

// ---------------------------------------------------------------------------
// Escape / alias analysis (per function).
//
// A local variable is "append-safe" when no alias of its value can survive to
// a later point in the function: assignments of the form `s = s + x` may then
// mutate the string buffer in place (kami_str_iadd) instead of copying the
// whole string — turning O(n²) build-up loops into amortized O(n).
//
// The analysis is flow-insensitive and conservative: a single retaining use
// (stored somewhere, passed to a call, captured by a closure, iterated while
// appends exist, ...) disqualifies the variable. Uses that only *read* the
// value and produce fresh objects (len(s), s[i], s + t, comparisons, print,
// str()/format() from f-strings, method receivers) are allowed.
namespace {

struct AliasScan {
    const Module& mod;
    std::set<int64_t> unsafe;          // local slots that may have live aliases
    std::set<int64_t> container_slots; // slots holding fresh lists/dicts/sets

    explicit AliasScan(const Module& m) : mod(m) {}

    static bool nonretaining_builtin(int64_t id) {
        return id == KB_PRINT || id == KB_PRINT_EX || id == KB_LEN || id == KB_ORD ||
               id == KB_STR || id == KB_FORMAT;
    }

    void mark(const Expr* e) {
        if (e && e->kind == ExprKind::Name && e->res == Res::Local)
            unsafe.insert(e->res_idx);
    }

    void closure_captures(const Stmt* fn) {
        for (size_t i = 0; i < fn->multi_tkind.size(); i++)
            if (fn->multi_tkind[i] == 1) unsafe.insert(fn->multi_tidx[i]);
    }

    // Does this expression always produce a FRESH value (a heap object no one
    // else references, or a primitive)? Locals assigned a non-fresh value may
    // alias somebody else's buffer and must never be mutated in place:
    //   s = lst[0]          (shares the element's buffer)
    //   s = some_global     (shares the global's buffer)
    //   b = helper(a)       (a user function may return its argument)
    static bool is_fresh(const Expr* e) {
        switch (e->kind) {
        case ExprKind::IntLit:
        case ExprKind::FloatLit:
        case ExprKind::StrLit:
        case ExprKind::BoolLit:
        case ExprKind::NoneLit:
        case ExprKind::ListLit:
        case ExprKind::SetLit:
        case ExprKind::MapLit:
        case ExprKind::ListComp:
        case ExprKind::SetComp:
        case ExprKind::MapComp:
        case ExprKind::Binary: // every binop builds a new value in this runtime
        case ExprKind::Unary:
            return true;
        case ExprKind::BoolOp: // may yield either operand unchanged
            return is_fresh(e->a.get()) && is_fresh(e->b.get());
        case ExprKind::IfExp:
            return is_fresh(e->a.get()) && is_fresh(e->c.get());
        case ExprKind::Call: {
            const Expr* callee = e->a.get();
            if (callee && callee->res == Res::BuiltinFunc) {
                switch (callee->res_idx) { // builtins that never return an argument
                case KB_STR: case KB_CHR: case KB_FORMAT: case KB_INPUT:
                case KB_BIN: case KB_HEX: case KB_OCT: case KB_LEN:
                case KB_ORD: case KB_INT: case KB_FLOAT: case KB_BOOL:
                case KB_ABS: case KB_ROUND:
                    return true;
                default: return false; // max(a, b) returns an operand, ...
                }
            }
            return false; // user functions may return an argument/global
        }
        default: return false; // Name/Index/Slice/Attr/MethodCall/...
        }
    }

    // `retains` = the value produced at this position may be stored beyond
    // the expression itself.
    void expr(const Expr* e, bool retains) {
        if (!e) return;
        switch (e->kind) {
        case ExprKind::Name:
            if (retains) mark(e);
            return;
        case ExprKind::Binary: // all binops produce fresh values
        case ExprKind::Unary:
            expr(e->a.get(), false);
            expr(e->b.get(), false);
            return;
        case ExprKind::BoolOp: // `a or b` may yield either operand unchanged
            expr(e->a.get(), retains);
            expr(e->b.get(), retains);
            return;
        case ExprKind::IfExp:
            expr(e->a.get(), retains);
            expr(e->b.get(), false); // condition
            expr(e->c.get(), retains);
            return;
        case ExprKind::Index:
        case ExprKind::Slice:
        case ExprKind::Attr:
            expr(e->a.get(), false);
            expr(e->b.get(), false);
            for (auto& p : e->args) expr(p.get(), false);
            return;
        case ExprKind::Call: {
            const Expr* callee = e->a.get();
            bool safe_args = callee && callee->res == Res::BuiltinFunc &&
                             nonretaining_builtin(callee->res_idx);
            if (callee && callee->res != Res::BuiltinFunc && callee->res != Res::UserFunc)
                expr(callee, false);
            for (auto& a : e->args) expr(a.get(), !safe_args);
            for (auto& kv : e->kwargs) expr(kv.second.get(), true);
            return;
        }
        case ExprKind::CallStar:
            expr(e->a.get(), false);
            for (auto& a : e->args) expr(a.get(), true);
            for (auto& p : e->pairs) {
                expr(p.first.get(), true);
                expr(p.second.get(), true);
            }
            return;
        case ExprKind::MethodCall:
            expr(e->a.get(), false); // receiver: never aliased as a whole
            for (auto& a : e->args) expr(a.get(), true);
            for (auto& kv : e->kwargs) expr(kv.second.get(), true);
            return;
        case ExprKind::CCall:
            for (auto& a : e->args) expr(a.get(), true); // C may stash pointers
            return;
        case ExprKind::ListLit:
        case ExprKind::SetLit:
            for (auto& a : e->args) expr(a.get(), true);
            return;
        case ExprKind::MapLit:
            for (auto& p : e->pairs) {
                expr(p.first.get(), true);
                expr(p.second.get(), true);
            }
            return;
        case ExprKind::Starred:
            expr(e->a.get(), true);
            return;
        case ExprKind::Yield:
            // the yielded value escapes to the consumer: always retained
            expr(e->a.get(), true);
            return;
        case ExprKind::ListComp:
        case ExprKind::SetComp:
        case ExprKind::MapComp:
            for (auto& cl : e->clauses) {
                expr(cl.iter.get(), true); // iterated: guard mid-loop mutation
                for (size_t i = 0; i < cl.tkind.size(); i++)
                    if (cl.tkind[i] == 1) unsafe.insert(cl.tidx[i]);
                for (auto& c : cl.conds) expr(c.get(), false);
            }
            expr(e->a.get(), true);
            if (!e->pairs.empty()) {
                expr(e->pairs[0].first.get(), true);
                expr(e->pairs[0].second.get(), true);
            }
            return;
        case ExprKind::Closure:
            closure_captures(mod.functions[(size_t)e->res_idx]);
            return;
        case ExprKind::Lambda: // resolved away before this pass
        default: return;
        }
    }

    void stmts(const std::vector<StmtPtr>& body) {
        for (auto& sp : body) stmt(sp.get());
    }

    void stmt(const Stmt* s) {
        switch (s->kind) {
        case StmtKind::ExprStmt:
            expr(s->e1.get(), false);
            return;
        case StmtKind::Assign:
            expr(s->e1.get(), true);
            if (s->target_res == Res::Local) {
                if (!is_fresh(s->e1.get())) unsafe.insert(s->target_idx);
                switch (s->e1->kind) { // slots that ever hold a fresh container
                case ExprKind::ListLit:
                case ExprKind::SetLit:
                case ExprKind::MapLit:
                case ExprKind::ListComp:
                case ExprKind::SetComp:
                case ExprKind::MapComp:
                    container_slots.insert(s->target_idx);
                    break;
                default: break;
                }
            }
            return;
        case StmtKind::IndexAssign:
            expr(s->e1.get(), false);
            expr(s->e2.get(), false);
            expr(s->e3.get(), true);
            return;
        case StmtKind::AttrAssign:
            expr(s->e1.get(), false);
            expr(s->e3.get(), true);
            return;
        case StmtKind::MultiAssign:
            for (auto& t : s->targets) {
                expr(t.get(), false);
                // unpacked values are shared elements of the RHS sequence
                if (t->kind == ExprKind::Name && t->res == Res::Local)
                    unsafe.insert(t->res_idx);
            }
            for (auto& v : s->values) expr(v.get(), true);
            return;
        case StmtKind::If:
        case StmtKind::While:
            expr(s->e1.get(), false);
            stmts(s->body);
            stmts(s->orelse);
            return;
        case StmtKind::For:
            expr(s->e1.get(), true); // iterated: guard mid-loop mutation
            // loop variables receive shared elements of the sequence
            if (s->target_res == Res::Local) unsafe.insert(s->target_idx);
            for (size_t i = 0; i < s->multi_tkind.size(); i++)
                if (s->multi_tkind[i] == 1) unsafe.insert(s->multi_tidx[i]);
            stmts(s->body);
            stmts(s->orelse);
            return;
        case StmtKind::FuncDef: // nested function: captures alias locals
            closure_captures(s);
            return;
        case StmtKind::Return:
        case StmtKind::Raise:
            expr(s->e1.get(), false); // function exits: no later appends
            return;
        case StmtKind::Try:
            stmts(s->body);
            for (auto& h : s->handlers) {
                if (h.as_kind == 1) unsafe.insert(h.as_idx);
                stmts(h.body);
            }
            stmts(s->orelse);
            stmts(s->final_body);
            return;
        case StmtKind::With:
            expr(s->e1.get(), true);
            if (s->target_res == Res::Local) unsafe.insert(s->target_idx);
            stmts(s->body);
            return;
        case StmtKind::Del:
            for (auto& t : s->targets) expr(t.get(), false);
            return;
        default: return;
        }
    }

    // pass 2: flag `v = v + x` assignments whose target has no aliases
    void flag(std::vector<StmtPtr>& body, int64_t first_nonparam) {
        for (auto& sp : body) flag_stmt(sp.get(), first_nonparam);
    }
    void flag_stmt(Stmt* s, int64_t first_nonparam) {
        switch (s->kind) {
        case StmtKind::Assign: {
            const Expr* v = s->e1.get();
            if (s->target_res == Res::Local && v && v->kind == ExprKind::Binary &&
                v->op == KOP_ADD && v->a && v->a->kind == ExprKind::Name &&
                v->a->res == Res::Local && v->a->res_idx == s->target_idx &&
                s->target_idx >= first_nonparam && !unsafe.count(s->target_idx))
                s->str_iadd = true;
            // Escape analysis, part II.2: overwriting an unaliased local that
            // holds a fresh container — the old list/dict/set is provably dead,
            // so its memory can be handed straight back to the allocator
            // (kami_free_hint) instead of waiting for a GC cycle.
            else if (s->target_res == Res::Local && s->target_idx >= first_nonparam &&
                     !unsafe.count(s->target_idx) &&
                     container_slots.count(s->target_idx))
                s->free_hint = true;
            return;
        }
        case StmtKind::If:
        case StmtKind::While:
        case StmtKind::For:
            flag(s->body, first_nonparam);
            flag(s->orelse, first_nonparam);
            return;
        case StmtKind::Try:
            flag(s->body, first_nonparam);
            for (auto& h : s->handlers) flag(h.body, first_nonparam);
            flag(s->orelse, first_nonparam);
            flag(s->final_body, first_nonparam);
            return;
        case StmtKind::With:
            flag(s->body, first_nonparam);
            return;
        default: return;
        }
    }
};

} // namespace

static void mark_string_iadds(const Module& m, Stmt* fn) {
    AliasScan scan(m);
    scan.stmts(fn->body);
    // parameters are caller-owned: never mutate their buffers in place
    int64_t first_nonparam = (int64_t)fn->params.size() + (fn->vararg.empty() ? 0 : 1) +
                             (fn->kwarg.empty() ? 0 : 1);
    scan.flag(fn->body, first_nonparam);
}

void analyze(Module& m) {
    Sema s(m);
    s.collect_module();
    for (auto& sp : m.body) s.resolve_stmt(sp.get());
    m.nglobals = (int64_t)s.globals.size();
    for (Stmt* fn : m.functions) mark_string_iadds(m, fn);
}

bool pyext_module_name(const std::string& name) { return pyext_module(name); }

bool known_builtin_module(const std::string& name) {
    return noop_module(name) || modules().count(name) != 0;
}

} // namespace kami
