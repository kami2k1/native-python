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
    add_compile_options(/utf-8 /EHsc)
    set(KAMI_RT_OPTS /O2)
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
)
target_include_directories(kamirt PUBLIC runtime/include)
target_link_libraries(kamirt PUBLIC Threads::Threads)
target_compile_options(kamirt PRIVATE ${KAMI_RT_OPTS})

# ---- compiler ----
add_library(kamipy_core STATIC
    compiler/src/lexer.cpp
    compiler/src/parser.cpp
    compiler/src/sema.cpp
    compiler/src/codegen.cpp
    compiler/src/driver.cpp
)
target_include_directories(kamipy_core PUBLIC compiler/src runtime/include)

add_executable(kamipy cli/main.cpp)
target_link_libraries(kamipy PRIVATE kamipy_core)

# kamipy expects libkamirt.a next to its own binary
add_dependencies(kamipy kamirt)

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
