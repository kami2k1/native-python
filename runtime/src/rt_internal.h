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
    uint64_t hash;
    char data[1]; // len bytes + NUL
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

struct KamiFuncObj {
    ObjHeader h;
    void* fn;          // KamiFn (null when builtin_id >= 0)
    int64_t min_arity;
    int64_t arity;     // max
    int64_t builtin_id; // >= 0 => call via kami_builtin
    const char* name;
    KamiValue* captures; // heap array, null when not a closure
    int64_t ncaptures;
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
};

struct KamiSocket {
    ObjHeader h;
    int64_t fd;
    bool closed;
    double timeout; // seconds; <0 = blocking
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
KamiList* list_new(int64_t cap);                       // lock held
KamiMap* map_new();                                    // lock held
void list_push(KamiList* l, const KamiValue* v);       // lock held
void map_set(KamiMap* m, const KamiValue* k, const KamiValue* v); // lock held
bool map_get(KamiMap* m, const KamiValue* k, KamiValue* out);     // lock held
bool map_del(KamiMap* m, const KamiValue* k);                     // lock held
uint64_t value_hash(const KamiValue* v);
bool value_eq(const KamiValue* a, const KamiValue* b);
KamiValue* class_lookup(KamiClassObj* c, const std::string& name); // lock held

std::string format_float(double d);
std::string percent_format(const std::string& fmt, const KamiValue* args, int64_t nargs);
void json_loads(KamiValue* out, const std::string& text);          // lock held
std::string json_dumps(const KamiValue* v);                        // lock held
KamiClassObj* internal_class(const char* name);                    // lock held
// Native HTTP/HTTPS client (http_client.cpp) — call WITHOUT the lock held.
// Returns false and sets `err` on transport failure (like requests raising).
bool http_request(const std::string& method, const std::string& url,
                  const std::string& body, const std::string& content_type,
                  double timeout_sec, long& status_out, std::string& body_out,
                  std::string& err);
void socket_method(std::unique_lock<std::recursive_mutex>& lk, KamiValue* out, KamiValue* obj,
                   const std::string& m, KamiValue** argv, int64_t nargs);
std::string format_value(KamiValue* v, const std::string& spec); // format() spec
std::string value_str(const KamiValue* v);   // human string (print)
std::string value_repr(const KamiValue* v);  // repr (inside containers)
const char* type_name(int64_t tag);
std::string& tls_error();                    // last caught error message (per thread)

[[noreturn]] void panic(const std::string& msg); // throws KamiError

} // namespace kami
