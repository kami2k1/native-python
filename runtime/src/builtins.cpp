// Builtin functions: print/len/str/... plus math, time, random, threading modules.
#include "../include/kami_builtins.h"
#include "rt_internal.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <random>

namespace kami {

bool dispatch_netio(std::unique_lock<std::recursive_mutex>& lk, int64_t id, KamiValue* out,
                    KamiValue** argv, int64_t nargs);
void socket_method(std::unique_lock<std::recursive_mutex>& lk, KamiValue* out, KamiValue* obj,
                   const std::string& m, KamiValue** argv, int64_t nargs);

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

// Python-like format(value, spec): [[fill]align][sign][width][,][.prec][type]
static std::string format_value(KamiValue* v, const std::string& spec) {
    char fill = ' ';
    char align = 0;
    char sign = 0;
    char type = 0;
    long width = 0;
    long prec = -1;
    size_t i = 0;
    if (spec.size() >= 2 && (spec[1] == '<' || spec[1] == '>' || spec[1] == '^')) {
        fill = spec[0];
        align = spec[1];
        i = 2;
    } else if (!spec.empty() && (spec[0] == '<' || spec[0] == '>' || spec[0] == '^')) {
        align = spec[0];
        i = 1;
    }
    if (i < spec.size() && (spec[i] == '+' || spec[i] == '-' || spec[i] == ' ')) {
        sign = spec[i];
        i++;
    }
    if (i < spec.size() && spec[i] == '0' && !align) {
        fill = '0';
        align = '>';
        i++;
    }
    while (i < spec.size() && isdigit((unsigned char)spec[i])) {
        width = width * 10 + (spec[i] - '0');
        i++;
    }
    if (i < spec.size() && spec[i] == ',') i++; // thousands sep: ignored
    if (i < spec.size() && spec[i] == '.') {
        i++;
        prec = 0;
        while (i < spec.size() && isdigit((unsigned char)spec[i])) {
            prec = prec * 10 + (spec[i] - '0');
            i++;
        }
    }
    if (i < spec.size()) {
        type = spec[i];
        i++;
    }
    if (i != spec.size()) panic("unsupported format spec: '" + spec + "'");

    std::string body;
    char buf[64];
    auto num = [&](double d) {
        switch (type) {
        case 'f': case 'F':
            snprintf(buf, sizeof buf, "%.*f", prec < 0 ? 6 : (int)prec, d);
            body = buf;
            break;
        case 'e': case 'E':
            snprintf(buf, sizeof buf, type == 'e' ? "%.*e" : "%.*E",
                     prec < 0 ? 6 : (int)prec, d);
            body = buf;
            break;
        case 'g': case 'G':
            snprintf(buf, sizeof buf, type == 'g' ? "%.*g" : "%.*G",
                     prec < 0 ? 6 : (int)prec, d);
            body = buf;
            break;
        case '%':
            snprintf(buf, sizeof buf, "%.*f%%", prec < 0 ? 6 : (int)prec, d * 100.0);
            body = buf;
            break;
        default:
            panic("unsupported format type for float: '" + spec + "'");
        }
    };
    if (v->tag == KT_INT || v->tag == KT_BOOL) {
        switch (type) {
        case 0: case 'd':
            snprintf(buf, sizeof buf, "%lld", (long long)v->i);
            body = buf;
            break;
        case 'x':
            snprintf(buf, sizeof buf, "%llx", (unsigned long long)v->i);
            body = buf;
            break;
        case 'X':
            snprintf(buf, sizeof buf, "%llX", (unsigned long long)v->i);
            body = buf;
            break;
        case 'o':
            snprintf(buf, sizeof buf, "%llo", (unsigned long long)v->i);
            body = buf;
            break;
        case 'b': {
            uint64_t u = (uint64_t)(v->i < 0 ? -v->i : v->i);
            std::string d2;
            if (!u) d2 = "0";
            while (u) { d2 += (char)('0' + (u & 1)); u >>= 1; }
            if (v->i < 0) body = "-";
            for (size_t k = d2.size(); k-- > 0;) body += d2[k];
            break;
        }
        case 'f': case 'F': case 'e': case 'E': case 'g': case 'G': case '%':
            num((double)v->i);
            break;
        default:
            panic("unsupported format type for int: '" + spec + "'");
        }
        if (sign == '+' && v->i >= 0) body = "+" + body;
    } else if (v->tag == KT_FLOAT) {
        if (type == 0) body = format_float(v->f);
        else num(v->f);
        if (sign == '+' && v->f >= 0) body = "+" + body;
    } else {
        if (type != 0 && type != 's') panic("unsupported format spec for value: '" + spec + "'");
        body = value_str(v);
        if (prec >= 0 && (long)body.size() > prec) body.resize((size_t)prec);
        if (!align) align = '<';
    }
    if (!align) align = '>';
    if ((long)body.size() < width) {
        size_t pad = (size_t)width - body.size();
        if (align == '>') body = std::string(pad, fill) + body;
        else if (align == '<') body += std::string(pad, fill);
        else {
            body = std::string(pad / 2, fill) + body + std::string(pad - pad / 2, fill);
        }
    }
    return body;
}

// Invoke a user function from inside a builtin. The lock is released around
// the call; out/argv must point into rooted storage.
static void call_user(std::unique_lock<std::recursive_mutex>& lk, const KamiValue* fnv,
                      KamiValue* out, KamiValue** argv, int64_t n) {
    lk.unlock();
    kami_call_value(out, fnv, argv, n);
    lk.lock();
}

static void set_float(KamiValue* out, double d) { out->tag = KT_FLOAT; out->f = d; }
static void set_int(KamiValue* out, int64_t i) { out->tag = KT_INT; out->i = i; }
static void set_none(KamiValue* out) { out->tag = KT_NONE; out->i = 0; }

// Materialize an iterable into a list value written to *out. The result is
// pinned as a GC root until unpin_scratch() is called; callers must pair them.
static void as_list_pinned(KamiValue* out, KamiValue* v) {
    if (v->tag == KT_LIST) { *out = *v; }
    else kami_iter_prep(out, v); // dict/set/str/file → list
    g_pins.push_back({out, 1});
}
static void unpin_scratch(KamiValue* out) {
    for (size_t i = g_pins.size(); i-- > 0;) {
        if (g_pins[i].first == out) {
            g_pins.erase(g_pins.begin() + (long)i);
            break;
        }
    }
}

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
    try {
        kami_call_value(&slots[0], &slots[1], argv, nargs);
    } catch (KamiError& e) {
        fflush(stdout);
        fprintf(stderr, "KamiPython runtime error (in thread): %s\n", e.msg.c_str());
        fflush(stderr);
        _Exit(1);
    }
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
        case KT_MAP:
        case KT_SET: set_int(out, ((KamiMap*)v->p)->count); return;
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
        check_arity(nargs, 1, 16, fn);
        // min(list) / max(list) form
        if (nargs == 1) {
            KamiValue mmv;
            as_list_pinned(&mmv, argv[0]);
            KamiList* l = (KamiList*)mmv.p;
            if (l->len == 0) { unpin_scratch(&mmv); panic(std::string(fn) + "() of empty sequence"); }
            KamiValue best = l->items[0];
            for (int64_t i = 1; i < l->len; i++) {
                KamiValue r;
                kami_binop(id == KB_MIN ? KOP_LT : KOP_GT, &r, &l->items[i], &best);
                if (r.i) best = l->items[i];
            }
            unpin_scratch(&mmv);
            *out = best;
            return;
        }
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
    // ---- extended builtins ----
    case KB_SUM: {
        check_arity(nargs, 1, 2, "sum");
        KamiValue sumv;
        as_list_pinned(&sumv, argv[0]);
        KamiList* l = (KamiList*)sumv.p;
        KamiValue acc;
        if (nargs == 2) acc = *argv[1];
        else { acc.tag = KT_INT; acc.i = 0; }
        for (int64_t i = 0; i < l->len; i++) {
            KamiValue r;
            kami_binop(KOP_ADD, &r, &acc, &l->items[i]);
            acc = r;
        }
        unpin_scratch(&sumv);
        *out = acc;
        return;
    }
    case KB_SORTED: {
        check_arity(nargs, 1, 3, "sorted");
        KamiValue srcv;
        as_list_pinned(&srcv, argv[0]);
        argv[0] = &srcv;
        bool has_key = nargs >= 2 && argv[1]->tag != KT_NONE;
        bool reverse = nargs >= 3 && kami_truthy(argv[2]);
        KamiList* src = (KamiList*)argv[0]->p;
        int64_t n = src->len;
        if (!has_key) {
            KamiList* r = list_new(n);
            out->tag = KT_LIST;
            out->p = r;
            for (int64_t i = 0; i < n; i++) r->items[r->len++] = src->items[i];
            std::sort(r->items, r->items + r->len,
                      [](const KamiValue& a, const KamiValue& b) {
                          KamiValue c;
                          kami_binop(KOP_LT, &c, &a, &b);
                          return c.i != 0;
                      });
            if (reverse)
                for (int64_t i = 0, j = r->len - 1; i < j; i++, j--)
                    std::swap(r->items[i], r->items[j]);
            unpin_scratch(&srcv);
            return;
        }
        // decorate-sort-undecorate; pairs live in a rooted list (via out)
        KamiValue keyfn = *argv[1];
        KamiList* pairs = list_new(n);
        out->tag = KT_LIST;
        out->p = pairs;
        for (int64_t i = 0; i < n; i++) {
            KamiList* pair = list_new(2);
            pair->items[0].tag = KT_NONE;
            pair->items[1] = src->items[i];
            pair->len = 2;
            KamiValue pv{KT_LIST, {0}};
            pv.p = pair;
            pairs->items[pairs->len++] = pv;
        }
        for (int64_t i = 0; i < n; i++) {
            KamiList* pair = (KamiList*)pairs->items[i].p;
            KamiValue* item = &pair->items[1]; // heap slot: rooted + stable
            call_user(lk, &keyfn, &pair->items[0], &item, 1);
        }
        std::sort(pairs->items, pairs->items + n,
                  [](const KamiValue& a, const KamiValue& b) {
                      KamiValue c;
                      kami_binop(KOP_LT, &c, &((KamiList*)a.p)->items[0],
                                 &((KamiList*)b.p)->items[0]);
                      return c.i != 0;
                  });
        KamiList* r = list_new(n);
        out->tag = KT_LIST;
        out->p = r;
        for (int64_t i = 0; i < n; i++) {
            int64_t src_i = reverse ? n - 1 - i : i;
            r->items[r->len++] = ((KamiList*)pairs->items[src_i].p)->items[1];
        }
        unpin_scratch(&srcv);
        return;
    }
    case KB_REVERSED: {
        check_arity(nargs, 1, 1, "reversed");
        KamiValue revv;
        as_list_pinned(&revv, argv[0]);
        KamiList* src = (KamiList*)revv.p;
        KamiList* r = list_new(src->len);
        out->tag = KT_LIST;
        out->p = r;
        for (int64_t i = src->len - 1; i >= 0; i--) r->items[r->len++] = src->items[i];
        unpin_scratch(&revv);
        return;
    }
    case KB_ENUMERATE: {
        check_arity(nargs, 1, 1, "enumerate");
        KamiValue* v = argv[0];
        KamiList* r = list_new(4);
        out->tag = KT_LIST;
        out->p = r;
        auto push_pair = [&](int64_t i, const KamiValue* item) {
            KamiList* pair = list_new(2);
            pair->items[0].tag = KT_INT;
            pair->items[0].i = i;
            pair->items[1] = *item;
            pair->len = 2;
            KamiValue pv;
            pv.tag = KT_LIST;
            pv.p = pair;
            list_push(r, &pv);
        };
        if (v->tag == KT_LIST) {
            KamiList* l = (KamiList*)v->p;
            for (int64_t i = 0; i < l->len; i++) push_pair(i, &l->items[i]);
        } else if (v->tag == KT_STR) {
            KamiStr* s = (KamiStr*)v->p;
            for (int64_t i = 0; i < s->len; i++) {
                KamiValue ch;
                ch.tag = KT_STR;
                ch.p = str_new(s->data + i, 1);
                push_pair(i, &ch);
            }
        } else {
            panic("enumerate() expected a list or str");
        }
        return;
    }
    case KB_ZIP: {
        check_arity(nargs, 2, 2, "zip");
        if (argv[0]->tag != KT_LIST || argv[1]->tag != KT_LIST)
            panic("zip() expects two lists");
        KamiList* a = (KamiList*)argv[0]->p;
        KamiList* b = (KamiList*)argv[1]->p;
        int64_t n = a->len < b->len ? a->len : b->len;
        KamiList* r = list_new(n > 4 ? n : 4);
        out->tag = KT_LIST;
        out->p = r;
        for (int64_t i = 0; i < n; i++) {
            KamiList* pair = list_new(2);
            pair->items[0] = a->items[i];
            pair->items[1] = b->items[i];
            pair->len = 2;
            KamiValue pv;
            pv.tag = KT_LIST;
            pv.p = pair;
            list_push(r, &pv);
        }
        return;
    }
    case KB_BOOL: {
        check_arity(nargs, 1, 1, "bool");
        out->tag = KT_BOOL;
        out->i = kami_truthy(argv[0]);
        return;
    }
    case KB_ROUND: {
        check_arity(nargs, 1, 2, "round");
        double x = arg_num(argv[0], "round");
        if (nargs == 1) {
            set_int(out, (int64_t)llround(x));
        } else {
            int64_t nd = arg_int(argv[1], "round");
            double p = std::pow(10.0, (double)nd);
            set_float(out, std::llround(x * p) / p);
        }
        return;
    }
    case KB_INPUT: {
        check_arity(nargs, 0, 1, "input");
        if (nargs == 1) {
            std::string prompt = value_str(argv[0]);
            fwrite(prompt.data(), 1, prompt.size(), stdout);
            fflush(stdout);
        }
        std::string line;
        lk.unlock(); // don't hold the GIL while blocked on stdin
        int c;
        bool got = false;
        while ((c = fgetc(stdin)) != EOF && c != '\n') {
            line += (char)c;
            got = true;
        }
        if (c == '\n') got = true;
        lk.lock();
        if (!line.empty() && line.back() == '\r') line.pop_back();
        (void)got;
        out->tag = KT_STR;
        out->p = str_new(line.data(), (int64_t)line.size());
        return;
    }
    case KB_POW: {
        check_arity(nargs, 2, 2, "pow");
        kami_binop(KOP_POW, out, argv[0], argv[1]);
        return;
    }
    case KB_ALL:
    case KB_ANY: {
        const char* fn = id == KB_ALL ? "all" : "any";
        check_arity(nargs, 1, 1, fn);
        KamiValue allv;
        as_list_pinned(&allv, argv[0]);
        KamiList* l = (KamiList*)allv.p;
        bool result = id == KB_ALL;
        for (int64_t i = 0; i < l->len; i++) {
            bool t = kami_truthy(&l->items[i]) != 0;
            if (id == KB_ALL && !t) { result = false; break; }
            if (id == KB_ANY && t) { result = true; break; }
        }
        unpin_scratch(&allv);
        out->tag = KT_BOOL;
        out->i = result;
        return;
    }
    case KB_BIN:
    case KB_HEX:
    case KB_OCT: {
        const char* fn = id == KB_BIN ? "bin" : id == KB_HEX ? "hex" : "oct";
        check_arity(nargs, 1, 1, fn);
        int64_t v = arg_int(argv[0], fn);
        bool neg = v < 0;
        uint64_t u = neg ? (uint64_t)(-v) : (uint64_t)v;
        std::string digits;
        int base = id == KB_BIN ? 2 : id == KB_HEX ? 16 : 8;
        const char* dc = "0123456789abcdef";
        if (u == 0) digits = "0";
        while (u) {
            digits += dc[u % (uint64_t)base];
            u /= (uint64_t)base;
        }
        std::string r = neg ? "-" : "";
        r += id == KB_BIN ? "0b" : id == KB_HEX ? "0x" : "0o";
        for (size_t i = digits.size(); i-- > 0;) r += digits[i];
        out->tag = KT_STR;
        out->p = str_new(r.data(), (int64_t)r.size());
        return;
    }
    case KB_LIST:
    case KB_TUPLE: {
        check_arity(nargs, 0, 1, id == KB_LIST ? "list" : "tuple");
        KamiList* r = list_new(4);
        out->tag = KT_LIST;
        out->p = r;
        if (nargs == 0) return;
        if (argv[0]->tag == KT_LIST) {
            KamiList* src = (KamiList*)argv[0]->p;
            for (int64_t i = 0; i < src->len; i++) list_push(r, &src->items[i]);
        } else if (argv[0]->tag == KT_STR) {
            KamiStr* s2 = (KamiStr*)argv[0]->p;
            for (int64_t i = 0; i < s2->len; i++) {
                KamiValue ch;
                ch.tag = KT_STR;
                ch.p = str_new(s2->data + i, 1);
                list_push(r, &ch);
            }
        } else {
            panic("list() expected a list or str");
        }
        return;
    }
    case KB_DICT: {
        check_arity(nargs, 0, 0, "dict");
        KamiMap* m = map_new();
        out->tag = KT_MAP;
        out->p = m;
        return;
    }
    case KB_ISINSTANCE: {
        check_arity(nargs, 2, 2, "isinstance");
        auto matches = [&](const KamiValue* spec) -> bool {
            if (spec->tag == KT_STR) {
                KamiStr* s2 = (KamiStr*)spec->p;
                std::string t(s2->data, (size_t)s2->len);
                int64_t tag = argv[0]->tag;
                if (t == "int") return tag == KT_INT || tag == KT_BOOL;
                if (t == "float") return tag == KT_FLOAT;
                if (t == "str") return tag == KT_STR;
                if (t == "bool") return tag == KT_BOOL;
                if (t == "list" || t == "tuple") return tag == KT_LIST;
                if (t == "dict") return tag == KT_MAP;
                return false;
            }
            if (spec->tag == KT_CLASS) {
                if (argv[0]->tag != KT_OBJECT) return false;
                KamiClassObj* want = (KamiClassObj*)spec->p;
                KamiClassObj* c = ((KamiInstance*)argv[0]->p)->cls;
                while (c) {
                    if (c == want) return true;
                    c = c->parent;
                }
                return false;
            }
            panic("isinstance() arg 2 must be a type or class");
        };
        bool r = false;
        if (argv[1]->tag == KT_LIST) {
            KamiList* l = (KamiList*)argv[1]->p;
            for (int64_t i = 0; i < l->len && !r; i++) r = matches(&l->items[i]);
        } else {
            r = matches(argv[1]);
        }
        out->tag = KT_BOOL;
        out->i = r;
        return;
    }
    case KB_FORMAT: {
        check_arity(nargs, 1, 2, "format");
        std::string spec;
        if (nargs == 2) {
            if (argv[1]->tag != KT_STR) panic("format() spec must be a str");
            KamiStr* s2 = (KamiStr*)argv[1]->p;
            spec.assign(s2->data, (size_t)s2->len);
        }
        std::string r = format_value(argv[0], spec);
        out->tag = KT_STR;
        out->p = str_new(r.data(), (int64_t)r.size());
        return;
    }
    case KB_NOOP: {
        set_none(out);
        return;
    }
    case KB_SYS_EXIT: {
        check_arity(nargs, 0, 1, "sys.exit");
        int64_t code = nargs ? arg_int(argv[0], "sys.exit") : 0;
        fflush(stdout);
        _Exit((int)code);
    }
    case KB_SYS_ARGV: {
        KamiList* r = list_new(g_argc > 4 ? g_argc : 4);
        out->tag = KT_LIST;
        out->p = r;
        for (int64_t i = 0; i < g_argc; i++) {
            KamiValue v;
            v.tag = KT_STR;
            v.p = str_new(g_argv[i], (int64_t)strlen(g_argv[i]));
            list_push(r, &v);
        }
        return;
    }
    case KB_DIVMOD: {
        check_arity(nargs, 2, 2, "divmod");
        KamiValue q, r2;
        kami_binop(KOP_FLOORDIV, &q, argv[0], argv[1]);
        kami_binop(KOP_MOD, &r2, argv[0], argv[1]);
        KamiList* l = list_new(2);
        l->items[0] = q;
        l->items[1] = r2;
        l->len = 2;
        out->tag = KT_LIST;
        out->p = l;
        return;
    }
    case KB_SET: {
        check_arity(nargs, 0, 1, "set");
        kami_make_set(out);
        if (nargs == 1) {
            KamiValue seq;
            kami_iter_prep(&seq, argv[0]); // seq is a fresh list...
            // ...not yet rooted: re-derive it while still under our lock and
            // push elements immediately (no allocation between prep and adds
            // except set growth, which keeps `seq` reachable? No — root it:)
            KamiList* l = (KamiList*)seq.p;
            KamiValue none{KT_NONE, {0}};
            for (int64_t i = 0; i < l->len; i++)
                map_set((KamiMap*)out->p, &l->items[i], &none);
        }
        return;
    }
    case KB_OPEN: {
        check_arity(nargs, 1, 2, "open");
        if (argv[0]->tag != KT_STR) panic("open() expected a path string");
        KamiStr* path = (KamiStr*)argv[0]->p;
        std::string mode = "r";
        if (nargs == 2) {
            if (argv[1]->tag != KT_STR) panic("open() mode must be a str");
            KamiStr* ms = (KamiStr*)argv[1]->p;
            mode.assign(ms->data, (size_t)ms->len);
        }
        std::string m2;
        for (char c : mode)
            if (c != 't' && c != 'U') m2 += c; // text mode markers: ignored
        if (m2.empty()) m2 = "r";
        FILE* fp = fopen(path->data, m2.c_str());
        if (!fp)
            panic(std::string("FileNotFoundError: cannot open '") + path->data + "' (mode '" +
                  mode + "')");
        KamiFile* f = (KamiFile*)gc_alloc(sizeof(KamiFile), KT_FILE);
        f->fp = fp;
        f->closed = false;
        out->tag = KT_FILE;
        out->p = f;
        return;
    }
    case KB_WITH_ENTER: {
        check_arity(nargs, 1, 1, "with");
        if (argv[0]->tag == KT_OBJECT) {
            KamiInstance* in = (KamiInstance*)argv[0]->p;
            if (KamiValue* mv = class_lookup(in->cls, "__enter__")) {
                KamiValue fnv = *mv;
                if (fnv.tag == KT_FUNC) {
                    // bound call: self = argv[0]
                    KamiValue* a2[1] = {argv[0]};
                    KamiFuncObj* fo = (KamiFuncObj*)fnv.p;
                    if (fo->arity != 1) panic("__enter__() must take only self");
                    call_user(lk, &fnv, out, a2, 1);
                    return;
                }
            }
        }
        *out = *argv[0];
        return;
    }
    case KB_WITH_EXIT: {
        check_arity(nargs, 1, 1, "with");
        set_none(out);
        if (argv[0]->tag == KT_FILE) {
            KamiFile* f = (KamiFile*)argv[0]->p;
            if (!f->closed) {
                fclose((FILE*)f->fp);
                f->closed = true;
            }
            return;
        }
        if (argv[0]->tag == KT_OBJECT) {
            KamiInstance* in = (KamiInstance*)argv[0]->p;
            KamiValue* mv = class_lookup(in->cls, "__exit__");
            const char* name = "__exit__";
            if (!mv) {
                mv = class_lookup(in->cls, "close");
                name = "close";
            }
            if (mv && mv->tag == KT_FUNC) {
                KamiFuncObj* fo = (KamiFuncObj*)mv->p;
                KamiValue fnv = *mv;
                KamiValue nones[3] = {{KT_NONE, {0}}, {KT_NONE, {0}}, {KT_NONE, {0}}};
                KamiValue* a2[4] = {argv[0], &nones[0], &nones[1], &nones[2]};
                int64_t want = fo->arity; // includes self
                if (want != 1 && want != 4)
                    panic(std::string(name) + "() must take (self) or (self, exc_type, "
                                              "exc_val, tb)");
                KamiValue dummy{KT_NONE, {0}};
                call_user(lk, &fnv, &dummy, a2, want);
            }
            return;
        }
        return; // other values: nothing to clean up
    }
    case KB_PRINT_EX: {
        // args: [sep, end, values...]
        if (nargs < 2) panic("print(): bad kwargs form");
        std::string sep = value_str(argv[0]);
        std::string end = value_str(argv[1]);
        std::string line;
        for (int64_t i = 2; i < nargs; i++) {
            if (i > 2) line += sep;
            line += value_str(argv[i]);
        }
        line += end;
        fwrite(line.data(), 1, line.size(), stdout);
        set_none(out);
        return;
    }
    default:
        if (dispatch_netio(lk, id, out, argv, nargs)) return;
        panic("unknown builtin id " + std::to_string(id));
    }
}

} // namespace kami

using namespace kami;

extern "C" void kami_builtin(int64_t id, KamiValue* out, KamiValue** argv, int64_t nargs) {
    std::unique_lock<std::recursive_mutex> lk(g_lock);
    dispatch(lk, id, out, argv, nargs);
}
