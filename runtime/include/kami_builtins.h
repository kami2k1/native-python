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
    KB__COUNT
};
