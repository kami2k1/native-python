#pragma once
// Shared builtin IDs between compiler and runtime.
#include <stdint.h>

enum KamiBuiltinId : int64_t {
    KB_PRINT = 0,
    KB_LEN,
    KB_STR,
    KB_INT,
    KB_FLOAT,
    KB_ABS,
    KB_MIN,
    KB_MAX,
    KB_ORD,
    KB_CHR,
    KB_TYPE,
    KB_RANGE,
    // math
    KB_MATH_SQRT,
    KB_MATH_SIN,
    KB_MATH_COS,
    KB_MATH_TAN,
    KB_MATH_EXP,
    KB_MATH_LOG,
    KB_MATH_POW,
    KB_MATH_FLOOR,
    KB_MATH_CEIL,
    KB_MATH_FABS,
    // time
    KB_TIME_TIME,
    KB_TIME_SLEEP,
    // random
    KB_RANDOM_RANDOM,
    KB_RANDOM_RANDINT,
    KB_RANDOM_SEED,
    // threading
    KB_THREAD_SPAWN,
    KB_THREAD_JOIN,
    // extended builtins (v0.2)
    KB_SUM,
    KB_SORTED,
    KB_REVERSED,
    KB_ENUMERATE,
    KB_ZIP,
    KB_BOOL,
    KB_ROUND,
    KB_INPUT,
    KB_POW,
    KB_PRINT_EX, // args: [sep, end, values...]
    KB_ALL,
    KB_ANY,
    KB_BIN,
    KB_HEX,
    KB_OCT,
    KB_LIST,
    KB_DICT,
    KB_TUPLE,
    KB_ISINSTANCE,
    KB_FORMAT,
    KB_NOOP,      // doctest.testmod etc.
    KB_SYS_EXIT,
    KB_SYS_ARGV,
    KB_DIVMOD,
    KB_SET,
    KB_OPEN,
    KB_WITH_ENTER,
    KB_WITH_EXIT,
    // os / os.path
    KB_OS_GETCWD, KB_OS_LISTDIR, KB_OS_REMOVE, KB_OS_MKDIR, KB_OS_MAKEDIRS,
    KB_OS_RMDIR, KB_OS_RENAME, KB_OS_SYSTEM, KB_OS_GETENV,
    KB_OSP_EXISTS, KB_OSP_ISFILE, KB_OSP_ISDIR, KB_OSP_JOIN, KB_OSP_BASENAME,
    KB_OSP_DIRNAME, KB_OSP_GETSIZE, KB_OSP_ABSPATH,
    // logging (level encoded as first arg by sema)
    KB_LOG_BASICCONFIG, KB_LOG_LOG,
    // json
    KB_JSON_LOADS, KB_JSON_DUMPS,
    // socket
    KB_SOCKET_SOCKET,
    // requests
    KB_REQUESTS_GET, KB_REQUESTS_POST,
    // functional
    KB_MAP, KB_FILTER,
    // re
    KB_RE_MATCH, KB_RE_SEARCH, KB_RE_FULLMATCH, KB_RE_FINDALL, KB_RE_SUB, KB_RE_SPLIT,
    // math (v0.5.1 additions)
    KB_MATH_FACTORIAL, KB_MATH_GCD, KB_MATH_ISQRT, KB_MATH_HYPOT, KB_MATH_LOG2,
    KB_MATH_LOG10, KB_MATH_ATAN, KB_MATH_ASIN, KB_MATH_ACOS, KB_MATH_ATAN2,
    KB_MATH_DEGREES, KB_MATH_RADIANS, KB_MATH_TRUNC, KB_MATH_ISNAN, KB_MATH_ISINF,
    // random (additions)
    KB_RANDOM_RANDRANGE, KB_RANDOM_CHOICE, KB_RANDOM_SHUFFLE, KB_RANDOM_UNIFORM,
    KB_RANDOM_SAMPLE, KB_RANDOM_CHOICES,
    // os (additions)
    KB_OS_WALK, KB_OS_CHDIR, KB_OS_GETPID, KB_OS_URANDOM,
    KB_OSP_EXPANDUSER, KB_OSP_SPLITEXT, KB_OSP_SPLIT, KB_OSP_ISABS,
    // sys (argv already exists as KB_SYS_ARGV)
    // time (additions)
    KB_TIME_MONOTONIC, KB_TIME_PERF_COUNTER,
    // diagnostics
    KB_KWARGS_UNSUPPORTED,
    // raw C-ABI primitives (runtime/src/syscalls.cpp) — the landing pads for
    // CPython's C extension modules: posix/_os, _socket, _struct.
    KB_SYS_OPEN, KB_SYS_READ, KB_SYS_WRITE, KB_SYS_CLOSE, KB_SYS_LSEEK,
    KB_SYS_GETHOSTNAME,
    KB_STRUCT_PACK, KB_STRUCT_UNPACK, KB_STRUCT_CALCSIZE,
    KB__COUNT
};
