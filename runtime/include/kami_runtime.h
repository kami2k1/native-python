#pragma once
// KamiPython native runtime — public C ABI used by generated code.
//
// Design notes:
//  * Every dynamic value is a 16-byte tagged union (KamiValue).
//  * All heap objects are managed by a stop-allocation mark&sweep GC.
//  * Generated code NEVER stores object pointers in registers across calls:
//    values only live in registered frame slots, globals, or pins, and every
//    mutation goes through a runtime call that holds the global runtime lock
//    (GIL-style). This makes the GC and threading correct by construction.
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct KamiValue {
    int64_t tag;
    union {
        int64_t i;   // INT / BOOL
        double  f;   // FLOAT
        void*   p;   // heap object (STR/LIST/MAP/FUNC/THREAD)
    };
} KamiValue;

enum KamiTag : int64_t {
    KT_NONE = 0,
    KT_BOOL = 1,
    KT_INT = 2,
    KT_FLOAT = 3,
    KT_STR = 4,
    KT_LIST = 5,
    KT_MAP = 6,
    KT_FUNC = 7,
    KT_THREAD = 8,
    KT_CLASS = 9,
    KT_OBJECT = 10,
    KT_SET = 11,
    KT_FILE = 12,
    KT_SOCKET = 13,
    KT_LOCK = 14,
};

enum KamiBinOp : int64_t {
    KOP_ADD = 0, KOP_SUB, KOP_MUL, KOP_DIV, KOP_FLOORDIV, KOP_MOD,
    KOP_EQ, KOP_NE, KOP_LT, KOP_GT, KOP_LE, KOP_GE, KOP_IN,
    KOP_POW, KOP_IS, KOP_ISNOT,
    KOP_BITAND, KOP_BITOR, KOP_BITXOR, KOP_SHL, KOP_SHR,
};
enum KamiUnOp : int64_t { KUOP_NEG = 0, KUOP_NOT = 1, KUOP_INV = 2 };

// Calling convention for compiled user functions.
typedef void (*KamiFn)(KamiValue* ret, KamiValue** argv, int64_t nargs,
                       KamiValue* captures);

// --- lifecycle ---
void kami_rt_init(int64_t argc, char** argv);
void kami_rt_shutdown(void);

// --- GC roots ---
void kami_globals_init(int64_t n);
void kami_global_get(KamiValue* out, int64_t idx);
void kami_global_set(int64_t idx, const KamiValue* v);
void kami_global_make_func(int64_t idx, void* fnptr, int64_t min_arity,
                           int64_t arity, const char* name);
void kami_frame_push(KamiValue* slots, int64_t n); // zeroes slots, registers as roots
void kami_frame_pop(void);

// --- value constructors / moves ---
void kami_copy(KamiValue* dst, const KamiValue* src);
void kami_make_none(KamiValue* out);
void kami_make_bool(KamiValue* out, int64_t b);
void kami_make_int(KamiValue* out, int64_t v);
void kami_make_float(KamiValue* out, double v);
void kami_make_str(KamiValue* out, const char* data, int64_t len);
void kami_make_list(KamiValue* out, KamiValue** items, int64_t n);
// `*args` prologue: packs argv[first..nargs) into a list stored in *out.
void kami_pack_varargs(KamiValue* out, KamiValue** argv, int64_t nargs, int64_t first);
void kami_make_map(KamiValue* out);
void kami_make_builtin_func(KamiValue* out, int64_t builtin_id, const char* name);
// Build a closure: copies ncap captured values (given as an array of slot
// pointers) into the function object. out must be a rooted slot.
void kami_make_closure(KamiValue* out, void* fnptr, int64_t min_arity, int64_t arity,
                       const char* name, KamiValue** capture_slots, int64_t ncap);
void kami_make_set(KamiValue* out);
void kami_set_add(KamiValue* set, const KamiValue* v);

// --- operations ---
int32_t kami_truthy(const KamiValue* v);
void kami_binop(int64_t op, KamiValue* out, const KamiValue* a, const KamiValue* b);
void kami_unop(int64_t op, KamiValue* out, const KamiValue* a);
void kami_index_get(KamiValue* out, const KamiValue* obj, const KamiValue* idx);
void kami_index_set(KamiValue* obj, const KamiValue* idx, const KamiValue* val);
void kami_del_index(KamiValue* obj, const KamiValue* idx);
void kami_call_value(KamiValue* out, const KamiValue* fn, KamiValue** argv, int64_t nargs);
void kami_method(KamiValue* out, KamiValue* obj, const char* name, KamiValue** argv,
                 int64_t nargs);
void kami_builtin(int64_t id, KamiValue* out, KamiValue** argv, int64_t nargs);

// --- loop helpers ---
int32_t kami_range_cond(const KamiValue* i, const KamiValue* stop, const KamiValue* step);
// Converts an iterable into an index-able sequence (dict→keys, set→elements,
// file→lines, list/str→itself). out may alias seq.
void kami_iter_prep(KamiValue* out, const KamiValue* seq);
int32_t kami_iter_cond(const KamiValue* seq, const KamiValue* idx);
void kami_iter_get(KamiValue* out, const KamiValue* seq, const KamiValue* idx);
void kami_unpack(KamiValue* out, const KamiValue* seq, int64_t idx, int64_t expect_len);
// Python slice semantics; pass tag=NONE values for omitted start/stop/step.
void kami_slice(KamiValue* out, const KamiValue* obj, const KamiValue* start,
                const KamiValue* stop, const KamiValue* step);

// --- classes / attributes ---
void kami_global_make_class(int64_t idx, const char* name, int64_t parent_gidx);
void kami_class_add_method(int64_t cls_gidx, const char* name, void* fnptr,
                           int64_t min_arity, int64_t arity);
void kami_attr_get(KamiValue* out, const KamiValue* obj, const char* name);
void kami_attr_set(KamiValue* obj, const char* name, const KamiValue* val);

// --- exceptions ---
// Runs an outlined try-body (signature: int64_t body(KamiValue* frame)).
// Returns the body's code (0 normal, 1 return, 2 break, 3 continue) or -1 if
// a runtime error was caught; the message is then available via kami_last_error.
int64_t kami_try(void* body_fn, KamiValue* frame);
void kami_last_error(KamiValue* out);
void kami_raise(const KamiValue* msg);
void kami_rethrow(void);
// Top-level entry: runs the module body, catching runtime errors.
void kami_run_module(void* module_fn);

// --- diagnostics ---
void kami_panic(const char* msg);

#ifdef __cplusplus
} // extern "C"
#endif
