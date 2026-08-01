cmake_minimum_required(VERSION 3.20)
project(KamiPython CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
if(NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE Release)
endif()

find_package(Threads REQUIRED)

# Compiler-specific flags (MSVC vs GCC/Clang)
if(MSVC)
    # /EHs (NOT /EHsc): the runtime raises Python exceptions as C++ exceptions
    # across extern "C" boundaries (kami_raise → kami_try). With /EHc MSVC
    # assumes extern "C" never throws and may delete the catch handler,
    # breaking try/except at runtime (and emitting warning C4297).
    add_compile_options(/utf-8 /EHs)
    set(KAMI_RT_OPTS $<$<NOT:$<CONFIG:Debug>>:/O2>)
else()
    set(KAMI_RT_OPTS -O2)
endif()

# ---- runtime library (linked into every compiled program) ----
add_library(kamirt STATIC
    runtime/src/gc.cpp
    runtime/src/value.cpp
    runtime/src/collections.cpp
    runtime/src/ops.cpp
    runtime/src/objects.cpp
    runtime/src/builtins.cpp
    runtime/src/netio.cpp
    runtime/src/http_client.cpp
    runtime/src/kami_regex.cpp
    runtime/src/syscalls.cpp
)
target_include_directories(kamirt PUBLIC runtime/include)
target_link_libraries(kamirt PUBLIC Threads::Threads)
if(WIN32)
    # winhttp: native HTTP/HTTPS client for `requests` (TLS via SChannel)
    target_link_libraries(kamirt PUBLIC ws2_32 winhttp)
else()
    # OpenSSL (optional): TLS for https:// in the native `requests` client.
    # Plain http:// works without it.
    find_package(OpenSSL QUIET)
    if(OpenSSL_FOUND)
        target_compile_definitions(kamirt PRIVATE KAMI_HAVE_OPENSSL)
        target_link_libraries(kamirt PUBLIC OpenSSL::SSL OpenSSL::Crypto)
    else()
        message(STATUS "OpenSSL not found — requests will support http:// only")
    endif()
endif()
target_compile_options(kamirt PRIVATE ${KAMI_RT_OPTS})

# ---- compiler ----
# Embedded LLVM backend + LLD linker (Part III): when the LLVM and LLD
# development libraries are available, kamipy compiles IR to machine code in
# process and links executables itself — no clang/gcc/ld on the user machine.
# Without them, kamipy falls back to invoking an external clang++.
find_package(LLVM CONFIG QUIET)
find_package(LLD CONFIG QUIET)
if(NOT LLVM_FOUND OR NOT LLD_FOUND)
    # Debian/Ubuntu install versioned LLVM outside the default search path.
    foreach(v RANGE 21 14 -1)
        if(EXISTS "/usr/lib/llvm-${v}/lib/cmake/llvm")
            find_package(LLVM CONFIG QUIET PATHS "/usr/lib/llvm-${v}/lib/cmake/llvm" NO_DEFAULT_PATH)
            find_package(LLD CONFIG QUIET PATHS "/usr/lib/llvm-${v}/lib/cmake/lld" NO_DEFAULT_PATH)
            if(LLVM_FOUND AND LLD_FOUND)
                break()
            endif()
        endif()
    endforeach()
endif()
if(LLVM_FOUND AND LLD_FOUND)
    message(STATUS "Embedding LLVM ${LLVM_PACKAGE_VERSION} + LLD (self-contained kamipy)")
    set(KAMI_EMBED_LLVM ON)
else()
    message(STATUS "LLVM/LLD dev libraries not found — kamipy will call an external clang++")
    set(KAMI_EMBED_LLVM OFF)
endif()

add_library(kamipy_core STATIC
    compiler/src/lexer.cpp
    compiler/src/parser.cpp
    compiler/src/sema.cpp
    compiler/src/typeinf.cpp
    compiler/src/codegen.cpp
    compiler/src/cext.cpp
    compiler/src/modules.cpp
    compiler/src/driver.cpp
    $<IF:$<BOOL:${KAMI_EMBED_LLVM}>,compiler/src/native_backend.cpp,compiler/src/native_backend_stub.cpp>
)
target_include_directories(kamipy_core PUBLIC compiler/src runtime/include)
if(KAMI_EMBED_LLVM)
    target_include_directories(kamipy_core SYSTEM PRIVATE ${LLVM_INCLUDE_DIRS} ${LLD_INCLUDE_DIRS})
    target_compile_definitions(kamipy_core PRIVATE ${LLVM_DEFINITIONS} KAMI_EMBED_LLVM)
    set_source_files_properties(compiler/src/native_backend.cpp PROPERTIES
        COMPILE_FLAGS "$<IF:$<CXX_COMPILER_ID:MSVC>,/GR-,-fno-rtti>")
    llvm_map_components_to_libnames(KAMI_LLVM_LIBS
        core irreader support passes native)
    if(WIN32)
        set(KAMI_LLD_LIBS lldCOFF lldCommon)
    else()
        set(KAMI_LLD_LIBS lldELF lldCommon)
    endif()
    target_link_libraries(kamipy_core PRIVATE ${KAMI_LLD_LIBS} ${KAMI_LLVM_LIBS})
endif()
if(NOT WIN32 AND OpenSSL_FOUND)
    # The driver must link compiled programs against the same TLS libs the
    # runtime was built with (see driver.cpp link command).
    target_compile_definitions(kamipy_core PRIVATE KAMI_RT_NEEDS_OPENSSL)
endif()

add_executable(kamipy cli/main.cpp)
target_link_libraries(kamipy PRIVATE kamipy_core)

# kamipy expects libkamirt.a next to its own binary
add_dependencies(kamipy kamirt)

# Ship the pure-Python standard library (itertools, functools, ...) next to the
# kamipy binary so the driver can bundle it on import. The library is Python —
# compiled by the same pipeline, never reimplemented in C++.
add_custom_command(TARGET kamipy POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_directory
            ${CMAKE_CURRENT_SOURCE_DIR}/runtime/pylib
            $<TARGET_FILE_DIR:kamipy>/pylib
    COMMENT "Copying Python stdlib (pylib/) next to kamipy")

# ---- tests ----
enable_testing()

add_executable(test_lexer tests/unit/test_lexer.cpp)
target_link_libraries(test_lexer PRIVATE kamipy_core)
add_test(NAME unit.lexer COMMAND test_lexer)

add_executable(test_parser tests/unit/test_parser.cpp)
target_link_libraries(test_parser PRIVATE kamipy_core)
add_test(NAME unit.parser COMMAND test_parser)

add_executable(test_cabi tests/unit/test_cabi.cpp)
target_link_libraries(test_cabi PRIVATE kamipy_core)
add_test(NAME unit.cabi COMMAND test_cabi)

add_test(NAME integration
         COMMAND bash ${CMAKE_CURRENT_SOURCE_DIR}/tests/integration/run_tests.sh
                 $<TARGET_FILE:kamipy>)
set_tests_properties(integration PROPERTIES TIMEOUT 600)
