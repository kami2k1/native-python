#pragma once
#include <string>
#include <vector>

namespace kami {

struct BuildOptions {
    std::string input;
    std::string output;      // default: input basename without extension
    bool emit_llvm = false;  // keep the .ll next to the output
    bool emit_ast = false;   // print the AST dump and stop
    int opt_level = 2;
    std::string argv0;       // for locating libkamirt
};

// Returns the produced executable path. Throws std::runtime_error / CompileError.
std::string build(const BuildOptions& opts);

// Run one external command (no shell — no injection). Returns exit code.
int run_process(const std::vector<std::string>& args);

std::string find_runtime_lib(const std::string& argv0);

} // namespace kami
