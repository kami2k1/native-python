#pragma once
// C-extension and C-ABI binding layer.
//
// CPython's standard library is only half Python: the interesting half lives in
// C extension modules (_socket, _math, posix, _struct, _json). When the Python
// half of a module is compiled by KamiPython, those calls have to land
// somewhere. This file is the map from "C extension member" to either
//
//   * a runtime primitive (a KB_* builtin implemented in runtime/src), or
//   * a direct C-ABI call to a symbol in libc / libm / ws2_32 / msvcrt,
//
// plus the small amount of ctypes/cffi understanding needed to turn
// `lib.sqrt(2.0)` into `call double @sqrt(double 2.0)` in the emitted IR.
#include <cstdint>
#include <string>
#include <unordered_map>

namespace kami {

// A C-ABI signature is a compact string: the first character is the return
// type, the rest are parameters. The width matters — handing an i64 to a
// function whose parameter is a C `int` is undefined behaviour, not a nicety —
// so each C width gets its own code:
//   'i' int64_t (long/long long/size_t/pointer-sized)
//   'l' int32_t (C `int`, and anything narrower after integer promotion)
//   'd' double        'f' float
//   's' const char* (NUL-terminated)          'v' void (return only)
// So "dd" is double(double), "sl" is char*(int), "ils" is long(int, char*).
bool valid_csig(const std::string& sig);

struct CPrimitive {
    int64_t builtin_id = -1; // >= 0: reuse this runtime primitive
    std::string symbol;      // otherwise: emit a direct call to this C symbol
    std::string csig;
    std::string lib; // extra link library, e.g. "m" or "ws2_32" ("" = implicit)
    int min_args = 0;
    int max_args = 0;
};

// Is `name` a C extension module we know how to bind (_socket, _math, ...)?
bool is_cext_module(const std::string& name);

// The two FFI front ends. They are handled by the compiler, so an installed
// CPython copy of ctypes/ must never shadow them.
bool is_ffi_module(const std::string& name);

// Members of a C extension module: `_math.sqrt`, `posix.getcwd`, ...
bool cext_lookup(const std::string& module, const std::string& attr, CPrimitive& out);

// Integer constants exported by those modules: `_os.O_RDONLY`, `_socket.AF_INET`.
bool cext_const(const std::string& module, const std::string& attr, int64_t& out);

// ---- ctypes / cffi ----------------------------------------------------------

// "ctypes.c_double" / "c_double" → 'd'. Returns 0 when unknown.
char ctypes_type_code(const std::string& name);

// A C type as written in a cffi cdef() string ("double", "char *", "void") → code.
char c_decl_type_code(const std::string& type_tokens);

// "libm.so.6" → "m", "ws2_32.dll" → "ws2_32", "libc.so.6" → "" (implicit).
// The result is what gets handed to the linker as -l<lib>.
std::string derive_link_lib(const std::string& dll_name);

// Extract `double sqrt(double);`-style prototypes from a cffi cdef() string.
// Declarations we cannot parse are skipped: cdef() accepts far more C than we
// need, and an unparsed prototype simply falls back to ctypes' default int ABI.
void parse_cdef(const std::string& text, std::unordered_map<std::string, std::string>& sigs);

} // namespace kami
