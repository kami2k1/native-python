// Fallback when kamipy is built without the LLVM/LLD development libraries:
// the driver shells out to an external clang++ exactly as before.
#include "native_backend.h"

#include <stdexcept>

namespace kami {

bool native_backend_available() { return false; }

void compile_ir_to_object(const std::string&, const std::string&, int) {
    throw std::runtime_error("kamipy was built without the embedded LLVM backend");
}

void link_executable(const std::vector<std::string>&, const std::string&,
                     const std::vector<std::string>&, const std::string&) {
    throw std::runtime_error("kamipy was built without the embedded LLD linker");
}

} // namespace kami
