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
    runtime/src/oslayer.cpp
    runtime/src/netsock.cpp
    runtime/src/builtins.cpp
    runtime/src/syscalls.cpp
)
target_include_directories(kamirt PUBLIC runtime/include)
target_link_libraries(kamirt PUBLIC Threads::Threads)
if(WIN32)
    # secur32/crypt32: SChannel, the OS TLS stack behind socket.wrap_tls()
    target_link_libraries(kamirt PUBLIC ws2_32 secur32 crypt32)
else()
    # OpenSSL (optional): the TLS provider behind socket.wrap_tls(). Plain
    # http:// works without it; https:// then raises SSLError.
    find_package(OpenSSL QUIET)
    if(OpenSSL_FOUND)
        target_compile_definitions(kamirt PRIVATE KAMI_HAVE_OPENSSL)
        target_link_libraries(kamirt PUBLIC OpenSSL::SSL OpenSSL::Crypto)
    else()
        message(STATUS "OpenSSL not found — https:// will raise SSLError at runtime")
    endif()
endif()
target_compile_options(kamirt PRIVATE ${KAMI_RT_OPTS})

# ---- compiler ----
add_library(kamipy_core STATIC
    compiler/src/lexer.cpp
    compiler/src/parser.cpp
    compiler/src/sema.cpp
    compiler/src/codegen.cpp
    compiler/src/cext.cpp
    compiler/src/modules.cpp
    compiler/src/driver.cpp
)
target_include_directories(kamipy_core PUBLIC compiler/src runtime/include)
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
# A separate ALL target (rather than a POST_BUILD step on kamipy) so that
# editing a .py file is enough to refresh the shipped copy.
add_custom_target(kamipy_pylib ALL
    COMMAND ${CMAKE_COMMAND} -E copy_directory
            ${CMAKE_CURRENT_SOURCE_DIR}/runtime/pylib
            $<TARGET_FILE_DIR:kamipy>/pylib
    DEPENDS ${CMAKE_CURRENT_SOURCE_DIR}/runtime/pylib
    COMMENT "Copying Python stdlib (pylib/) next to kamipy")
add_dependencies(kamipy_pylib kamipy)

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
