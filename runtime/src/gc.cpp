// GC + root management for the KamiPython runtime.
// Non-moving mark & sweep, triggered on allocation, guarded by the global lock.
#include "rt_internal.h"

#include <cstdlib>
#include <cstring>
#include <cstdio>

namespace kami {

std::recursive_mutex g_lock;
std::atomic<bool> g_multithreaded{false};
std::vector<KamiValue> g_globals;
std::vector<std::pair<KamiValue*, int64_t>> g_pins;
std::vector<FrameStack*> g_frame_stacks;

static ObjHeader* g_all_objects = nullptr;
static uint64_t g_bytes_live = 0;
static uint64_t g_bytes_since_gc = 0;
static uint64_t g_gc_threshold = 1 << 20; // 1 MiB
static uint64_t g_gc_runs = 0;

// ---- thread-local frame registration ----
namespace {
struct TlsReg {
    FrameStack fs;
    bool registered = false;
    ~TlsReg() {
        if (registered) {
            Lock lk(g_lock);
            for (size_t i = 0; i < g_frame_stacks.size(); i++) {
                if (g_frame_stacks[i] == &fs) {
                    g_frame_stacks.erase(g_frame_stacks.begin() + i);
                    break;
                }
            }
        }
    }
};
thread_local TlsReg t_reg;
} // namespace

FrameStack* tls_frames() {
    if (!t_reg.registered) {
        g_frame_stacks.push_back(&t_reg.fs);
        t_reg.registered = true;
    }
    return &t_reg.fs;
}

// ---- marking ----
static void mark_value(const KamiValue* v, std::vector<ObjHeader*>& stack) {
    if (v->tag < KT_STR || v->p == nullptr) return;
    ObjHeader* h = (ObjHeader*)v->p;
    if (h->mark) return;
    h->mark = 1;
    stack.push_back(h);
}

static void mark_children(ObjHeader* h, std::vector<ObjHeader*>& stack) {
    switch (h->type) {
    case KT_LIST: {
        KamiList* l = (KamiList*)h;
        for (int64_t i = 0; i < l->len; i++) mark_value(&l->items[i], stack);
        break;
    }
    case KT_MAP: {
        KamiMap* m = (KamiMap*)h;
        for (int64_t i = 0; i < m->cap; i++) {
            if (m->entries[i].used) {
                mark_value(&m->entries[i].key, stack);
                mark_value(&m->entries[i].val, stack);
            }
        }
        break;
    }
    default: break; // STR / FUNC / THREAD have no Value children
    }
}

void gc_collect() {
    g_gc_runs++;
    // mark
    std::vector<ObjHeader*> stack;
    for (auto& v : g_globals) mark_value(&v, stack);
    for (auto& pin : g_pins)
        for (int64_t i = 0; i < pin.second; i++) mark_value(&pin.first[i], stack);
    for (FrameStack* fs : g_frame_stacks)
        for (auto& fr : fs->frames)
            for (int64_t i = 0; i < fr.second; i++) mark_value(&fr.first[i], stack);
    while (!stack.empty()) {
        ObjHeader* h = stack.back();
        stack.pop_back();
        mark_children(h, stack);
    }
    // sweep
    uint64_t live = 0;
    ObjHeader** link = &g_all_objects;
    while (*link) {
        ObjHeader* h = *link;
        if (h->mark) {
            h->mark = 0;
            live += h->size;
            link = &h->next;
        } else {
            *link = h->next;
            switch (h->type) {
            case KT_LIST: free(((KamiList*)h)->items); break;
            case KT_MAP: free(((KamiMap*)h)->entries); break;
            case KT_THREAD: {
                ThreadData* td = ((KamiThreadObj*)h)->td;
                if (td) {
                    if (!td->joined && td->th.joinable()) td->th.detach();
                    delete td;
                }
                break;
            }
            default: break;
            }
            free(h);
        }
    }
    g_bytes_live = live;
    g_bytes_since_gc = 0;
    g_gc_threshold = g_bytes_live * 2;
    if (g_gc_threshold < (1u << 20)) g_gc_threshold = 1 << 20;
}

void* gc_alloc(uint64_t size, uint32_t type) {
    if (g_bytes_since_gc > g_gc_threshold) gc_collect();
    ObjHeader* h = (ObjHeader*)calloc(1, size);
    if (!h) panic("out of memory");
    h->type = type;
    h->mark = 0;
    h->size = size;
    h->next = g_all_objects;
    g_all_objects = h;
    g_bytes_since_gc += size;
    return h;
}

void gc_track_extra(uint64_t bytes) { g_bytes_since_gc += bytes; }
void gc_track_free(uint64_t) {}

[[noreturn]] void panic(const std::string& msg) {
    fflush(stdout);
    fprintf(stderr, "KamiPython runtime error: %s\n", msg.c_str());
    fflush(stderr);
    _Exit(1);
}

} // namespace kami

using namespace kami;

extern "C" {

void kami_rt_init(void) {}

void kami_rt_shutdown(void) {
    // Join any still-running threads so we exit cleanly.
    std::vector<ThreadData*> pending;
    {
        Lock lk(g_lock);
        for (ObjHeader* h = g_all_objects; h; h = h->next) {
            if (h->type == KT_THREAD) {
                ThreadData* td = ((KamiThreadObj*)h)->td;
                if (td && !td->joined && td->th.joinable()) {
                    td->joined = true;
                    pending.push_back(td);
                }
            }
        }
    }
    for (ThreadData* td : pending) td->th.join();
    fflush(stdout);
}

void kami_globals_init(int64_t n) {
    Lock lk(g_lock);
    g_globals.assign((size_t)n, KamiValue{KT_NONE, {0}});
}

void kami_global_get(KamiValue* out, int64_t idx) {
    Lock lk(g_lock);
    *out = g_globals[(size_t)idx];
}

void kami_global_set(int64_t idx, const KamiValue* v) {
    Lock lk(g_lock);
    g_globals[(size_t)idx] = *v;
}

void kami_global_make_func(int64_t idx, void* fnptr, int64_t arity, const char* name) {
    Lock lk(g_lock);
    KamiFuncObj* f = (KamiFuncObj*)gc_alloc(sizeof(KamiFuncObj), KT_FUNC);
    f->fn = fnptr;
    f->arity = arity;
    f->name = name;
    g_globals[(size_t)idx].tag = KT_FUNC;
    g_globals[(size_t)idx].p = f;
}

void kami_frame_push(KamiValue* slots, int64_t n) {
    memset(slots, 0, sizeof(KamiValue) * (size_t)n); // before registering: GC never sees garbage
    Lock lk(g_lock);
    tls_frames()->frames.push_back({slots, n});
}

void kami_frame_pop(void) {
    Lock lk(g_lock);
    tls_frames()->frames.pop_back();
}

void kami_panic(const char* msg) { panic(msg); }

} // extern "C"
