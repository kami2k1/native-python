// kamipy — the KamiPython compiler CLI.
//   kamipy build main.py [-o app] [-O0|-O1|-O2] [--emit-llvm] [--emit-ast]
//   kamipy run   main.py
//   kamipy clean
#include "../compiler/src/driver.h"
#include "../compiler/src/token.h"

#include <cstdio>
#include <cstring>
#include <string>
#include <filesystem>

namespace fs = std::filesystem;
using namespace kami;

static const char* CACHE_DIR = ".kamipy-cache";

// C/C++ sources, objects, libraries and linker flags that ride along with the
// compiled Python.
static bool is_native_input(const std::string& a) {
    if (a.rfind("-l", 0) == 0 || a.rfind("-L", 0) == 0) return true;
    static const char* exts[] = {".c", ".cpp", ".cc", ".cxx", ".o", ".obj", ".a", ".lib", ".so"};
    for (const char* e : exts) {
        size_t n = strlen(e);
        if (a.size() > n && a.compare(a.size() - n, n, e) == 0) return true;
    }
    return false;
}

static void usage() {
    fprintf(stderr,
            "KamiPython native compiler\n"
            "usage:\n"
            "  kamipy build <file.py> [-o <out>] [-O0|-O1|-O2] [--emit-llvm] [--emit-ast]\n"
            "               [--target <triple>] [--sysroot <dir>]\n"
            "               [extra.c ...] [lib.a ...] [-l<name>] [-L<dir>]\n"
            "  kamipy run   <file.py>\n"
            "  kamipy clean\n"
            "\n"
            "Cross-compilation: --target picks the LLVM target triple (needs the\n"
            "target's sysroot and lld), e.g.\n"
            "  kamipy build main.py --target x86_64-unknown-linux-gnu -o app_linux\n"
            "\n"
            "C interop: extra C/C++ sources, objects and libraries are compiled and\n"
            "linked into the same executable, so ctypes.CDLL can bind to them.\n");
}

int main(int argc, char** argv) {
    if (argc < 2) {
        usage();
        return 2;
    }
    std::string cmd = argv[1];
    try {
        if (cmd == "clean") {
            std::error_code ec;
            fs::remove_all(CACHE_DIR, ec);
            printf("cleaned %s\n", CACHE_DIR);
            return 0;
        }
        if (cmd != "build" && cmd != "run") {
            usage();
            return 2;
        }
        if (argc < 3) {
            usage();
            return 2;
        }
        BuildOptions opts;
        opts.argv0 = argv[0];
        opts.input = argv[2];
        for (int i = 3; i < argc; i++) {
            std::string a = argv[i];
            if (a == "-o" && i + 1 < argc) opts.output = argv[++i];
            else if (a == "-O0") opts.opt_level = 0;
            else if (a == "-O1") opts.opt_level = 1;
            else if (a == "-O2") opts.opt_level = 2;
            else if (a == "--emit-llvm") opts.emit_llvm = true;
            else if (a == "--emit-ast") opts.emit_ast = true;
            else if (a == "--target" && i + 1 < argc) opts.target = argv[++i];
            else if (a.rfind("--target=", 0) == 0) opts.target = a.substr(9);
            else if (a == "--sysroot" && i + 1 < argc) opts.sysroot = argv[++i];
            else if (a.rfind("--sysroot=", 0) == 0) opts.sysroot = a.substr(10);
            else if (is_native_input(a)) opts.extra_inputs.push_back(a);
            else {
                fprintf(stderr, "unknown option: %s\n", a.c_str());
                return 2;
            }
        }
        if (cmd == "run") {
            fs::create_directories(CACHE_DIR);
            opts.output = (fs::path(CACHE_DIR) / fs::path(opts.input).stem()).string();
            std::string exe = build(opts);
            fs::path p = fs::absolute(exe);
            return run_process({p.string()});
        }
        std::string exe = build(opts);
        if (!opts.emit_ast) printf("built %s\n", exe.c_str());
        return 0;
    } catch (const CompileError& e) {
        const char* file = !e.file.empty() ? e.file.c_str()
                                           : (argc > 2 ? argv[2] : "<input>");
        fprintf(stderr, "%s:%d: error: %s\n", file, e.line, e.what());
        return 1;
    } catch (const std::exception& e) {
        fprintf(stderr, "kamipy: error: %s\n", e.what());
        return 1;
    }
}
