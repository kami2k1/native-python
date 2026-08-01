// Unit tests for the C-ABI binding layer and the system-Python discovery.
// Exit code 0 = pass.
#include "../../compiler/src/cext.h"
#include "../../compiler/src/modules.h"

#include <cstdio>
#include <filesystem>

using namespace kami;

static int failures = 0;

static void eq_str(const std::string& got, const std::string& want, int line) {
    if (got == want) return;
    fprintf(stderr, "FAIL line %d: got \"%s\", want \"%s\"\n", line, got.c_str(), want.c_str());
    failures++;
}

static void eq_char(char got, char want, int line) {
    if (got == want) return;
    fprintf(stderr, "FAIL line %d: got '%c' (%d), want '%c'\n", line, got ? got : '?', (int)got,
            want);
    failures++;
}

static void is_true(bool cond, const char* what, int line) {
    if (cond) return;
    fprintf(stderr, "FAIL line %d: %s\n", line, what);
    failures++;
}

#define EQS(got, want) eq_str(got, want, __LINE__)
#define EQC(got, want) eq_char(got, want, __LINE__)
#define OK(cond) is_true(cond, #cond, __LINE__)

int main() {
    // ---- link-library derivation ----
    EQS(derive_link_lib("libm.so.6"), "m");
    EQS(derive_link_lib("libssl.so.1.1"), "ssl");
    EQS(derive_link_lib("/usr/lib/x86_64-linux-gnu/libcrypto.so.3"), "crypto");
    EQS(derive_link_lib("libz.dylib"), "z");
    EQS(derive_link_lib("ws2_32.dll"), "ws2_32");
    EQS(derive_link_lib("C:\\Windows\\System32\\kernel32.dll"), "kernel32");
    // The C runtime is always linked already; asking again is wrong.
    EQS(derive_link_lib("libc.so.6"), "");
    EQS(derive_link_lib("msvcrt.dll"), "");

    // ---- ctypes types: width matters ----
    EQC(ctypes_type_code("ctypes.c_double"), 'd');
    EQC(ctypes_type_code("c_float"), 'f');
    EQC(ctypes_type_code("c_int"), 'l');   // 32-bit
    EQC(ctypes_type_code("c_short"), 'l'); // promoted to int
    EQC(ctypes_type_code("c_longlong"), 'i');
    EQC(ctypes_type_code("c_size_t"), 'i');
    EQC(ctypes_type_code("c_void_p"), 'i');
    EQC(ctypes_type_code("c_char_p"), 's');
    EQC(ctypes_type_code("None"), 'v');
    OK(ctypes_type_code("SomeStruct") == 0);

    // ---- cffi cdef() prototypes ----
    std::unordered_map<std::string, std::string> sigs;
    parse_cdef("double sqrt(double x);\n"
               "int toupper(int c);\n"
               "char *strerror(int errnum);\n"
               "void free(void *p);\n"
               "size_t strlen(const char *s);\n"
               "int gettimeofday();\n"
               "typedef struct { int a; } thing;\n"
               "struct opaque;\n",
               sigs);
    EQS(sigs["sqrt"], "dd");
    EQS(sigs["toupper"], "ll");
    EQS(sigs["strerror"], "sl");
    EQS(sigs["free"], "vi");
    EQS(sigs["strlen"], "is");
    EQS(sigs["gettimeofday"], "l");
    OK(sigs.count("thing") == 0);

    // ---- signature validation ----
    OK(valid_csig("dd"));
    OK(valid_csig("v"));
    OK(valid_csig("islfd"));
    OK(!valid_csig(""));
    OK(!valid_csig("dv"));  // void parameter
    OK(!valid_csig("zz"));  // unknown code
    OK(!valid_csig("iiiiiiiii")); // too many parameters

    // ---- C extension mapping ----
    CPrimitive p;
    OK(is_cext_module("_math"));
    OK(is_cext_module("posix"));
    OK(is_ffi_module("ctypes"));
    OK(!is_cext_module("json")); // native module, not a C extension binding
    OK(cext_lookup("_math", "sqrt", p));
    EQS(p.symbol, "sqrt");
    EQS(p.csig, "dd");
    EQS(p.lib, "m");
    OK(cext_lookup("_math", "factorial", p) && p.builtin_id >= 0 && p.symbol.empty());
    OK(cext_lookup("posix", "read", p) && p.builtin_id >= 0);
    OK(!cext_lookup("_math", "no_such_function", p));
    int64_t cv = -1;
    OK(cext_const("_socket", "AF_INET", cv) && cv == 2);
    OK(cext_const("posix", "O_RDONLY", cv) && cv == 0);
    OK(!cext_const("_socket", "NOPE", cv));

    // ---- discovery: must be side-effect free, cached and self-consistent ----
    const auto& a = find_system_python_stdlib();
    const auto& b = find_system_python_stdlib();
    OK(&a == &b); // cached, computed exactly once
    for (const auto& dir : a) {
        OK(std::filesystem::is_directory(dir));
        OK(dir.is_absolute());
    }

    if (failures) {
        fprintf(stderr, "%d failure(s)\n", failures);
        return 1;
    }
    printf("test_cabi: all checks passed\n");
    return 0;
}
