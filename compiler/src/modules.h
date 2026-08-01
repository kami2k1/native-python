#pragma once
// KamiPython import subsystem: where does `import X` come from?
//
// Four sources, consulted in this order:
//   1. Project      — X.py (or X/__init__.py) next to the file being compiled.
//   2. Native       — a module the runtime implements as a C-ABI primitive
//                     (math, os, json, socket, ...). Nothing to bundle.
//   3. System Python— the CPython installation found on this machine.
//   4. Bundled pylib— the pure-Python library shipped next to the kamipy binary.
//
// (2) sits above (3) on purpose: `import json` should keep using the native
// primitive, which is both faster and already verified, instead of dragging in
// CPython's json/ package. (3) sits above (4) so that the real library on the
// machine is what gets compiled — no manual copying of a stdlib/ folder — while
// (4) still catches the case where CPython is absent, or where its source uses
// syntax this compiler does not accept yet. A candidate that fails to *parse*
// falls through to the next one, which is what makes the ordering safe.
#include "ast.h"

#include <filesystem>
#include <string>
#include <vector>

namespace kami {

// Import roots of the Python installation(s) on this machine, best first.
// Computed once and cached. Sources, in priority order:
//   $KAMIPY_PYTHON_STDLIB, $PYTHONHOME, $PYTHONPATH, the interpreter found on
//   $PATH, then a platform scan (Windows registry + the usual install prefixes;
//   /usr/lib/python3.*, /usr/local/lib/python3.* and friends on Unix).
const std::vector<std::filesystem::path>& find_system_python_stdlib();

// Third-party package directories (site-packages / dist-packages) belonging
// to the discovered Python installations, best first. Pure-Python packages
// found here are compiled in when possible; everything else is bridged
// through the embedded CPython interpreter at runtime (KT_PYOBJ).
const std::vector<std::filesystem::path>& find_site_packages();

// Does a third-party package named `dotted` exist in any site-packages dir —
// as pure-Python source, a package directory, or a compiled C extension
// (.so/.pyd)? Used to route unknown imports through the CPython bridge.
bool site_packages_has(const std::string& dotted);

enum class ModuleOrigin { NotFound, Project, Pylib, Native, SystemPython, SitePackages };

struct ResolvedModule {
    ModuleOrigin origin = ModuleOrigin::NotFound;
    std::filesystem::path path; // .py file, empty for Native/NotFound
};

struct ImportPolicy {
    std::filesystem::path project_dir;               // where the input file lives
    std::vector<std::filesystem::path> pylib_dirs;   // shipped pure-Python library
    bool use_system_python = true;                   // --no-system-stdlib turns this off
    bool verbose = false;                            // report each resolution on stderr
};

// Every place `dotted` could come from, best first. Pure lookup: no parsing,
// no side effects. A Native result is always alone and always last-resort-free.
std::vector<ResolvedModule> resolve_module(const std::string& dotted, const ImportPolicy& pol);

// Splice every importable module reachable from `mod` into `mod.body`,
// dependency-first, so the whole program compiles into one native executable.
// Records what came from where in mod.user_modules / mod.system_modules.
void bundle_imported_modules(Module& mod, const ImportPolicy& pol);

// Symbol prefix used for a library module's top-level names ("re" ->
// "std_re_"), and the AST rename that applies it.
std::string library_prefix(const std::string& dotted);
void mangle_module(std::vector<StmtPtr>& body, const std::string& prefix);

// The global name a library module's top-level binding ends up under after
// mangling. Dunders (__version__, __all__) are NOT prefixed (mangle_module
// skips them), so every name mapping must go through this helper.
std::string mangled_library_name(const std::string& prefix, const std::string& name);

// Default pylib search path for a kamipy binary living in `bindir`.
std::vector<std::filesystem::path> bundled_pylib_dirs(const std::filesystem::path& bindir);

} // namespace kami
