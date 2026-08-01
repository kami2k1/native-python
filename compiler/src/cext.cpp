#include "cext.h"

#include "../../runtime/include/kami_builtins.h"

#include <algorithm>
#include <cctype>
#include <vector>

namespace kami {

bool valid_csig(const std::string& sig) {
    if (sig.empty() || sig.size() > 8) return false;
    if (std::string("ildfsv").find(sig[0]) == std::string::npos) return false;
    for (size_t i = 1; i < sig.size(); i++)
        if (std::string("ildfs").find(sig[i]) == std::string::npos) return false;
    return true;
}

namespace {

// A libm function bound directly: no runtime primitive in between, the compiled
// program calls the C library exactly like a C program would.
CPrimitive libm1(const char* sym) {
    CPrimitive p;
    p.symbol = sym;
    p.csig = "dd";
    p.lib = "m";
    p.min_args = p.max_args = 1;
    return p;
}
CPrimitive libm2(const char* sym) {
    CPrimitive p;
    p.symbol = sym;
    p.csig = "ddd";
    p.lib = "m";
    p.min_args = p.max_args = 2;
    return p;
}
CPrimitive native(const char* sym, const char* sig, const char* lib, int nargs) {
    CPrimitive p;
    p.symbol = sym;
    p.csig = sig;
    p.lib = lib;
    p.min_args = p.max_args = nargs;
    return p;
}
CPrimitive prim(int64_t id, int lo, int hi) {
    CPrimitive p;
    p.builtin_id = id;
    p.min_args = lo;
    p.max_args = hi;
    return p;
}

using Table = std::unordered_map<std::string, CPrimitive>;
using Modules = std::unordered_map<std::string, Table>;

const Modules& cext_modules() {
    static const Modules m = [] {
        Modules m;
        // ---- _math: straight through to libm --------------------------------
        Table math;
        for (const char* f : {"sqrt", "sin", "cos", "tan", "exp", "log", "log2", "log10", "fabs",
                              "floor", "ceil", "atan", "asin", "acos", "sinh", "cosh", "tanh",
                              "cbrt", "expm1", "log1p"})
            math[f] = libm1(f);
        for (const char* f : {"pow", "atan2", "fmod", "hypot", "copysign", "remainder"})
            math[f] = libm2(f);
        math["factorial"] = prim(KB_MATH_FACTORIAL, 1, 1);
        math["gcd"] = prim(KB_MATH_GCD, 0, 16);
        math["isqrt"] = prim(KB_MATH_ISQRT, 1, 1);
        math["trunc"] = prim(KB_MATH_TRUNC, 1, 1);
        math["isnan"] = prim(KB_MATH_ISNAN, 1, 1);
        math["isinf"] = prim(KB_MATH_ISINF, 1, 1);
        m["_math"] = math;
        m["cmath"] = {}; // recognised but empty: complex math is not modelled

        // ---- _json ----------------------------------------------------------
        m["_json"] = {
            {"loads", prim(KB_JSON_LOADS, 1, 1)},
            {"dumps", prim(KB_JSON_DUMPS, 1, 3)},
            {"scanstring", prim(KB_JSON_LOADS, 1, 3)},
            {"encode_basestring_ascii", prim(KB_JSON_DUMPS, 1, 1)},
        };

        // ---- _os / posix / nt ------------------------------------------------
        Table os = {
            {"getcwd", prim(KB_OS_GETCWD, 0, 0)},
            {"getcwdb", prim(KB_OS_GETCWD, 0, 0)},
            {"listdir", prim(KB_OS_LISTDIR, 0, 1)},
            {"unlink", prim(KB_OS_REMOVE, 1, 1)},
            {"remove", prim(KB_OS_REMOVE, 1, 1)},
            {"mkdir", prim(KB_OS_MKDIR, 1, 1)},
            {"rmdir", prim(KB_OS_RMDIR, 1, 1)},
            {"rename", prim(KB_OS_RENAME, 2, 2)},
            {"system", prim(KB_OS_SYSTEM, 1, 1)},
            {"getpid", prim(KB_OS_GETPID, 0, 0)},
            {"urandom", prim(KB_OS_URANDOM, 1, 1)},
            {"chdir", prim(KB_OS_CHDIR, 1, 1)},
            {"getenv", prim(KB_OS_GETENV, 1, 2)},
            // Raw descriptor I/O: real syscalls, via runtime/src/syscalls.cpp.
            {"open", prim(KB_SYS_OPEN, 2, 3)},
            {"read", prim(KB_SYS_READ, 2, 2)},
            {"write", prim(KB_SYS_WRITE, 2, 2)},
            {"close", prim(KB_SYS_CLOSE, 1, 1)},
            {"lseek", prim(KB_SYS_LSEEK, 3, 3)},
            {"strerror", native("strerror", "sl", "", 1)},
        };
        m["_os"] = os;
        m["posix"] = os;
        m["nt"] = os;

        // ---- _socket ---------------------------------------------------------
#ifdef _WIN32
        const char* sock_lib = "ws2_32";
#else
        const char* sock_lib = "";
#endif
        m["_socket"] = {
            {"socket", prim(KB_SOCKET_SOCKET, 0, 2)},
            {"gethostname", prim(KB_SYS_GETHOSTNAME, 0, 0)},
            // htons/htonl are uint16_t/uint32_t in C: 32-bit registers.
            {"htons", native("htons", "ll", sock_lib, 1)},
            {"ntohs", native("ntohs", "ll", sock_lib, 1)},
            {"htonl", native("htonl", "ll", sock_lib, 1)},
            {"ntohl", native("ntohl", "ll", sock_lib, 1)},
        };

        // ---- _struct ---------------------------------------------------------
        m["_struct"] = {
            {"pack", prim(KB_STRUCT_PACK, 1, 16)},
            {"unpack", prim(KB_STRUCT_UNPACK, 2, 2)},
            {"calcsize", prim(KB_STRUCT_CALCSIZE, 1, 1)},
        };

        // ---- _time / _random -------------------------------------------------
        m["_time"] = {
            {"time", prim(KB_TIME_TIME, 0, 0)},
            {"monotonic", prim(KB_TIME_MONOTONIC, 0, 0)},
            {"perf_counter", prim(KB_TIME_PERF_COUNTER, 0, 0)},
            {"sleep", prim(KB_TIME_SLEEP, 1, 1)},
        };
        m["_random"] = {
            {"random", prim(KB_RANDOM_RANDOM, 0, 0)},
            {"seed", prim(KB_RANDOM_SEED, 0, 1)},
        };
        return m;
    }();
    return m;
}

} // namespace

bool is_cext_module(const std::string& name) { return cext_modules().count(name) != 0; }

bool is_ffi_module(const std::string& name) { return name == "ctypes" || name == "cffi"; }

bool cext_lookup(const std::string& module, const std::string& attr, CPrimitive& out) {
    auto mit = cext_modules().find(module);
    if (mit == cext_modules().end()) return false;
    auto fit = mit->second.find(attr);
    if (fit == mit->second.end()) return false;
    out = fit->second;
    return true;
}

bool cext_const(const std::string& module, const std::string& attr, int64_t& out) {
    struct Entry {
        const char* mod;
        const char* attr;
        int64_t val;
    };
    // Values are the ones every mainstream platform agrees on; the O_* flags
    // are translated by the runtime (see syscalls.cpp) so they stay portable.
    static const Entry table[] = {
        {"_socket", "AF_INET", 2},      {"_socket", "AF_INET6", 10},
        {"_socket", "SOCK_STREAM", 1},  {"_socket", "SOCK_DGRAM", 2},
        {"_socket", "SOL_SOCKET", 1},   {"_socket", "SO_REUSEADDR", 2},
        {"_os", "O_RDONLY", 0},         {"_os", "O_WRONLY", 1},
        {"_os", "O_RDWR", 2},           {"_os", "O_CREAT", 0100},
        {"_os", "O_TRUNC", 01000},      {"_os", "O_APPEND", 02000},
        {"posix", "O_RDONLY", 0},       {"posix", "O_WRONLY", 1},
        {"posix", "O_RDWR", 2},         {"posix", "O_CREAT", 0100},
        {"posix", "O_TRUNC", 01000},    {"posix", "O_APPEND", 02000},
        {"nt", "O_RDONLY", 0},          {"nt", "O_WRONLY", 1},
        {"nt", "O_RDWR", 2},            {"nt", "O_CREAT", 0100},
        {"nt", "O_TRUNC", 01000},       {"nt", "O_APPEND", 02000},
        {"_os", "SEEK_SET", 0},         {"_os", "SEEK_CUR", 1},
        {"_os", "SEEK_END", 2},         {"posix", "SEEK_SET", 0},
        {"posix", "SEEK_CUR", 1},       {"posix", "SEEK_END", 2},
    };
    for (const Entry& e : table)
        if (module == e.mod && attr == e.attr) {
            out = e.val;
            return true;
        }
    return false;
}

// ---------------------------------------------------------------------------
// ctypes / cffi
// ---------------------------------------------------------------------------

char ctypes_type_code(const std::string& name) {
    std::string n = name;
    size_t dot = n.rfind('.');
    if (dot != std::string::npos) n = n.substr(dot + 1); // "ctypes.c_int" → "c_int"
    if (n == "c_double" || n == "c_longdouble") return 'd';
    if (n == "c_float") return 'f';
    if (n == "c_char_p" || n == "c_wchar_p") return 's';
    if (n == "None" || n == "c_void") return 'v';
    // Pointers and the 64-bit integers travel in full registers; C `int` and
    // everything narrower is a 32-bit register after integer promotion.
    if (n == "c_void_p" || n == "c_longlong" || n == "c_ulonglong" || n == "c_int64" ||
        n == "c_uint64" || n == "c_size_t" || n == "c_ssize_t")
        return 'i';
#ifdef _WIN32
    if (n == "c_long" || n == "c_ulong") return 'l'; // LLP64: long is 32-bit
#else
    if (n == "c_long" || n == "c_ulong") return 'i'; // LP64: long is 64-bit
#endif
    if (n.rfind("c_", 0) == 0) return 'l'; // c_int, c_short, c_byte, c_bool, ...
    return 0;
}

char c_decl_type_code(const std::string& type_tokens) {
    std::string t;
    for (char c : type_tokens)
        if (!isspace((unsigned char)c) || (!t.empty() && t.back() != ' '))
            t += (char)tolower((unsigned char)c);
    while (!t.empty() && t.back() == ' ') t.pop_back();
    if (t.find('*') != std::string::npos) {
        // char*/const char* is a string; any other pointer is an opaque word.
        return t.find("char") != std::string::npos ? 's' : 'i';
    }
    if (t.find("double") != std::string::npos) return 'd';
    if (t.find("float") != std::string::npos) return 'f';
    if (t == "void") return 'v';
    // Pointer-sized integers.
    if (t.find("long long") != std::string::npos || t.find("size_t") != std::string::npos ||
        t.find("intptr") != std::string::npos || t.find("ptrdiff") != std::string::npos)
        return 'i';
    if (t.find("long") != std::string::npos) {
#ifdef _WIN32
        return 'l'; // LLP64: long is 32-bit
#else
        return 'i'; // LP64
#endif
    }
    if (t.find("int") != std::string::npos || t.find("short") != std::string::npos ||
        t.find("char") != std::string::npos || t.find("bool") != std::string::npos)
        return 'l';
    return 0;
}

std::string derive_link_lib(const std::string& dll_name) {
    // Strip directories: only the base name carries the library identity.
    std::string base = dll_name;
    size_t slash = base.find_last_of("/\\");
    if (slash != std::string::npos) base = base.substr(slash + 1);

    // libfoo.so.1.2 / libfoo.dylib / libfoo.a → foo
    if (base.rfind("lib", 0) == 0) base = base.substr(3);
    for (const char* ext : {".so", ".dylib", ".dll", ".a", ".lib"}) {
        size_t p = base.find(ext);
        if (p != std::string::npos) {
            base = base.substr(0, p);
            break;
        }
    }
    // The C runtime is linked into every program already; asking the linker for
    // it again is at best redundant and on Windows an error.
    if (base == "c" || base == "msvcrt" || base == "ucrtbase" || base == "System" || base.empty())
        return "";
    return base;
}

void parse_cdef(const std::string& text, std::unordered_map<std::string, std::string>& sigs) {
    // One prototype per ';'. Anything that is not "<type> name(<params>)" — a
    // typedef, a struct body, a #define — is skipped.
    size_t pos = 0;
    while (pos < text.size()) {
        size_t end = text.find(';', pos);
        std::string decl = text.substr(pos, end == std::string::npos ? std::string::npos
                                                                     : end - pos);
        pos = end == std::string::npos ? text.size() : end + 1;

        size_t open = decl.find('(');
        size_t close = decl.rfind(')');
        if (open == std::string::npos || close == std::string::npos || close < open) continue;
        std::string head = decl.substr(0, open);
        std::string params = decl.substr(open + 1, close - open - 1);
        if (head.find("typedef") != std::string::npos || head.find('#') != std::string::npos)
            continue;

        // The function name is the last identifier in the head; everything
        // before it is the return type (possibly with a trailing '*').
        size_t e = head.size();
        while (e > 0 && isspace((unsigned char)head[e - 1])) e--;
        size_t b = e;
        while (b > 0 && (isalnum((unsigned char)head[b - 1]) || head[b - 1] == '_')) b--;
        if (b == e) continue;
        std::string name = head.substr(b, e - b);
        std::string ret = head.substr(0, b);
        char rc = c_decl_type_code(ret);
        if (!rc) continue;

        std::string sig(1, rc);
        bool ok = true;
        size_t p = 0;
        while (p < params.size() && ok) {
            size_t comma = params.find(',', p);
            std::string one =
                params.substr(p, comma == std::string::npos ? std::string::npos : comma - p);
            p = comma == std::string::npos ? params.size() : comma + 1;
            // Drop a parameter name if present: "double x" → "double".
            size_t te = one.size();
            while (te > 0 && isspace((unsigned char)one[te - 1])) te--;
            size_t tb = te;
            while (tb > 0 && (isalnum((unsigned char)one[tb - 1]) || one[tb - 1] == '_')) tb--;
            std::string type = one;
            if (tb > 0 && c_decl_type_code(one.substr(0, tb))) type = one.substr(0, tb);
            char c = c_decl_type_code(type);
            if (c == 'v') continue; // f(void)
            if (!c) {
                ok = false;
                break;
            }
            sig += c;
        }
        if (ok && valid_csig(sig)) sigs[name] = sig;
    }
}

} // namespace kami
