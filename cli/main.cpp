// kamipy — the KamiPython compiler CLI.
//   kamipy build main.py [-o app] [-O0|-O1|-O2] [--emit-llvm] [--emit-ast]
//   kamipy run   main.py
//   kamipy clean
#include "../compiler/src/driver.h"
#include "../compiler/src/token.h"

#include <cstdio>
#include <cstring>
#include <filesystem>

namespace fs = std::filesystem;
using namespace kami;

static const char* CACHE_DIR = ".kamipy-cache";

static void usage() {
    fprintf(stderr,
            "KamiPython native compiler\n"
            "usage:\n"
            "  kamipy build <file.py> [-o <out>] [-O0|-O1|-O2] [--emit-llvm] [--emit-ast]\n"
            "  kamipy run   <file.py>\n"
            "  kamipy clean\n");
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
        fprintf(stderr, "%s:%d: error: %s\n",
                argc > 2 ? argv[2] : "<input>", e.line, e.what());
        return 1;
    } catch (const std::exception& e) {
        fprintf(stderr, "kamipy: error: %s\n", e.what());
        return 1;
    }
}
