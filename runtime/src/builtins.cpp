// Builtin functions: print/len/str/... plus math, time, random, threading modules.
#include "../include/kami_builtins.h"
#include "rt_internal.h"

#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <random>

namespace kami {

static double arg_num(KamiValue* v, const char* fn) {
    if (v->tag == KT_INT || v->tag == KT_BOOL) return (double)v->i;
    if (v->tag == KT_FLOAT) return v->f;
    panic(std::string(fn) + "() expected a number, got '" + type_name(v->tag) + "'");
}

static int64_t arg_int(KamiValue* v, const char* fn) {
    if (v->tag == KT_INT || v->tag == KT_BOOL) return v->i;
    panic(std::string(fn) + "() expected an int, got '" + type_name(v->tag) + "'");
}

static void check_arity(int64_t nargs, int64_t lo, int64_t hi, const char* fn) {
    if (nargs < lo || nargs > hi)
        panic(std::string(fn) + "() got " + std::to_string(nargs) + " argument(s)");
}

static std::mt19937_64 g_rng{0xC0FFEEull};

static void set_float(KamiValue* out, double d) { out->tag = KT_FLOAT; out->f = d; }
static void set_int(KamiValue* out, int64_t i) { out->tag = KT_INT; out->i = i; }
static void set_none(KamiValue* out) { out->tag = KT_NONE; out->i = 0; }

// ---- threading ----
struct PinBlock {
    KamiValue* vals;
    int64_t n;
};

static void thread_body(PinBlock pb) {
    // frame: [0]=result, [1]=fn, [2..]=args   (registered as GC roots)
    std::vector<KamiValue> slots((size_t)pb.n + 1);
    kami_frame_push(slots.data(), pb.n + 1);
    {
        Lock lk(g_lock);
        for (int64_t i = 0; i < pb.n; i++) slots[(size_t)(1 + i)] = pb.vals[i];
        for (size_t i = 0; i < g_pins.size(); i++) {
            if (g_pins[i].first == pb.vals) {
                g_pins.erase(g_pins.begin() + (long)i);
                break;
            }
        }
        free(pb.vals);
    }
    int64_t nargs = pb.n - 1;
    KamiValue* argv[16];
    for (int64_t i = 0; i < nargs; i++) argv[i] = &slots[(size_t)(2 + i)];
    kami_call_value(&slots[0], &slots[1], argv, nargs);
    kami_frame_pop();
}

static void builtin_spawn(KamiValue* out, KamiValue** argv, int64_t nargs) {
    // The real lock is held by the caller (kami_builtin). Enable locking on all
    // hot paths BEFORE the new thread exists.
    g_multithreaded.store(true, std::memory_order_seq_cst);
    check_arity(nargs, 1, 9, "threading.spawn");
    if (argv[0]->tag != KT_FUNC) panic("threading.spawn(): first argument must be a function");
    // Pin fn + args so they stay alive until the new thread roots them.
    PinBlock pb;
    pb.n = nargs;
    pb.vals = (KamiValue*)malloc(sizeof(KamiValue) * (size_t)nargs);
    if (!pb.vals) panic("out of memory");
    for (int64_t i = 0; i < nargs; i++) pb.vals[i] = *argv[i];
    g_pins.push_back({pb.vals, pb.n});

    KamiThreadObj* to = (KamiThreadObj*)gc_alloc(sizeof(KamiThreadObj), KT_THREAD);
    to->td = new ThreadData();
    out->tag = KT_THREAD;
    out->p = to; // out is a rooted frame slot: object safe before we release the lock
    to->td->th = std::thread(thread_body, pb);
}

static void builtin_join(std::unique_lock<std::recursive_mutex>& lk, KamiValue* out,
                         KamiValue** argv, int64_t nargs) {
    check_arity(nargs, 1, 1, "threading.join");
    if (argv[0]->tag != KT_THREAD) panic("threading.join(): argument must be a thread");
    KamiThreadObj* to = (KamiThreadObj*)argv[0]->p;
    set_none(out);
    if (to->td->joined) return;
    to->td->joined = true;
    ThreadData* td = to->td;
    lk.unlock(); // release the GIL while blocked, so the target thread can run
    td->th.join();
    lk.lock();
}

// ---- conversions ----
static void builtin_str(KamiValue* out, KamiValue* v) {
    std::string s = value_str(v);
    out->tag = KT_STR;
    out->p = str_new(s.data(), (int64_t)s.size());
}

static void builtin_int(KamiValue* out, KamiValue* v) {
    switch (v->tag) {
    case KT_BOOL:
    case KT_INT: set_int(out, v->i); return;
    case KT_FLOAT: set_int(out, (int64_t)v->f); return; // trunc toward zero
    case KT_STR: {
        KamiStr* s = (KamiStr*)v->p;
        char* end = nullptr;
        long long r = strtoll(s->data, &end, 10);
        while (end && *end == ' ') end++;
        if (end == s->data || (end && *end != '\0'))
            panic(std::string("invalid literal for int(): '") + s->data + "'");
        set_int(out, (int64_t)r);
        return;
    }
    default: panic(std::string("int() argument must be a number or str"));
    }
}

static void builtin_float(KamiValue* out, KamiValue* v) {
    switch (v->tag) {
    case KT_BOOL:
    case KT_INT: set_float(out, (double)v->i); return;
    case KT_FLOAT: set_float(out, v->f); return;
    case KT_STR: {
        KamiStr* s = (KamiStr*)v->p;
        char* end = nullptr;
        double d = strtod(s->data, &end);
        while (end && *end == ' ') end++;
        if (end == s->data || (end && *end != '\0'))
            panic(std::string("could not convert string to float: '") + s->data + "'");
        set_float(out, d);
        return;
    }
    default: panic("float() argument must be a number or str");
    }
}

static void dispatch(std::unique_lock<std::recursive_mutex>& lk, int64_t id, KamiValue* out,
                     KamiValue** argv, int64_t nargs) {
    switch (id) {
    case KB_PRINT: {
        std::string line;
        for (int64_t i = 0; i < nargs; i++) {
            if (i) line += " ";
            line += value_str(argv[i]);
        }
        line += "\n";
        fwrite(line.data(), 1, line.size(), stdout);
        set_none(out);
        return;
    }
    case KB_LEN: {
        check_arity(nargs, 1, 1, "len");
        KamiValue* v = argv[0];
        switch (v->tag) {
        case KT_STR: set_int(out, ((KamiStr*)v->p)->len); return;
        case KT_LIST: set_int(out, ((KamiList*)v->p)->len); return;
        case KT_MAP: set_int(out, ((KamiMap*)v->p)->count); return;
        default: panic(std::string("object of type '") + type_name(v->tag) + "' has no len()");
        }
    }
    case KB_STR: check_arity(nargs, 1, 1, "str"); builtin_str(out, argv[0]); return;
    case KB_INT: check_arity(nargs, 1, 1, "int"); builtin_int(out, argv[0]); return;
    case KB_FLOAT: check_arity(nargs, 1, 1, "float"); builtin_float(out, argv[0]); return;
    case KB_ABS: {
        check_arity(nargs, 1, 1, "abs");
        KamiValue* v = argv[0];
        if (v->tag == KT_INT || v->tag == KT_BOOL) { set_int(out, v->i < 0 ? -v->i : v->i); return; }
        if (v->tag == KT_FLOAT) { set_float(out, std::fabs(v->f)); return; }
        panic("abs() expected a number");
    }
    case KB_MIN:
    case KB_MAX: {
        const char* fn = id == KB_MIN ? "min" : "max";
        check_arity(nargs, 2, 16, fn);
        KamiValue best = *argv[0];
        for (int64_t i = 1; i < nargs; i++) {
            KamiValue r;
            kami_binop(id == KB_MIN ? KOP_LT : KOP_GT, &r, argv[i], &best);
            if (r.i) best = *argv[i];
        }
        *out = best;
        return;
    }
    case KB_ORD: {
        check_arity(nargs, 1, 1, "ord");
        if (argv[0]->tag != KT_STR || ((KamiStr*)argv[0]->p)->len != 1)
            panic("ord() expected a string of length 1");
        set_int(out, (unsigned char)((KamiStr*)argv[0]->p)->data[0]);
        return;
    }
    case KB_CHR: {
        check_arity(nargs, 1, 1, "chr");
        char c = (char)arg_int(argv[0], "chr");
        out->tag = KT_STR;
        out->p = str_new(&c, 1);
        return;
    }
    case KB_TYPE: {
        check_arity(nargs, 1, 1, "type");
        const char* n = type_name(argv[0]->tag);
        out->tag = KT_STR;
        out->p = str_new(n, (int64_t)strlen(n));
        return;
    }
    case KB_RANGE: {
        check_arity(nargs, 1, 3, "range");
        int64_t start = 0, stop, step = 1;
        if (nargs == 1) stop = arg_int(argv[0], "range");
        else {
            start = arg_int(argv[0], "range");
            stop = arg_int(argv[1], "range");
            if (nargs == 3) step = arg_int(argv[2], "range");
        }
        if (step == 0) panic("range() step must not be zero");
        KamiList* l = list_new(8);
        out->tag = KT_LIST;
        out->p = l; // root via out before pushes (which may trigger GC)
        KamiValue v{KT_INT, {0}};
        if (step > 0)
            for (int64_t i = start; i < stop; i += step) { v.i = i; list_push(l, &v); }
        else
            for (int64_t i = start; i > stop; i += step) { v.i = i; list_push(l, &v); }
        return;
    }
    // ---- math ----
    case KB_MATH_SQRT: check_arity(nargs, 1, 1, "math.sqrt"); set_float(out, std::sqrt(arg_num(argv[0], "math.sqrt"))); return;
    case KB_MATH_SIN: check_arity(nargs, 1, 1, "math.sin"); set_float(out, std::sin(arg_num(argv[0], "math.sin"))); return;
    case KB_MATH_COS: check_arity(nargs, 1, 1, "math.cos"); set_float(out, std::cos(arg_num(argv[0], "math.cos"))); return;
    case KB_MATH_TAN: check_arity(nargs, 1, 1, "math.tan"); set_float(out, std::tan(arg_num(argv[0], "math.tan"))); return;
    case KB_MATH_EXP: check_arity(nargs, 1, 1, "math.exp"); set_float(out, std::exp(arg_num(argv[0], "math.exp"))); return;
    case KB_MATH_LOG: check_arity(nargs, 1, 1, "math.log"); set_float(out, std::log(arg_num(argv[0], "math.log"))); return;
    case KB_MATH_POW: check_arity(nargs, 2, 2, "math.pow"); set_float(out, std::pow(arg_num(argv[0], "math.pow"), arg_num(argv[1], "math.pow"))); return;
    case KB_MATH_FLOOR: check_arity(nargs, 1, 1, "math.floor"); set_int(out, (int64_t)std::floor(arg_num(argv[0], "math.floor"))); return;
    case KB_MATH_CEIL: check_arity(nargs, 1, 1, "math.ceil"); set_int(out, (int64_t)std::ceil(arg_num(argv[0], "math.ceil"))); return;
    case KB_MATH_FABS: check_arity(nargs, 1, 1, "math.fabs"); set_float(out, std::fabs(arg_num(argv[0], "math.fabs"))); return;
    // ---- time ----
    case KB_TIME_TIME: {
        check_arity(nargs, 0, 0, "time.time");
        auto now = std::chrono::system_clock::now().time_since_epoch();
        set_float(out, std::chrono::duration<double>(now).count());
        return;
    }
    case KB_TIME_SLEEP: {
        check_arity(nargs, 1, 1, "time.sleep");
        double sec = arg_num(argv[0], "time.sleep");
        set_none(out);
        lk.unlock(); // release the GIL while sleeping
        std::this_thread::sleep_for(std::chrono::duration<double>(sec));
        lk.lock();
        return;
    }
    // ---- random ----
    case KB_RANDOM_RANDOM: {
        check_arity(nargs, 0, 0, "random.random");
        set_float(out, (double)(g_rng() >> 11) * (1.0 / 9007199254740992.0));
        return;
    }
    case KB_RANDOM_RANDINT: {
        check_arity(nargs, 2, 2, "random.randint");
        int64_t a = arg_int(argv[0], "random.randint");
        int64_t b = arg_int(argv[1], "random.randint");
        if (a > b) panic("random.randint(): empty range");
        set_int(out, a + (int64_t)(g_rng() % (uint64_t)(b - a + 1)));
        return;
    }
    case KB_RANDOM_SEED: {
        check_arity(nargs, 1, 1, "random.seed");
        g_rng.seed((uint64_t)arg_int(argv[0], "random.seed"));
        set_none(out);
        return;
    }
    // ---- threading ----
    case KB_THREAD_SPAWN: builtin_spawn(out, argv, nargs); return;
    case KB_THREAD_JOIN: builtin_join(lk, out, argv, nargs); return;
    default: panic("unknown builtin id " + std::to_string(id));
    }
}

} // namespace kami

using namespace kami;

extern "C" void kami_builtin(int64_t id, KamiValue* out, KamiValue** argv, int64_t nargs) {
    std::unique_lock<std::recursive_mutex> lk(g_lock);
    dispatch(lk, id, out, argv, nargs);
}
