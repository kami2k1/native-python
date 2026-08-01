// Generators (yield) for the KamiPython runtime.
//
// A generator is a *fiber*: the generator body runs on its own stack and
// suspends/resumes with a plain context switch on the SAME OS thread — no
// extra threads, no GIL handoff, no state-machine transformation in codegen.
// `yield x` compiles to a single runtime call (kami_gen_yield) that stores
// the value and switches back to the consumer; kami_gen_next switches in
// again and the body continues exactly where it stopped, its whole frame
// (an alloca on the fiber stack) intact.
//
// GC integration: every fiber owns its own FrameStack, registered in
// g_frame_stacks for the whole life of the generator, so values held by a
// *suspended* generator frame are ordinary GC roots. On a context switch the
// thread-local "current FrameStack" pointer is swapped (gc.cpp).
//
// Platform layer: Windows fibers (CreateFiber/SwitchToFiber) on _WIN32,
// POSIX ucontext (makecontext/swapcontext) everywhere else.
#include "rt_internal.h"

#include <cstdlib>
#include <cstring>

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#else
#ifdef __APPLE__
#define _XOPEN_SOURCE 600
#endif
#include <ucontext.h>
#endif

namespace kami {

static const size_t GEN_STACK_SIZE = 512 * 1024; // per-generator fiber stack

enum GenState : int {
    GEN_CREATED = 0,   // fiber not started yet
    GEN_SUSPENDED = 1, // parked at a yield
    GEN_RUNNING = 2,   // currently executing (re-entry is an error)
    GEN_DONE = 3,      // returned, raised, or closed
};

struct GenCtl {
    // --- what to run ---
    void* fn = nullptr;          // KamiFn of the compiled generator body
    KamiValue fnobj{KT_NONE, {0}}; // keeps a closure's captures alive (GC-marked)
    KamiValue* captures = nullptr;
    KamiValue args[18];          // argument values (GC-marked via the gen object)
    KamiValue* argv[18];         // pointers into args, passed to fn
    int64_t nargs = 0;

    // --- communication slots (GC-marked via the gen object) ---
    KamiValue yielded{KT_NONE, {0}};
    KamiValue sent{KT_NONE, {0}};
    KamiValue retval{KT_NONE, {0}};

    // --- state ---
    int state = GEN_CREATED;
    bool buffered = false;   // kami_iter_cond pulled a value not yet consumed
    bool closing = false;    // resume must raise GeneratorExit inside the body
    bool failed = false;     // body escaped with an exception
    std::string errmsg;
    const char* name = "generator";

    // --- fiber machinery ---
    FrameStack fs;           // the fiber's own GC root stack (registered)
    bool fs_registered = false;
#ifdef _WIN32
    LPVOID fiber = nullptr;
    LPVOID caller_fiber = nullptr;
#else
    ucontext_t ctx;          // the generator's context
    ucontext_t caller_ctx;   // where to switch back to on yield/finish
    void* stack = nullptr;
#endif
};

// The generator currently executing on this thread (nested generators nest
// naturally: each advance() saves and restores the previous value).
static thread_local GenCtl* t_current_gen = nullptr;
// makecontext can only pass ints portably; hand the ctl over via TLS.
static thread_local GenCtl* t_starting_gen = nullptr;

// ---------------------------------------------------------------------------
// Fiber entry point: runs the compiled body to completion, catching every
// Python-level error at the boundary so a C++ exception never crosses a
// context switch.
// ---------------------------------------------------------------------------

static void gen_body(GenCtl* g) {
    KamiValue ret{KT_NONE, {0}};
    try {
        ((KamiFn)g->fn)(&ret, g->argv, g->nargs, g->captures);
        Lock lk(g_lock);
        g->retval = ret;
    } catch (KamiError& e) {
        Lock lk(g_lock);
        if (!g->closing || e.msg != "GeneratorExit") {
            g->failed = true;
            g->errmsg = e.msg;
        }
    }
    {
        Lock lk(g_lock);
        g->state = GEN_DONE;
        g->fs.frames.clear();
    }
    // Do NOT return from the fiber function: switch back to the consumer.
#ifdef _WIN32
    SwitchToFiber(g->caller_fiber);
#else
    // swapcontext back; uc_link would also work but an explicit switch keeps
    // the control flow identical to the Windows path.
    swapcontext(&g->ctx, &g->caller_ctx);
#endif
    // never reached
}

#ifdef _WIN32
static void CALLBACK gen_fiber_entry(void*) {
    GenCtl* g = t_starting_gen;
    gen_body(g);
}
#else
static void gen_fiber_entry() {
    GenCtl* g = t_starting_gen;
    gen_body(g);
}
#endif

// ---------------------------------------------------------------------------
// Switching
// ---------------------------------------------------------------------------

// Switch into the generator until it yields or finishes. The caller must NOT
// hold g_lock (the body runs arbitrary user code).
static void gen_switch_in(GenCtl* g) {
    GenCtl* prev_gen = t_current_gen;
    FrameStack* prev_fs = kami::set_tls_frames(&g->fs);
    t_current_gen = g;
    g->state = GEN_RUNNING;
#ifdef _WIN32
    // The consumer side must itself be a fiber before it can switch away.
    static thread_local bool converted = false;
    if (!converted) {
        LPVOID self = ConvertThreadToFiber(nullptr);
        if (!self && GetLastError() == ERROR_ALREADY_FIBER) self = GetCurrentFiber();
        converted = true;
    }
    g->caller_fiber = GetCurrentFiber();
    if (!g->fiber) {
        t_starting_gen = g;
        g->fiber = CreateFiberEx(64 * 1024, GEN_STACK_SIZE, 0, gen_fiber_entry, nullptr);
        if (!g->fiber) panic("cannot create generator fiber");
    }
    SwitchToFiber(g->fiber);
#else
    if (!g->stack) {
        g->stack = malloc(GEN_STACK_SIZE);
        if (!g->stack) panic("out of memory (generator stack)");
        getcontext(&g->ctx);
        g->ctx.uc_stack.ss_sp = g->stack;
        g->ctx.uc_stack.ss_size = GEN_STACK_SIZE;
        g->ctx.uc_link = nullptr;
        t_starting_gen = g;
        makecontext(&g->ctx, (void (*)())gen_fiber_entry, 0);
    }
    swapcontext(&g->caller_ctx, &g->ctx);
#endif
    // back on the consumer side
    t_current_gen = prev_gen;
    kami::set_tls_frames(prev_fs);
    if (g->state == GEN_RUNNING) g->state = GEN_SUSPENDED;
}

// Advance the generator with `sent` as the value of the paused yield
// expression. Returns true and fills *out when a value was yielded; returns
// false when the generator finished normally. Raises when the body raised.
bool gen_advance(KamiValue* out, KamiGenObj* gen, const KamiValue* sent) {
    GenCtl* g = gen->ctl;
    {
        Lock lk(g_lock);
        if (g->state == GEN_RUNNING) panic("generator already executing");
        if (g->state == GEN_DONE) return false;
        if (g->state == GEN_CREATED && sent && sent->tag != KT_NONE)
            panic("TypeError: can't send non-None value to a just-started generator");
        g->sent = sent ? *sent : KamiValue{KT_NONE, {0}};
        if (!g->fs_registered) {
            g_frame_stacks.push_back(&g->fs);
            g->fs_registered = true;
        }
    }
    gen_switch_in(g);
    Lock lk(g_lock);
    if (g->state == GEN_DONE) {
        if (g->failed) {
            g->failed = false; // an exception propagates once (CPython parity)
            std::string m = g->errmsg;
            panic(m);
        }
        return false;
    }
    if (out) *out = g->yielded;
    return true;
}

// ---------------------------------------------------------------------------
// Object lifecycle
// ---------------------------------------------------------------------------

static KamiGenObj* gen_alloc(const char* name) {
    Lock lk(g_lock);
    KamiGenObj* gen = (KamiGenObj*)gc_alloc(sizeof(KamiGenObj), KT_GEN);
    gen->ctl = new GenCtl();
    if (name) gen->ctl->name = name;
    return gen;
}

void gen_mark_children(ObjHeader* h, std::vector<ObjHeader*>& stack,
                       void (*mark)(const KamiValue*, std::vector<ObjHeader*>&)) {
    GenCtl* g = ((KamiGenObj*)h)->ctl;
    if (!g) return;
    mark(&g->fnobj, stack);
    for (int64_t i = 0; i < g->nargs; i++) mark(&g->args[i], stack);
    mark(&g->yielded, stack);
    mark(&g->sent, stack);
    mark(&g->retval, stack);
}

// GC sweep of an unreachable generator. Nobody can ever resume it again, so
// the fiber (with any live frames on its stack) is simply torn down. Its
// FrameStack is unregistered first — the next collection reclaims whatever
// only the dead frames kept alive. (finally-blocks of abandoned generators do
// not run; CPython gives no guarantee there either.)
void gen_finalize(ObjHeader* h) {
    GenCtl* g = ((KamiGenObj*)h)->ctl;
    if (!g) return;
    if (g->fs_registered) {
        for (size_t i = 0; i < g_frame_stacks.size(); i++) {
            if (g_frame_stacks[i] == &g->fs) {
                g_frame_stacks.erase(g_frame_stacks.begin() + (long)i);
                break;
            }
        }
    }
#ifdef _WIN32
    if (g->fiber) DeleteFiber(g->fiber);
#else
    free(g->stack);
#endif
    delete g;
    ((KamiGenObj*)h)->ctl = nullptr;
}

// Shared constructor: copies the arguments into the control block (they must
// survive until the fiber starts and roots them itself via the frame push in
// the compiled prologue — the gen object marks them, see gen_mark_children).
static void gen_create_common(KamiValue* out, void* fnptr, KamiValue* fnobj,
                              KamiValue* captures, const char* name,
                              KamiValue** argv, int64_t nargs) {
    if (nargs > 18) panic("too many arguments to a generator function");
    KamiGenObj* gen = gen_alloc(name);
    out->tag = KT_GEN;
    out->p = gen; // out is a rooted slot: the object is safe from here on
    GenCtl* g = gen->ctl;
    g->fn = fnptr;
    g->captures = captures;
    if (fnobj) g->fnobj = *fnobj;
    g->nargs = nargs;
    Lock lk(g_lock);
    for (int64_t i = 0; i < nargs; i++) {
        g->args[i] = *argv[i];
        g->argv[i] = &g->args[i];
    }
}

// Called by kami_call_value / kami_method when a function object carries
// KFN_GENERATOR (lock held by caller; argv already normalized by
// prep_user_argv, so defaults/kwargs/varargs behave exactly like a plain
// call).
void gen_create_from_func(KamiValue* out, KamiFuncObj* fo, const KamiValue* fnval,
                          KamiValue** argv, int64_t nargs) {
    KamiValue fv = *fnval;
    gen_create_common(out, fo->fn, &fv, fo->captures, fo->name, argv, nargs);
}

// Lazy element pull used by the for-loop protocol (kami_iter_cond/get).
bool gen_pull(KamiGenObj* gen) {
    GenCtl* g = gen->ctl;
    {
        Lock lk(g_lock);
        if (g->buffered) return true;
        if (g->state == GEN_DONE) return false;
    }
    KamiValue v{KT_NONE, {0}};
    if (!gen_advance(&v, gen, nullptr)) return false;
    Lock lk(g_lock);
    g->buffered = true; // value parked in g->yielded
    return true;
}

void gen_take_buffered(KamiValue* out, KamiGenObj* gen) {
    Lock lk(g_lock);
    GenCtl* g = gen->ctl;
    if (!g->buffered) panic("internal error: generator buffer empty");
    g->buffered = false;
    *out = g->yielded;
}

// generator methods: __next__ / send / close (dispatched from kami_method)
bool gen_method(KamiValue* out, KamiValue* obj, const std::string& m, KamiValue** argv,
                int64_t nargs) {
    KamiGenObj* gen = (KamiGenObj*)obj->p;
    if (m == "__next__" || m == "next") {
        if (nargs != 0) panic("__next__() takes no arguments");
        kami_gen_next(out, obj);
        return true;
    }
    if (m == "send") {
        if (nargs != 1) panic("send() takes exactly one argument");
        GenCtl* g = gen->ctl;
        {
            Lock lk(g_lock);
            if (g->buffered) panic("send() on a generator being iterated");
        }
        if (!gen_advance(out, gen, argv[0])) panic("StopIteration");
        return true;
    }
    if (m == "close") {
        if (nargs != 0) panic("close() takes no arguments");
        GenCtl* g = gen->ctl;
        {
            Lock lk(g_lock);
            if (g->state == GEN_DONE || g->state == GEN_CREATED) {
                g->state = GEN_DONE;
                out->tag = KT_NONE;
                out->i = 0;
                return true;
            }
            g->closing = true;
            g->buffered = false;
        }
        // Resume so the paused yield raises GeneratorExit; finally-blocks run.
        if (gen_advance(nullptr, gen, nullptr))
            panic("RuntimeError: generator ignored GeneratorExit");
        out->tag = KT_NONE;
        out->i = 0;
        return true;
    }
    if (m == "throw") {
        if (nargs < 1 || nargs > 2) panic("throw() takes 1 or 2 arguments");
        // Simplified: close the generator and re-raise the given error here.
        GenCtl* g = gen->ctl;
        std::string msg;
        {
            Lock lk(g_lock);
            msg = value_str(argv[nargs - 1]);
        }
        {
            Lock lk(g_lock);
            if (g->state == GEN_SUSPENDED) {
                g->closing = true;
                g->buffered = false;
            } else {
                g->state = GEN_DONE;
            }
        }
        if (g->state != GEN_DONE) gen_advance(nullptr, gen, nullptr);
        panic(msg);
    }
    return false;
}

// Drain a generator into a fresh list (list(g), sorted(g), sum(g), ...).
void gen_drain_to_list(KamiValue* out, KamiValue* gen_val) {
    // Root the result and the element while the generator runs user code.
    KamiValue scratch[3];
    scratch[0] = KamiValue{KT_NONE, {0}}; // list
    scratch[1] = KamiValue{KT_NONE, {0}}; // element
    scratch[2] = *gen_val;                // the generator itself
    PinGuard pin(scratch, 3);
    {
        Lock lk(g_lock);
        KamiList* l = list_new(4);
        scratch[0].tag = KT_LIST;
        scratch[0].p = l;
    }
    KamiGenObj* gen = (KamiGenObj*)scratch[2].p;
    for (;;) {
        // respect a value parked by a half-finished for-loop pull
        {
            Lock lk(g_lock);
            GenCtl* g = gen->ctl;
            if (g->buffered) {
                g->buffered = false;
                scratch[1] = g->yielded;
                list_push((KamiList*)scratch[0].p, &scratch[1]);
                continue;
            }
        }
        if (!gen_advance(&scratch[1], gen, nullptr)) break;
        Lock lk(g_lock);
        list_push((KamiList*)scratch[0].p, &scratch[1]);
    }
    *out = scratch[0];
}

} // namespace kami

using namespace kami;

extern "C" {

// Direct call site of a generator function: build the generator object
// instead of running the body. argv layout matches a normal direct call —
// the compiled prologue fills defaults from nargs when the fiber starts.
void kami_gen_create(KamiValue* out, void* fnptr, KamiValue** argv, int64_t nargs) {
    gen_create_common(out, fnptr, nullptr, nullptr, "generator", argv, nargs);
}

// `yield v` inside a compiled generator body: park the value, switch to the
// consumer, and (much later) resume returning the sent value.
void kami_gen_yield(KamiValue* out, const KamiValue* v) {
    GenCtl* g = t_current_gen;
    if (!g) panic("yield outside of a running generator");
    {
        Lock lk(g_lock);
        g->yielded = *v;
        g->state = GEN_SUSPENDED;
    }
#ifdef _WIN32
    SwitchToFiber(g->caller_fiber);
#else
    swapcontext(&g->ctx, &g->caller_ctx);
#endif
    // resumed
    if (g->closing) throw KamiError{"GeneratorExit"};
    Lock lk(g_lock);
    *out = g->sent;
    g->sent = KamiValue{KT_NONE, {0}};
}

// next(g): yields the next value or raises StopIteration.
void kami_gen_next(KamiValue* out, KamiValue* gen_val) {
    if (gen_val->tag != KT_GEN) panic("next() argument must be a generator");
    KamiGenObj* gen = (KamiGenObj*)gen_val->p;
    {
        Lock lk(g_lock);
        GenCtl* g = gen->ctl;
        if (g->buffered) { // interleaved with a for-loop pull
            g->buffered = false;
            *out = g->yielded;
            return;
        }
    }
    if (!gen_advance(out, gen, nullptr)) panic("StopIteration");
}

} // extern "C"
