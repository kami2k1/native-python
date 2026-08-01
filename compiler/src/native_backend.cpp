// Embedded LLVM TargetMachine + LLD linker (see native_backend.h).
//
// Built only when CMake finds the LLVM + LLD development libraries
// (KAMI_EMBED_LLVM); otherwise native_backend_stub.cpp is compiled instead
// and the driver falls back to invoking an external clang++.
#include "native_backend.h"

#include <cstdlib>
#include <filesystem>
#include <stdexcept>

#include "lld/Common/Driver.h"
#include "llvm/IR/LLVMContext.h"
#include "llvm/IR/LegacyPassManager.h"
#include "llvm/IR/Module.h"
#include "llvm/IR/Verifier.h"
#include "llvm/IRReader/IRReader.h"
#include "llvm/MC/TargetRegistry.h"
#include "llvm/Passes/PassBuilder.h"
#include "llvm/Support/FileSystem.h"
#include "llvm/Support/SourceMgr.h"
#include "llvm/Support/TargetSelect.h"
#include "llvm/Support/raw_ostream.h"
#include "llvm/Target/TargetMachine.h"
#include "llvm/TargetParser/Host.h"

#ifdef _WIN32
LLD_HAS_DRIVER(coff)
#else
LLD_HAS_DRIVER(elf)
#endif

namespace fs = std::filesystem;

namespace kami {

bool native_backend_available() { return true; }

static void init_native_target_once() {
    static bool done = false;
    if (done) return;
    llvm::InitializeNativeTarget();
    llvm::InitializeNativeTargetAsmPrinter();
    llvm::InitializeNativeTargetAsmParser();
    done = true;
}

void compile_ir_to_object(const std::string& ir, const std::string& obj_path,
                          int opt_level) {
    init_native_target_once();

    llvm::LLVMContext ctx;
    llvm::SMDiagnostic diag;
    std::unique_ptr<llvm::Module> mod = llvm::parseIR(
        llvm::MemoryBufferRef(ir, "kamipy-module"), diag, ctx);
    if (!mod) {
        std::string msg;
        llvm::raw_string_ostream os(msg);
        diag.print("kamipy", os, /*ShowColors=*/false);
        throw std::runtime_error("internal error: generated IR is invalid:\n" + os.str());
    }

    std::string triple = llvm::sys::getDefaultTargetTriple();
    mod->setTargetTriple(triple);
    std::string err;
    const llvm::Target* target = llvm::TargetRegistry::lookupTarget(triple, err);
    if (!target) throw std::runtime_error("no native target: " + err);

    llvm::TargetOptions topts;
    std::unique_ptr<llvm::TargetMachine> tm(target->createTargetMachine(
        triple, llvm::sys::getHostCPUName(), /*Features=*/"", topts,
        llvm::Reloc::PIC_, std::nullopt,
        opt_level >= 2 ? llvm::CodeGenOptLevel::Aggressive
                       : llvm::CodeGenOptLevel::Default));
    if (!tm) throw std::runtime_error("cannot create target machine for " + triple);
    mod->setDataLayout(tm->createDataLayout());

    // mid-level optimization: the standard O1/O2/O3 pipelines (new PM)
    {
        llvm::LoopAnalysisManager lam;
        llvm::FunctionAnalysisManager fam;
        llvm::CGSCCAnalysisManager cam;
        llvm::ModuleAnalysisManager mam;
        llvm::PassBuilder pb(tm.get());
        pb.registerModuleAnalyses(mam);
        pb.registerCGSCCAnalyses(cam);
        pb.registerFunctionAnalyses(fam);
        pb.registerLoopAnalyses(lam);
        pb.crossRegisterProxies(lam, fam, cam, mam);
        llvm::OptimizationLevel lvl = opt_level <= 0   ? llvm::OptimizationLevel::O0
                                      : opt_level == 1 ? llvm::OptimizationLevel::O1
                                      : opt_level == 2 ? llvm::OptimizationLevel::O2
                                                       : llvm::OptimizationLevel::O3;
        llvm::ModulePassManager mpm =
            opt_level <= 0 ? pb.buildO0DefaultPipeline(lvl)
                           : pb.buildPerModuleDefaultPipeline(lvl);
        mpm.run(*mod, mam);
    }

    // native code generation straight to the object file
    std::error_code ec;
    llvm::raw_fd_ostream out(obj_path, ec, llvm::sys::fs::OF_None);
    if (ec) throw std::runtime_error("cannot write " + obj_path + ": " + ec.message());
    llvm::legacy::PassManager cgpm;
    if (tm->addPassesToEmitFile(cgpm, out, nullptr, llvm::CodeGenFileType::ObjectFile))
        throw std::runtime_error("target cannot emit object files");
    cgpm.run(*mod);
    out.flush();
}

#ifndef _WIN32
// ---- ELF link (Linux) ------------------------------------------------------
// lld needs the CRT startup objects and the system library paths that a
// compiler driver would normally supply. They come from libc-dev/gcc's
// runtime files, which exist on any machine that can run dynamically linked
// binaries with development headers — no clang or binutils required.
static fs::path find_first(const std::vector<fs::path>& cands) {
    for (const fs::path& c : cands)
        if (fs::exists(c)) return c;
    return {};
}

static fs::path find_gcc_crt_dir() {
    // /usr/lib/gcc/<triple>/<version>/crtbeginS.o (pick the highest version)
    std::vector<fs::path> roots = {"/usr/lib/gcc/x86_64-linux-gnu",
                                   "/usr/lib/gcc/aarch64-linux-gnu",
                                   "/usr/lib64/gcc/x86_64-pc-linux-gnu",
                                   "/usr/lib/gcc"};
    fs::path best;
    std::error_code ec;
    for (const fs::path& root : roots) {
        if (!fs::is_directory(root, ec)) continue;
        for (auto& entry : fs::directory_iterator(root, ec)) {
            if (!entry.is_directory()) continue;
            if (fs::exists(entry.path() / "crtbeginS.o") &&
                (best.empty() || entry.path().filename().string() >
                                     best.filename().string()))
                best = entry.path();
        }
        if (!best.empty()) break;
    }
    return best;
}

static std::vector<std::string> system_lib_dirs() {
    std::vector<std::string> dirs;
    for (const char* d :
         {"/usr/lib/x86_64-linux-gnu", "/usr/lib/aarch64-linux-gnu", "/usr/lib64",
          "/usr/lib", "/lib/x86_64-linux-gnu", "/lib64", "/lib"}) {
        std::error_code ec;
        if (fs::is_directory(d, ec)) dirs.push_back(d);
    }
    return dirs;
}

void link_executable(const std::vector<std::string>& objects,
                     const std::string& runtime_lib,
                     const std::vector<std::string>& libs,
                     const std::string& output) {
    std::vector<std::string> dirs = system_lib_dirs();
    auto in_dirs = [&](const char* name) -> fs::path {
        for (const std::string& d : dirs)
            if (fs::exists(fs::path(d) / name)) return fs::path(d) / name;
        return {};
    };
    fs::path scrt1 = in_dirs("Scrt1.o");
    fs::path crti = in_dirs("crti.o");
    fs::path crtn = in_dirs("crtn.o");
    fs::path gccdir = find_gcc_crt_dir();
    fs::path dyld = find_first({"/lib64/ld-linux-x86-64.so.2",
                                "/lib/ld-linux-aarch64.so.1",
                                "/lib/ld-linux.so.2", "/lib/ld-musl-x86_64.so.1"});
    if (scrt1.empty() || crti.empty() || crtn.empty() || gccdir.empty() || dyld.empty())
        throw std::runtime_error(
            "embedded linker: C runtime startup files not found (need libc6-dev "
            "and the gcc runtime, or set KAMIPY_CXX to use an external compiler)");

    std::vector<std::string> args = {
        "ld.lld", "--eh-frame-hdr", "-pie",
        "-dynamic-linker", dyld.string(),
        "-o", output,
        scrt1.string(), crti.string(), (gccdir / "crtbeginS.o").string(),
    };
    for (const std::string& o : objects) args.push_back(o);
    args.push_back(runtime_lib);
    args.push_back("-L" + gccdir.string());
    for (const std::string& d : dirs) args.push_back("-L" + d);
    args.push_back("--gc-sections");
    args.push_back("--as-needed");
    for (const std::string& l : libs) args.push_back("-l" + l);
    for (const char* l : {"stdc++", "m", "gcc_s", "gcc", "pthread", "c"})
        args.push_back(std::string("-l") + l);
    // libgcc again after libc (soft-float helpers on some targets)
    args.push_back("-lgcc_s");
    args.push_back("-lgcc");
    args.push_back((gccdir / "crtendS.o").string());
    args.push_back(crtn.string());

    std::vector<const char*> argv;
    argv.reserve(args.size());
    for (const std::string& a : args) argv.push_back(a.c_str());

    std::string errbuf;
    llvm::raw_string_ostream errs(errbuf);
    lld::Result res =
        lld::lldMain(argv, llvm::outs(), errs, {{lld::Gnu, &lld::elf::link}});
    if (res.retCode != 0)
        throw std::runtime_error("embedded linker failed:\n" + errs.str());
}
#else
// ---- COFF link (Windows) ---------------------------------------------------
// Uses the MSVC/Windows-SDK libraries found through the LIB environment
// variable (set by a developer prompt) or default install locations.
void link_executable(const std::vector<std::string>& objects,
                     const std::string& runtime_lib,
                     const std::vector<std::string>& libs,
                     const std::string& output) {
    std::vector<std::string> args = {"lld-link", "/out:" + output, "/nologo"};
    for (const std::string& o : objects) args.push_back(o);
    args.push_back(runtime_lib);
    args.push_back("ws2_32.lib");
    args.push_back("winhttp.lib");
    for (const std::string& l : libs) args.push_back(l + ".lib");
    args.push_back("/defaultlib:libcmt");
    std::vector<const char*> argv;
    for (const std::string& a : args) argv.push_back(a.c_str());
    std::string errbuf;
    llvm::raw_string_ostream errs(errbuf);
    lld::Result res =
        lld::lldMain(argv, llvm::outs(), errs, {{lld::WinLink, &lld::coff::link}});
    if (res.retCode != 0)
        throw std::runtime_error("embedded linker failed:\n" + errs.str());
}
#endif

} // namespace kami
