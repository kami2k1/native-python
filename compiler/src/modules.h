#pragma once
#include "ast.h"

#include <string>
#include <vector>

namespace kami {

// Translates every imported .py module (the Python stdlib in stdlib/, plus
// modules sitting next to the input file) into the program being compiled:
// each module's source is parsed, its module-level names are renamed into a
// namespace ("match" in re.py becomes the global "re__match"), and its
// top-level statements are spliced in front of the main body, dependencies
// first. mod.bundled records what was translated so sema can resolve
// "re.match" to the right global.
//
// Throws std::runtime_error on unreadable sources / circular imports and
// CompileError (with the module's file name in the message) on syntax errors.
void bundle_modules(Module& mod, const std::string& input_path, const std::string& argv0);

// Directories searched for stdlib modules, in order. Honours $KAMIPY_STDLIB.
std::vector<std::string> stdlib_search_dirs(const std::string& argv0);

// Rewrites a diagnostic that landed inside a translated module so it points at
// that module's own file and line. Returns the error unchanged otherwise.
CompileError locate_error(const Module& mod, const CompileError& e);

} // namespace kami
