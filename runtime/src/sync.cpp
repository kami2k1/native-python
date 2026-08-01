// threading.Condition / Event / Semaphore and threading.Thread objects —
// native synchronisation primitives for stdlib sources compiled from the
// system Python installation (queue.py, concurrent/futures/*.py, ...).
//
// The compiled program is native machine code, so Python-level primitives
// map straight onto the platform's own: std::condition_variable_any over the
// lock's recursive_timed_mutex. The runtime lock (GIL-style) is always
// released before blocking, exactly like threading.Lock (objects.cpp).
#include "rt_internal.h"

#include <chrono>
#include <condition_variable>
#include <cstring>

namespace kami {

std::vector<KamiValue> g_atexit;

struct SyncImpl {
    // Condition: cv waits on the associated KamiLock's mutex.
    // Event / Semaphore: self-contained mutex + cv + counter.
    std::condition_variable_any cv;
    std::mutex m;
    int64_t value = 0; // Event: flag; Semaphore: count
};

static double arg_timeout(KamiValue** argv, int64_t nargs, int64_t idx, KamiMap* kwmap) {
    // returns seconds; < 0 = no timeout (block forever)
    KamiValue tv{KT_NONE, {0}};
    bool have = false;
    if (nargs > idx) {
        tv = *argv[idx];
        have = true;
    } else if (kwmap) {
        KamiValue key{KT_STR, {0}};
        key.p = str_new("timeout", 7);
        have = map_get(kwmap, &key, &tv);
    }
    if (!have || tv.tag == KT_NONE) return -1.0;
    if (tv.tag == KT_FLOAT) return tv.f;
    if (tv.tag == KT_INT || tv.tag == KT_BOOL) return (double)tv.i;
    panic("timeout must be a number or None");
}

void sync_make(KamiValue* out, int64_t kind, KamiValue** argv, int64_t nargs) {
    Lock lk(g_lock);
    KamiSync* s = (KamiSync*)gc_alloc(sizeof(KamiSync), KT_SYNC);
    s->kind = kind;
    s->impl = new SyncImpl();
    s->assoc = KamiValue{KT_NONE, {0}};
    out->tag = KT_SYNC;
    out->p = s; // out is a rooted slot
    if (kind == 0) { // Condition([lock])
        if (nargs == 1 && argv[0]->tag != KT_NONE) {
            if (argv[0]->tag != KT_LOCK)
                panic("threading.Condition(): argument must be a Lock/RLock");
            s->assoc = *argv[0];
        } else {
            // no lock given: make our own RLock
            KamiLock* l = (KamiLock*)gc_alloc(sizeof(KamiLock), KT_LOCK);
            l->mtx = new std::recursive_timed_mutex();
            l->reentrant = true;
            l->depth = 0;
            s->assoc.tag = KT_LOCK;
            s->assoc.p = l;
        }
    } else if (kind == 2) { // Semaphore(value=1)
        s->impl->value = 1;
        if (nargs == 1) {
            if (argv[0]->tag != KT_INT) panic("threading.Semaphore(): value must be an int");
            s->impl->value = argv[0]->i;
        }
    }
}

void sync_destroy(KamiSync* s) {
    delete s->impl;
    s->impl = nullptr;
}

static void set_none(KamiValue* out) {
    out->tag = KT_NONE;
    out->i = 0;
}

bool sync_method(std::unique_lock<std::recursive_mutex>& lk, KamiValue* out, KamiValue* obj,
                 const std::string& m, KamiValue** argv, int64_t nargs, KamiMap* kwmap) {
    KamiSync* s = (KamiSync*)obj->p;
    SyncImpl* im = s->impl;
    if (s->kind == 0) { // ---- Condition ----
        KamiLock* l = (KamiLock*)s->assoc.p;
        auto* mtx = (std::recursive_timed_mutex*)l->mtx;
        if (m == "acquire" || m == "__enter__") {
            bool got = lock_acquire(lk, l, m == "__enter__" ? -1.0
                                                            : arg_timeout(argv, nargs, 1, kwmap));
            if (m == "__enter__") *out = *obj;
            else {
                out->tag = KT_BOOL;
                out->i = got;
            }
            return true;
        }
        if (m == "release" || m == "__exit__") {
            lock_release(l);
            set_none(out);
            return true;
        }
        if (m == "wait") {
            if (l->depth <= 0) panic("RuntimeError: cannot wait on un-acquired lock");
            double tmo = arg_timeout(argv, nargs, 0, kwmap);
            int64_t depth = l->depth;
            l->depth = 0;
            bool notified = true;
            lk.unlock(); // release the runtime lock while blocked
            if (tmo < 0) {
                im->cv.wait(*mtx);
            } else {
                notified = im->cv.wait_for(*mtx, std::chrono::milliseconds(
                                                     (long long)(tmo * 1000))) ==
                           std::cv_status::no_timeout;
            }
            lk.lock();
            l->depth = depth;
            out->tag = KT_BOOL;
            out->i = notified;
            return true;
        }
        if (m == "wait_for") { // predicate form: loop in native code
            if (nargs < 1) panic("Condition.wait_for() needs a predicate");
            if (l->depth <= 0) panic("RuntimeError: cannot wait on un-acquired lock");
            double tmo = arg_timeout(argv, nargs, 1, kwmap);
            auto deadline = std::chrono::steady_clock::now() +
                            std::chrono::milliseconds((long long)(tmo * 1000));
            for (;;) {
                KamiValue pred = *argv[0];
                KamiValue r{KT_NONE, {0}};
                lk.unlock();
                kami_call_value(&r, &pred, nullptr, 0);
                lk.lock();
                if (kami_truthy(&r)) {
                    out->tag = KT_BOOL;
                    out->i = 1;
                    return true;
                }
                int64_t depth = l->depth;
                l->depth = 0;
                lk.unlock();
                bool timed_out = false;
                if (tmo < 0) {
                    im->cv.wait(*mtx);
                } else {
                    timed_out = im->cv.wait_until(*mtx, deadline) == std::cv_status::timeout;
                }
                lk.lock();
                l->depth = depth;
                if (timed_out) {
                    KamiValue r2{KT_NONE, {0}};
                    lk.unlock();
                    kami_call_value(&r2, argv[0], nullptr, 0);
                    lk.lock();
                    out->tag = KT_BOOL;
                    out->i = kami_truthy(&r2);
                    return true;
                }
            }
        }
        if (m == "notify") {
            if (nargs == 0) im->cv.notify_one();
            else
                for (int64_t i = 0; i < (argv[0]->tag == KT_INT ? argv[0]->i : 1); i++)
                    im->cv.notify_one();
            set_none(out);
            return true;
        }
        if (m == "notify_all" || m == "notifyAll") {
            im->cv.notify_all();
            set_none(out);
            return true;
        }
        return false;
    }
    if (s->kind == 1) { // ---- Event ----
        if (m == "set") {
            {
                std::lock_guard<std::mutex> g(im->m);
                im->value = 1;
            }
            im->cv.notify_all();
            set_none(out);
            return true;
        }
        if (m == "clear") {
            std::lock_guard<std::mutex> g(im->m);
            im->value = 0;
            set_none(out);
            return true;
        }
        if (m == "is_set" || m == "isSet") {
            out->tag = KT_BOOL;
            out->i = im->value != 0;
            return true;
        }
        if (m == "wait") {
            double tmo = arg_timeout(argv, nargs, 0, kwmap);
            lk.unlock();
            bool set;
            {
                std::unique_lock<std::mutex> g(im->m);
                if (tmo < 0) {
                    im->cv.wait(g, [&] { return im->value != 0; });
                    set = true;
                } else {
                    set = im->cv.wait_for(g, std::chrono::milliseconds((long long)(tmo * 1000)),
                                          [&] { return im->value != 0; });
                }
            }
            lk.lock();
            out->tag = KT_BOOL;
            out->i = set;
            return true;
        }
        return false;
    }
    // ---- Semaphore ----
    if (m == "acquire" || m == "__enter__") {
        bool blocking = true;
        if (nargs >= 1 && m == "acquire") blocking = kami_truthy(argv[0]);
        double tmo = m == "acquire" ? arg_timeout(argv, nargs, 1, kwmap) : -1.0;
        if (!blocking) tmo = 0.0;
        lk.unlock();
        bool got;
        {
            std::unique_lock<std::mutex> g(im->m);
            auto ready = [&] { return im->value > 0; };
            if (tmo < 0) {
                im->cv.wait(g, ready);
                got = true;
            } else {
                got = im->cv.wait_for(g, std::chrono::milliseconds((long long)(tmo * 1000)),
                                      ready);
            }
            if (got) im->value--;
        }
        lk.lock();
        if (m == "__enter__") *out = *obj;
        else {
            out->tag = KT_BOOL;
            out->i = got;
        }
        return true;
    }
    if (m == "release" || m == "__exit__") {
        int64_t n = 1;
        if (m == "release" && nargs >= 1 && argv[0]->tag == KT_INT) n = argv[0]->i;
        {
            std::lock_guard<std::mutex> g(im->m);
            im->value += n;
        }
        im->cv.notify_all();
        set_none(out);
        return true;
    }
    return false;
}

// ---------------------------------------------------------------------------
// threading.Thread objects
// ---------------------------------------------------------------------------

// (thread_body for spawned targets lives in builtins.cpp; Thread objects
// store the target and start it on .start() with the same machinery.)
void thread_make(KamiValue* out, KamiValue** argv, int64_t nargs) {
    // argv: [target, args, name, daemon] — normalized by sema
    Lock lk(g_lock);
    KamiThreadObj* to = (KamiThreadObj*)gc_alloc(sizeof(KamiThreadObj), KT_THREAD);
    to->td = new ThreadData();
    out->tag = KT_THREAD;
    out->p = to;
    if (nargs > 0) to->td->target = *argv[0];
    if (nargs > 1 && argv[1]->tag == KT_LIST) {
        KamiList* l = (KamiList*)argv[1]->p;
        for (int64_t i = 0; i < l->len; i++) to->td->args.push_back(l->items[i]);
    }
    // argv[2] = name (kept only for repr; ignored), argv[3] = daemon
    if (nargs > 3 && argv[3]->tag != KT_NONE) to->td->daemon = kami_truthy(argv[3]);
}

void atexit_register(KamiValue** argv, int64_t nargs) {
    Lock lk(g_lock);
    // store callback followed by its arguments, length-prefixed
    KamiValue n{KT_INT, {0}};
    n.i = nargs;
    g_atexit.push_back(n);
    for (int64_t i = 0; i < nargs; i++) g_atexit.push_back(*argv[i]);
}

void atexit_run() {
    // Runs at shutdown BEFORE joining threads: callbacks wake worker threads
    // (concurrent.futures pushes None into the work queues here).
    std::vector<KamiValue> cbs;
    {
        Lock lk(g_lock);
        cbs.swap(g_atexit);
    }
    if (cbs.empty()) return;
    PinGuard pin(cbs.data(), (int64_t)cbs.size()); // callbacks stay GC roots
    // most-recently registered first (CPython's _register_atexit order)
    for (size_t i = cbs.size(); i > 0;) {
        // find the record start by walking from the front (records are small)
        size_t start = 0;
        int64_t n = 0;
        for (size_t j = 0; j < i;) {
            start = j;
            n = cbs[j].i;
            j += 1 + (size_t)n;
        }
        i = start;
        KamiValue* argv[16];
        int64_t nargs = n - 1;
        for (int64_t k = 0; k < nargs && k < 16; k++) argv[k] = &cbs[start + 2 + (size_t)k];
        KamiValue ret{KT_NONE, {0}};
        try {
            kami_call_value(&ret, &cbs[start + 1], argv, nargs);
        } catch (KamiError& e) {
            fprintf(stderr, "KamiPython: error in atexit callback: %s\n", e.msg.c_str());
        }
    }
}

} // namespace kami
