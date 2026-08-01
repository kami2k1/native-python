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
    // functional
    KB_MAP, KB_FILTER,
    // ---- module _kami: thin C-ABI bindings to libc / the OS ----------------
    // The Python standard library (stdlib/*.py) is compiled from source and
    // reaches the operating system exclusively through these primitives.
    KB_SYS_FD_OPEN, KB_SYS_FD_READ, KB_SYS_FD_WRITE, KB_SYS_FD_CLOSE, KB_SYS_FD_SEEK,
    KB_SYS_STAT, KB_SYS_FILESIZE, KB_SYS_LISTDIR, KB_SYS_MKDIR, KB_SYS_RMDIR,
    KB_SYS_UNLINK, KB_SYS_RENAME, KB_SYS_GETCWD, KB_SYS_CHDIR, KB_SYS_GETENV,
    KB_SYS_SYSTEM, KB_SYS_GETPID, KB_SYS_ERRMSG, KB_SYS_PLATFORM, KB_SYS_LOCALTIME,
    KB_SOCK_OPEN, KB_SOCK_CONNECT, KB_SOCK_BIND, KB_SOCK_LISTEN, KB_SOCK_ACCEPT,
    KB_SOCK_SEND, KB_SOCK_RECV, KB_SOCK_CLOSE, KB_SOCK_TIMEOUT,
    // diagnostics
    KB_KWARGS_UNSUPPORTED,
    KB__COUNT
};
