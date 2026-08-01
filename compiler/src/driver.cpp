#include "driver.h"

#include "codegen.h"
#include "native_backend.h"
#include "lexer.h"
#include "modules.h"
#include "parser.h"
#include "sema.h"

#include "../../runtime/include/kami_runtime.h"

#include <cstdio>
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

// ---- import handling -------------------------------------------------------
// The heavy lifting lives in modules.cpp; the driver's job is to describe *this
// build* (where the input file is, where the shipped pylib is, whether the
// system Python may be used) and then hand the module over.

static ImportPolicy import_policy(const fs::path& input, const std::string& argv0,
                                  const BuildOptions& opts) {
    ImportPolicy pol;
    pol.project_dir = fs::absolute(input).parent_path();
    pol.pylib_dirs = bundled_pylib_dirs(self_dir(argv0));
    pol.use_system_python = opts.use_system_stdlib && !getenv("KAMIPY_NO_SYSTEM_STDLIB");
    pol.verbose = opts.verbose;
    return pol;
}

static std::string path_to_string(const fs::path& p) {
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

// Where would `import X` look for a module right now? Printed by
// `kamipy paths` so a broken setup can be diagnosed without guesswork.
std::string describe_import_paths(const std::string& argv0) {
    std::string out = "project directory: <alongside the input file>\n\nbundled pylib:\n";
    std::error_code ec;
    for (const fs::path& d : bundled_pylib_dirs(self_dir(argv0)))
        out += "  " + path_to_string(d) + (fs::is_directory(d, ec) ? "" : "   (missing)") + "\n";
    out += "\nsystem Python (auto-discovered):\n";
    const auto& sys = find_system_python_stdlib();
    if (sys.empty()) out += "  <none found — falling back to the bundled pylib>\n";
    for (const fs::path& d : sys) out += "  " + path_to_string(d) + "\n";
    out += "\nthird-party packages (site-packages / dist-packages):\n";
    const auto& site = find_site_packages();
    if (site.empty())
        out += "  <none found>\n";
    for (const fs::path& d : site) out += "  " + path_to_string(d) + "\n";
    return out;
}

std::string build(const BuildOptions& opts) {
    std::string src = read_file(opts.input);

    Module mod = parse(lex(src));
    mod.source_path = fs::absolute(opts.input).string();
    bundle_imported_modules(mod, import_policy(opts.input, opts.argv0, opts));
    analyze(mod);
    infer_types(mod);
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
    if (opts.emit_llvm) {
        std::ofstream f(ll, std::ios::binary);
        if (!f) throw std::runtime_error("cannot write " + ll.string());
        f << ir;
    }

    std::string rtlib = find_runtime_lib(opts.argv0);

    // ---- embedded backend: TargetMachine + LLD, no external toolchain ----
    // KAMIPY_CXX forces the external-compiler path (also the fallback when
    // kamipy was built without the LLVM libraries).
    if (native_backend_available() && !getenv("KAMIPY_CXX")) {
        fs::path obj = out;
        obj += ".o";
        compile_ir_to_object(ir, obj.string(), opts.opt_level);
        std::vector<std::string> libs;
#ifdef KAMI_RT_NEEDS_OPENSSL
        libs.push_back("ssl");
        libs.push_back("crypto");
#endif
        for (const std::string& lib : mod.link_libs) {
            if (lib == "m" || lib == "pthread" || lib == "c") continue;
#ifdef _WIN32
            if (lib == "ws2_32") continue;
#endif
            libs.push_back(lib);
        }
        if (opts.verbose)
            fprintf(stderr, "kamipy: embedded LLVM backend: %s -> %s\n",
                    obj.string().c_str(), out.string().c_str());
        try {
            link_executable({obj.string()}, rtlib, libs, out.string());
        } catch (...) {
            std::error_code ec;
            fs::remove(obj, ec);
            throw;
        }
        std::error_code ec;
        fs::remove(obj, ec);
#ifndef _WIN32
        fs::permissions(out,
                        fs::perms::owner_exec | fs::perms::group_exec |
                            fs::perms::others_exec,
                        fs::perm_options::add, ec);
#endif
        return out.string();
    }

    // ---- external toolchain fallback ----
    {
        std::ofstream f(ll, std::ios::binary);
        if (!f) throw std::runtime_error("cannot write " + ll.string());
        f << ir;
    }
    std::string cxx = getenv("KAMIPY_CXX") ? getenv("KAMIPY_CXX") : "clang++";
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
    // Libraries the program's C-ABI bindings need (ctypes.CDLL("libm.so.6"),
    // a C extension mapped onto ws2_32, ...). Duplicates are harmless.
    for (const std::string& lib : mod.link_libs) {
#ifndef _WIN32
        if (lib == "m" || lib == "pthread") continue; // already on the command line
#else
        if (lib == "ws2_32" || lib == "m") continue;
#endif
        cmd.push_back("-l" + lib);
    }
    if (opts.verbose && !mod.link_libs.empty()) {
        std::string libs;
        for (const std::string& l : mod.link_libs) libs += " -l" + l;
        fprintf(stderr, "kamipy: C-ABI link flags:%s\n", libs.c_str());
    }
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
