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
    runtime/src/syscalls.cpp
    runtime/src/builtins.cpp
)
target_include_directories(kamirt PUBLIC runtime/include)
target_link_libraries(kamirt PUBLIC Threads::Threads)
if(WIN32)
    target_link_libraries(kamirt PUBLIC ws2_32)
endif()
target_compile_options(kamirt PRIVATE ${KAMI_RT_OPTS})

# ---- compiler ----
add_library(kamipy_core STATIC
    compiler/src/lexer.cpp
    compiler/src/parser.cpp
    compiler/src/sema.cpp
    compiler/src/modules.cpp
    compiler/src/codegen.cpp
    compiler/src/driver.cpp
)
target_include_directories(kamipy_core PUBLIC compiler/src runtime/include)

add_executable(kamipy cli/main.cpp)
target_link_libraries(kamipy PRIVATE kamipy_core)

# kamipy expects libkamirt.a next to its own binary
add_dependencies(kamipy kamirt)

# ---- Python standard library ----
# stdlib/*.py is translated from source by kamipy on every "import", so it has to
# travel with the binary: copy it next to kamipy after every build (this also
# covers multi-config generators, where the binary lands in build/<Config>/), and
# install it under lib/kamipy/stdlib. See stdlib_search_dirs().
add_custom_command(TARGET kamipy POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_directory
            ${CMAKE_CURRENT_SOURCE_DIR}/stdlib $<TARGET_FILE_DIR:kamipy>/stdlib
    COMMENT "Copying the Python standard library (stdlib/) next to kamipy")

install(TARGETS kamipy RUNTIME DESTINATION bin)
install(TARGETS kamirt ARCHIVE DESTINATION bin)
install(DIRECTORY stdlib/ DESTINATION lib/kamipy/stdlib FILES_MATCHING PATTERN "*.py")

# ---- tests ----
enable_testing()

add_executable(test_lexer tests/unit/test_lexer.cpp)
target_link_libraries(test_lexer PRIVATE kamipy_core)
add_test(NAME unit.lexer COMMAND test_lexer)

add_executable(test_parser tests/unit/test_parser.cpp)
target_link_libraries(test_parser PRIVATE kamipy_core)
add_test(NAME unit.parser COMMAND test_parser)

add_test(NAME integration
         COMMAND bash ${CMAKE_CURRENT_SOURCE_DIR}/tests/integration/run_tests.sh
                 $<TARGET_FILE:kamipy>)
set_tests_properties(integration PROPERTIES TIMEOUT 600)

# The same programs, but with the GC collecting before every allocation: a value
# that is not reachable from a registered root fails immediately and
# reproducibly instead of corrupting the heap much later.
add_test(NAME integration.gc_stress
         COMMAND bash ${CMAKE_CURRENT_SOURCE_DIR}/tests/integration/run_tests.sh
                 $<TARGET_FILE:kamipy>)
set_tests_properties(integration.gc_stress PROPERTIES
                     TIMEOUT 900 ENVIRONMENT "KAMIPY_GC_STRESS=1")
