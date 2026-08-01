#include "driver.h"

#include "codegen.h"
#include "lexer.h"
#include "modules.h"
#include "parser.h"
#include "sema.h"

#include "../../runtime/include/kami_runtime.h"

#include <cstdlib>
#include <filesystem>
#include <fstream>
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

std::string executable_dir(const std::string& argv0) { return self_dir(argv0).string(); }

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

std::string build(const BuildOptions& opts) {
    std::string src = read_file(opts.input);

    Module mod = parse(lex(src));
    mod.source_path = fs::absolute(opts.input).string();
    bundle_modules(mod, opts.input, opts.argv0);
    std::string ir;
    try {
        analyze(mod);
        if (opts.emit_ast) {
            fputs(dump_module(mod).c_str(), stdout);
            return "";
        }
        ir = codegen(mod, opts.input, opts.target);
    } catch (const CompileError& e) {
        // The error may sit in a translated stdlib module: report its own file.
        throw locate_error(mod, e);
    }

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
    std::vector<std::string> cmd = {cxx, "-O" + std::to_string(opts.opt_level)};
    // Cross-compilation: clang selects the backend from the target triple, so the
    // same .ll can be lowered for another platform given its sysroot.
    if (!opts.target.empty()) {
        cmd.push_back("--target=" + opts.target);
        cmd.push_back("-fuse-ld=lld");
    }
    if (!opts.sysroot.empty()) cmd.push_back("--sysroot=" + opts.sysroot);
    cmd.push_back(ll.string());
    // Extra C/C++ sources, objects and libraries are compiled and linked by the
    // same clang invocation, next to the Python program's IR (this is what makes
    // ctypes.CDLL of a locally built library work in a single binary).
    for (const auto& x : opts.extra_inputs) {
        // A .c file must be compiled as C, or clang++ would mangle its symbols
        // and the direct calls emitted for ctypes would not resolve.
        bool is_c = x.size() > 2 && x.compare(x.size() - 2, 2, ".c") == 0;
        if (is_c) {
            cmd.push_back("-x");
            cmd.push_back("c");
        }
        cmd.push_back(x);
        if (is_c) {
            cmd.push_back("-x");
            cmd.push_back("none");
        }
    }
    // Libraries named by ctypes.CDLL(...) — a compiled ctypes call is a direct
    // call, so the symbol has to be there at link time.
    for (const auto& l : mod.link_libs) cmd.push_back(l);
    cmd.push_back(rtlib);
    cmd.push_back("-o");
    cmd.push_back(out.string());
#ifndef _WIN32
    cmd.push_back("-pthread");
    cmd.push_back("-lm");
    cmd.push_back("-Wl,--gc-sections");
#else
    cmd.push_back("-lws2_32");
#endif
    // clang warns about override of module-less IR opt flags; keep output clean:
    cmd.push_back("-Wno-override-module");
    int rc = run_process(cmd);
    if (!opts.emit_llvm) {
        std::error_code ec;
        fs::remove(ll, ec);
    }
    if (rc != 0) {
        std::string msg = "linking failed (" + cxx + " exited with code " +
                          std::to_string(rc) + ")";
        if (!opts.target.empty())
            msg += "\nnote: cross-compiling to " + opts.target +
                   " also needs a sysroot for that platform (--sysroot) and a copy of the "
                   "runtime built for it (point KAMIPY_RT_LIB at the cross-built "
                   "libkamirt.a)";
        throw std::runtime_error(msg);
    }
    return out.string();
}

} // namespace kami
