#pragma once
// Internal runtime structures. Not part of the public ABI.
#include "kami_runtime.h"

#include <atomic>
#include <mutex>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

namespace kami {

// ---------------- heap objects ----------------
struct ObjHeader {
    uint32_t type;   // KamiTag
    uint32_t mark;   // GC mark bit
    uint64_t size;   // allocation size in bytes (for GC accounting)
    ObjHeader* next; // all-objects list
};

struct KamiStr {
    ObjHeader h;
    int64_t len;
    int64_t cap;   // buffer capacity (bytes, excluding NUL); >= len
    uint64_t hash; // FNV-1a, computed lazily (0 = not yet computed)
    char data[1];  // cap bytes + NUL
};

struct KamiList {
    ObjHeader h;
    int64_t len;
    int64_t cap;
    KamiValue* items; // malloc'ed
};

struct MapEntry {
    uint64_t hash;
    bool used;       // false = deleted tombstone
    KamiValue key;
    KamiValue val;
};

// Compact, insertion-ordered dict/set (CPython-style): `entries` holds records
// in insertion order (with tombstones); `index` is a hash table of
// (entry_index + 1), 0 meaning empty. Iteration walks entries[0..nentries).
struct KamiMap {
    ObjHeader h;
    int64_t count;    // live entries
    int64_t nentries; // slots used in entries[] (incl. tombstones)
    int64_t ecap;     // capacity of entries[]
    MapEntry* entries;
    int64_t* index;   // malloc'ed hash table (power of two), or null
    int64_t icap;
};

// KamiFuncObj::flags
enum : int64_t { KFN_VARARG = 1, KFN_KWARG = 2 };

struct KamiFuncObj {
    ObjHeader h;
    void* fn;          // KamiFn (null when builtin_id >= 0)
    int64_t min_arity;
    int64_t arity;     // number of fixed parameters (incl. keyword-only)
    int64_t builtin_id; // >= 0 => call via kami_builtin
    const char* name;
    KamiValue* captures; // heap array, null when not a closure
    int64_t ncaptures;
    int64_t kwonly;          // index where keyword-only params begin (== arity if none)
    int64_t flags;           // KFN_VARARG / KFN_KWARG
    const char* param_names; // comma-joined fixed parameter names (may be "")
};

struct ThreadData {
    std::thread th;
    bool joined = false;
};

struct KamiThreadObj {
    ObjHeader h;
    ThreadData* td;
};

struct KamiClassObj {
    ObjHeader h;
    const char* name;                                // static string
    KamiClassObj* parent;                            // may be null
    std::unordered_map<std::string, KamiValue>* members; // methods + class attrs
};

struct KamiInstance {
    ObjHeader h;
    KamiClassObj* cls;
    std::unordered_map<std::string, KamiValue>* fields;
};

struct KamiFile {
    ObjHeader h;
    void* fp; // FILE*
    bool closed;
    bool no_close; // sys.stdout / sys.stderr: .close() must be a no-op
};

struct KamiPyObj {
    ObjHeader h;
    void* obj; // PyObject* (owned reference)
};

struct KamiSocket {
    ObjHeader h;
    int64_t fd;
    bool closed;
    double timeout; // seconds; <0 = blocking
    void* tls;      // TlsConn* once wrap_tls() ran (netsock.cpp), else null
};

// threading.Lock() / threading.RLock(): a real OS mutex, not a GIL trick. The
// compiled program runs as native machine code, so Python-level locks have to
// map onto the platform's own synchronisation primitives.
struct KamiLock {
    ObjHeader h;
    void* mtx;      // std::recursive_timed_mutex*
    bool reentrant;
    int64_t depth;  // for locked()
};

// Runtime error used for Python-level exceptions (try/except).
struct KamiError {
    std::string msg;
};

// ---------------- global runtime state ----------------
struct FrameStack {
    std::vector<std::pair<KamiValue*, int64_t>> frames;
};

// The single runtime lock (GIL). Every public API call holds it, except
// around blocking operations (sleep/join) and while invoking user functions.
//
// Fast path: until the first thread is spawned the program is single-threaded,
// so hot-path calls skip the mutex entirely. g_multithreaded flips to true
// inside threading.spawn() *while holding the real lock* and before the second
// thread exists, so the transition is race-free. It never flips back.
extern std::recursive_mutex g_lock;
extern std::atomic<bool> g_multithreaded;

struct Lock {
    bool on;
    explicit Lock(std::recursive_mutex&)
        : on(g_multithreaded.load(std::memory_order_relaxed)) {
        if (on) g_lock.lock();
    }
    ~Lock() {
        if (on) g_lock.unlock();
    }
    Lock(const Lock&) = delete;
    Lock& operator=(const Lock&) = delete;
};

extern std::vector<KamiValue> g_globals;
extern std::unordered_map<const void*, KamiStr*> g_intern; // interned literals
extern int64_t g_argc;
extern char** g_argv;
extern std::vector<std::pair<KamiValue*, int64_t>> g_pins;
extern std::vector<FrameStack*> g_frame_stacks;

FrameStack* tls_frames(); // registers on first use (lock must be held)

// GC (lock must be held by caller)
void* gc_alloc(uint64_t size, uint32_t type);
void gc_collect();
void gc_track_free(uint64_t bytes); // account external buffers (list items, map entries)
void gc_track_extra(uint64_t bytes);

// helpers
KamiStr* str_new(const char* data, int64_t len);       // lock held
KamiStr* str_new_cap(const char* data, int64_t len, int64_t cap); // lock held
uint64_t str_hash(KamiStr* s); // lazy FNV-1a (lock held)
KamiList* list_new(int64_t cap);                       // lock held
KamiMap* map_new();                                    // lock held
void list_push(KamiList* l, const KamiValue* v);       // lock held
void map_set(KamiMap* m, const KamiValue* k, const KamiValue* v); // lock held
bool map_get(KamiMap* m, const KamiValue* k, KamiValue* out);     // lock held
bool map_del(KamiMap* m, const KamiValue* k);                     // lock held
uint64_t value_hash(const KamiValue* v);
bool value_eq(const KamiValue* a, const KamiValue* b);
KamiValue* class_lookup(KamiClassObj* c, const std::string& name); // lock held

// Rearranges a dynamic call's arguments into the layout the callee's prologue
// expects (fixed params, then packed *args tuple / **kwargs dict). Lock must
// be held. `packed` must be a caller-pinned 2-slot array (zeroed). Returns the
// final argument count written to argv2 (capacity >= fo->arity + 2).
int64_t prep_user_argv(KamiFuncObj* fo, KamiValue** argv, int64_t nargs, KamiMap* kwmap,
                       KamiValue** argv2, KamiValue* packed);

// RAII temporary GC root (exception-safe, unlike raw g_pins pushes).
struct PinGuard {
    KamiValue* vals;
    PinGuard(KamiValue* v, int64_t n) : vals(v) {
        Lock lk(g_lock);
        g_pins.push_back({v, n});
    }
    ~PinGuard() {
        Lock lk(g_lock);
        for (size_t i = g_pins.size(); i-- > 0;)
            if (g_pins[i].first == vals) {
                g_pins.erase(g_pins.begin() + (long)i);
                break;
            }
    }
    PinGuard(const PinGuard&) = delete;
    PinGuard& operator=(const PinGuard&) = delete;
};

std::string format_float(double d);
std::string percent_format(const std::string& fmt, const KamiValue* args, int64_t nargs);

// Sockets and TLS (netsock.cpp): C-ABI bindings to the OS. All protocol logic
// (HTTP, ...) lives in runtime/pylib/ as Python.
bool socket_create(KamiValue* out);
// threading.Lock: the GIL-style runtime lock must be dropped while blocking on
// a user lock, otherwise the program would deadlock against itself.
bool lock_acquire(std::unique_lock<std::recursive_mutex>& lk, KamiLock* l, double timeout);
void lock_release(KamiLock* l);
void lock_destroy(KamiLock* l);
void socket_release(KamiSocket* s); // frees the TLS context, if any
void socket_method(std::unique_lock<std::recursive_mutex>& lk, KamiValue* out, KamiValue* obj,
                   const std::string& m, KamiValue** argv, int64_t nargs);
std::string format_value(KamiValue* v, const std::string& spec); // format() spec
std::string value_str(const KamiValue* v);   // human string (print)
std::string value_repr(const KamiValue* v);  // repr (inside containers)
const char* type_name(int64_t tag);
std::string& tls_error();                    // last caught error message (per thread)

// CPython bridge (pycapi.cpp). All take the runtime lock themselves or are
// called with it held as documented.
void pyobj_finalize(ObjHeader* h);                                  // GC sweep
void pyobj_attr_get(KamiValue* out, const KamiValue* obj, const char* name);
void pyobj_method(KamiValue* out, KamiValue* obj, const std::string& m,
                  KamiValue** argv, int64_t nargs);
void pyobj_call(KamiValue* out, const KamiValue* fn, KamiValue** argv, int64_t nargs);
std::string pyobj_str(const KamiValue* v);
int32_t pyobj_truthy(const KamiValue* v);

[[noreturn]] void panic(const std::string& msg); // throws KamiError

} // namespace kami
