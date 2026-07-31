#pragma once
// Internal runtime structures. Not part of the public ABI.
#include "kami_runtime.h"

#include <atomic>
#include <mutex>
#include <string>
#include <thread>
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
    bool used;
    KamiValue key;
    KamiValue val;
};

struct KamiMap {
    ObjHeader h;
    int64_t count;
    int64_t cap; // power of two, 0 when empty
    MapEntry* entries; // malloc'ed
};

struct KamiFuncObj {
    ObjHeader h;
    void* fn; // KamiFn
    int64_t arity;
    const char* name; // static string from generated code
};

struct ThreadData {
    std::thread th;
    bool joined = false;
};

struct KamiThreadObj {
    ObjHeader h;
    ThreadData* td;
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
uint64_t value_hash(const KamiValue* v);
bool value_eq(const KamiValue* a, const KamiValue* b);

std::string value_str(const KamiValue* v);   // human string (print)
std::string value_repr(const KamiValue* v);  // repr (inside containers)
const char* type_name(int64_t tag);

[[noreturn]] void panic(const std::string& msg);

} // namespace kami
