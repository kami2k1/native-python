// Dynamic operators, indexing, iteration and calls.
#include "rt_internal.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstring>

namespace kami {

static double as_num(const KamiValue* v) {
    if (v->tag == KT_INT || v->tag == KT_BOOL) return (double)v->i;
    if (v->tag == KT_FLOAT) return v->f;
    panic(std::string("expected a number, got '") + type_name(v->tag) + "'");
}

static bool is_num(const KamiValue* v) {
    return v->tag == KT_INT || v->tag == KT_BOOL || v->tag == KT_FLOAT;
}

static void num_result_int(KamiValue* out, int64_t v) { out->tag = KT_INT; out->i = v; }
static void num_result_float(KamiValue* out, double v) { out->tag = KT_FLOAT; out->f = v; }
static void bool_result(KamiValue* out, bool b) { out->tag = KT_BOOL; out->i = b; }

static int compare(const KamiValue* a, const KamiValue* b, const char* opname) {
    if (is_num(a) && is_num(b)) {
        double x = as_num(a), y = as_num(b);
        return x < y ? -1 : (x > y ? 1 : 0);
    }
    if (a->tag == KT_STR && b->tag == KT_STR) {
        KamiStr *x = (KamiStr*)a->p, *y = (KamiStr*)b->p;
        int64_t n = x->len < y->len ? x->len : y->len;
        int c = n ? memcmp(x->data, y->data, (size_t)n) : 0;
        if (c) return c < 0 ? -1 : 1;
        return x->len < y->len ? -1 : (x->len > y->len ? 1 : 0);
    }
    panic(std::string("'") + opname + "' not supported between '" + type_name(a->tag) +
          "' and '" + type_name(b->tag) + "'");
}

static void op_in(KamiValue* out, const KamiValue* a, const KamiValue* b) {
    switch (b->tag) {
    case KT_LIST: {
        KamiList* l = (KamiList*)b->p;
        for (int64_t i = 0; i < l->len; i++)
            if (value_eq(&l->items[i], a)) { bool_result(out, true); return; }
        bool_result(out, false);
        return;
    }
    case KT_MAP:
    case KT_SET: {
        KamiValue tmp;
        bool_result(out, map_get((KamiMap*)b->p, a, &tmp));
        return;
    }
    case KT_STR: {
        if (a->tag != KT_STR) panic("'in <str>' requires a str operand");
        KamiStr *n = (KamiStr*)a->p, *h = (KamiStr*)b->p;
        if (n->len == 0) { bool_result(out, true); return; }
        for (int64_t i = 0; i + n->len <= h->len; i++)
            if (memcmp(h->data + i, n->data, (size_t)n->len) == 0) {
                bool_result(out, true);
                return;
            }
        bool_result(out, false);
        return;
    }
    default:
        panic(std::string("argument of type '") + type_name(b->tag) + "' is not iterable");
    }
}

static void binop_impl(int64_t op, KamiValue* out, const KamiValue* a, const KamiValue* b) {
    switch (op) {
    case KOP_ADD:
        if (a->tag == KT_INT && b->tag == KT_INT) { num_result_int(out, a->i + b->i); return; }
        if (is_num(a) && is_num(b)) { num_result_float(out, as_num(a) + as_num(b)); return; }
        if (a->tag == KT_STR && b->tag == KT_STR) {
            KamiStr *x = (KamiStr*)a->p, *y = (KamiStr*)b->p;
            std::string s(x->data, (size_t)x->len);
            s.append(y->data, (size_t)y->len);
            KamiStr* r = str_new(s.data(), (int64_t)s.size());
            out->tag = KT_STR;
            out->p = r;
            return;
        }
        if (a->tag == KT_LIST && b->tag == KT_LIST) {
            KamiList *x = (KamiList*)a->p, *y = (KamiList*)b->p;
            KamiList* r = list_new(x->len + y->len);
            for (int64_t i = 0; i < x->len; i++) r->items[r->len++] = x->items[i];
            for (int64_t i = 0; i < y->len; i++) r->items[r->len++] = y->items[i];
            out->tag = KT_LIST;
            out->p = r;
            return;
        }
        panic(std::string("unsupported operand types for +: '") + type_name(a->tag) +
              "' and '" + type_name(b->tag) + "'");
    case KOP_SUB:
        if (a->tag == KT_INT && b->tag == KT_INT) { num_result_int(out, a->i - b->i); return; }
        if (a->tag == KT_SET && b->tag == KT_SET) {
            KamiMap *x = (KamiMap*)a->p, *y = (KamiMap*)b->p;
            kami_make_set(out);
            KamiMap* r = (KamiMap*)out->p;
            KamiValue none{KT_NONE, {0}};
            KamiValue tmp;
            for (int64_t i = 0; i < x->nentries; i++) {
                if (!x->entries[i].used) continue;
                if (!map_get(y, &x->entries[i].key, &tmp))
                    map_set(r, &x->entries[i].key, &none);
            }
            return;
        }
        num_result_float(out, as_num(a) - as_num(b));
        return;
    case KOP_MUL: {
        if (a->tag == KT_INT && b->tag == KT_INT) { num_result_int(out, a->i * b->i); return; }
        if (is_num(a) && is_num(b)) { num_result_float(out, as_num(a) * as_num(b)); return; }
        // sequence repetition: "ab" * 3 / 3 * "ab" / [0] * n / n * [0]
        const KamiValue* seq = nullptr;
        const KamiValue* cnt = nullptr;
        if ((a->tag == KT_STR || a->tag == KT_LIST) && (b->tag == KT_INT || b->tag == KT_BOOL)) {
            seq = a;
            cnt = b;
        } else if ((b->tag == KT_STR || b->tag == KT_LIST) &&
                   (a->tag == KT_INT || a->tag == KT_BOOL)) {
            seq = b;
            cnt = a;
        }
        if (seq) {
            int64_t n = cnt->i < 0 ? 0 : cnt->i;
            if (seq->tag == KT_STR) {
                KamiStr* x = (KamiStr*)seq->p;
                std::string s;
                s.reserve((size_t)(x->len * n));
                for (int64_t i = 0; i < n; i++) s.append(x->data, (size_t)x->len);
                put_str(out, s.data(), (int64_t)s.size());
            } else {
                KamiList* x = (KamiList*)seq->p;
                // list_new is the only allocation here (and gc_alloc collects
                // *before* allocating), so r stays reachable until we publish it
                // — and publishing last keeps `seq` readable even if out aliases it.
                KamiList* r = list_new(x->len * n > 0 ? x->len * n : 1);
                for (int64_t i = 0; i < n; i++)
                    for (int64_t k = 0; k < x->len; k++) r->items[r->len++] = x->items[k];
                out->tag = KT_LIST;
                out->p = r;
            }
            return;
        }
        panic(std::string("unsupported operand types for *: '") + type_name(a->tag) +
              "' and '" + type_name(b->tag) + "'");
    }
    case KOP_DIV: {
        double y = as_num(b);
        if (y == 0.0) panic("division by zero");
        num_result_float(out, as_num(a) / y);
        return;
    }
    case KOP_FLOORDIV:
        if (a->tag == KT_INT && b->tag == KT_INT) {
            if (b->i == 0) panic("integer division by zero");
            int64_t q = a->i / b->i;
            if ((a->i % b->i != 0) && ((a->i < 0) != (b->i < 0))) q--; // Python floor
            num_result_int(out, q);
            return;
        }
        {
            double y = as_num(b);
            if (y == 0.0) panic("float floor division by zero");
            num_result_float(out, std::floor(as_num(a) / y));
        }
        return;
    case KOP_MOD:
        if (a->tag == KT_INT && b->tag == KT_INT) {
            if (b->i == 0) panic("integer modulo by zero");
            int64_t r = a->i % b->i;
            if (r != 0 && ((r < 0) != (b->i < 0))) r += b->i; // Python sign
            num_result_int(out, r);
            return;
        }
        {
            double y = as_num(b);
            if (y == 0.0) panic("float modulo by zero");
            double r = std::fmod(as_num(a), y);
            if (r != 0 && ((r < 0) != (y < 0))) r += y;
            num_result_float(out, r);
        }
        return;
    case KOP_EQ: bool_result(out, value_eq(a, b)); return;
    case KOP_NE: bool_result(out, !value_eq(a, b)); return;
    case KOP_LT: bool_result(out, compare(a, b, "<") < 0); return;
    case KOP_GT: bool_result(out, compare(a, b, ">") > 0); return;
    case KOP_LE: bool_result(out, compare(a, b, "<=") <= 0); return;
    case KOP_GE: bool_result(out, compare(a, b, ">=") >= 0); return;
    case KOP_IN: op_in(out, a, b); return;
    case KOP_IS:
    case KOP_ISNOT: {
        bool same = a->tag == b->tag &&
                    (a->tag == KT_NONE || a->i == b->i); // payload identity
        bool_result(out, op == KOP_IS ? same : !same);
        return;
    }
    case KOP_BITAND:
    case KOP_BITOR:
    case KOP_BITXOR:
    case KOP_SHL:
    case KOP_SHR: {
        if (a->tag == KT_SET && b->tag == KT_SET &&
            (op == KOP_BITAND || op == KOP_BITOR || op == KOP_BITXOR)) {
            KamiMap *x = (KamiMap*)a->p, *y = (KamiMap*)b->p;
            kami_make_set(out); // rooted via out before inserts
            KamiMap* r = (KamiMap*)out->p;
            KamiValue none{KT_NONE, {0}};
            KamiValue tmp;
            for (int64_t i = 0; i < x->nentries; i++) {
                if (!x->entries[i].used) continue;
                bool in_y = map_get(y, &x->entries[i].key, &tmp);
                if ((op == KOP_BITOR) || (op == KOP_BITAND && in_y) ||
                    (op == KOP_BITXOR && !in_y))
                    map_set(r, &x->entries[i].key, &none);
            }
            if (op != KOP_BITAND) {
                for (int64_t i = 0; i < y->nentries; i++) {
                    if (!y->entries[i].used) continue;
                    bool in_x = map_get(x, &y->entries[i].key, &tmp);
                    if (op == KOP_BITOR || (op == KOP_BITXOR && !in_x))
                        map_set(r, &y->entries[i].key, &none);
                }
            }
            return;
        }
        bool ok_a = a->tag == KT_INT || a->tag == KT_BOOL;
        bool ok_b = b->tag == KT_INT || b->tag == KT_BOOL;
        if (!ok_a || !ok_b)
            panic(std::string("bitwise operators require integers, got '") +
                  type_name(a->tag) + "' and '" + type_name(b->tag) + "'");
        int64_t x = a->i, y = b->i;
        int64_t r;
        switch (op) {
        case KOP_BITAND: r = x & y; break;
        case KOP_BITOR: r = x | y; break;
        case KOP_BITXOR: r = x ^ y; break;
        case KOP_SHL:
            if (y < 0) panic("negative shift count");
            r = (int64_t)((uint64_t)x << (uint64_t)y);
            break;
        default:
            if (y < 0) panic("negative shift count");
            r = x >> y;
            break;
        }
        num_result_int(out, r);
        return;
    }
    case KOP_POW: {
        if (a->tag == KT_INT && b->tag == KT_INT && b->i >= 0) {
            int64_t base = a->i, exp = b->i, r = 1;
            while (exp > 0) {
                if (exp & 1) r *= base;
                base *= base;
                exp >>= 1;
            }
            num_result_int(out, r);
            return;
        }
        num_result_float(out, std::pow(as_num(a), as_num(b)));
        return;
    }
    default: panic("bad binop");
    }
}

static int64_t norm_index(int64_t i, int64_t len, const char* what) {
    int64_t j = i < 0 ? i + len : i;
    if (j < 0 || j >= len) panic(std::string(what) + " index out of range");
    return j;
}

} // namespace kami

using namespace kami;

extern "C" {

void kami_binop(int64_t op, KamiValue* out, const KamiValue* a, const KamiValue* b) {
    Lock lk(g_lock);
    KamiValue x = *a, y = *b; // read first: out may alias a or b
    KamiValue r;
    binop_impl(op, &r, &x, &y);
    *out = r;
}

void kami_unop(int64_t op, KamiValue* out, const KamiValue* a) {
    Lock lk(g_lock);
    KamiValue x = *a;
    if (op == KUOP_NEG) {
        if (x.tag == KT_INT || x.tag == KT_BOOL) { out->tag = KT_INT; out->i = -x.i; return; }
        if (x.tag == KT_FLOAT) { out->tag = KT_FLOAT; out->f = -x.f; return; }
        panic(std::string("bad operand type for unary -: '") + type_name(x.tag) + "'");
    }
    if (op == KUOP_INV) {
        if (x.tag == KT_INT || x.tag == KT_BOOL) { out->tag = KT_INT; out->i = ~x.i; return; }
        panic(std::string("bad operand type for unary ~: '") + type_name(x.tag) + "'");
    }
    // KUOP_NOT
    KamiValue tmp = x;
    int32_t t;
    switch (tmp.tag) {
    case KT_NONE: t = 0; break;
    case KT_BOOL:
    case KT_INT: t = tmp.i != 0; break;
    case KT_FLOAT: t = tmp.f != 0.0; break;
    case KT_STR: t = ((KamiStr*)tmp.p)->len != 0; break;
    case KT_LIST: t = ((KamiList*)tmp.p)->len != 0; break;
    case KT_MAP: t = ((KamiMap*)tmp.p)->count != 0; break;
    default: t = 1;
    }
    out->tag = KT_BOOL;
    out->i = !t;
}

void kami_index_get(KamiValue* out, const KamiValue* obj, const KamiValue* idx) {
    Lock lk(g_lock);
    KamiValue o = *obj, ix = *idx;
    switch (o.tag) {
    case KT_LIST: {
        if (ix.tag != KT_INT) panic("list indices must be integers");
        KamiList* l = (KamiList*)o.p;
        *out = l->items[norm_index(ix.i, l->len, "list")];
        return;
    }
    case KT_STR: {
        if (ix.tag != KT_INT) panic("string indices must be integers");
        KamiStr* s = (KamiStr*)o.p;
        int64_t j = norm_index(ix.i, s->len, "string");
        KamiStr* r = str_new(s->data + j, 1);
        out->tag = KT_STR;
        out->p = r;
        return;
    }
    case KT_MAP: {
        KamiMap* m = (KamiMap*)o.p;
        KamiValue v;
        if (!map_get(m, &ix, &v)) panic("KeyError: " + value_repr(&ix));
        *out = v;
        return;
    }
    default:
        panic(std::string("'") + type_name(o.tag) + "' object is not subscriptable");
    }
}

void kami_index_set(KamiValue* obj, const KamiValue* idx, const KamiValue* val) {
    Lock lk(g_lock);
    KamiValue o = *obj, ix = *idx, v = *val;
    switch (o.tag) {
    case KT_LIST: {
        if (ix.tag != KT_INT) panic("list indices must be integers");
        KamiList* l = (KamiList*)o.p;
        l->items[norm_index(ix.i, l->len, "list")] = v;
        return;
    }
    case KT_MAP: map_set((KamiMap*)o.p, &ix, &v); return;
    default:
        panic(std::string("'") + type_name(o.tag) + "' object does not support item assignment");
    }
}

void kami_del_index(KamiValue* obj, const KamiValue* idx) {
    Lock lk(g_lock);
    KamiValue o = *obj, ix = *idx;
    switch (o.tag) {
    case KT_LIST: {
        if (ix.tag != KT_INT) panic("list indices must be integers");
        KamiList* l = (KamiList*)o.p;
        int64_t j = norm_index(ix.i, l->len, "list");
        for (int64_t i = j; i + 1 < l->len; i++) l->items[i] = l->items[i + 1];
        l->len--;
        return;
    }
    case KT_MAP:
    case KT_SET:
        if (!map_del((KamiMap*)o.p, &ix)) panic("KeyError: " + value_repr(&ix));
        return;
    default:
        panic(std::string("'") + type_name(o.tag) + "' object does not support item deletion");
    }
}

void kami_iter_prep(KamiValue* out, const KamiValue* seq) {
    Lock lk(g_lock);
    KamiValue v = *seq;
    switch (v.tag) {
    case KT_LIST:
        *out = v;
        return;
    case KT_STR: {
        // Materialize into a list of 1-char strings. Callers (for-loops via
        // kami_iter_cond/get, and as_list_pinned in builtins) all treat the
        // result as a list, so a bare KT_STR here would be misread as a
        // KamiList* and crash.
        KamiStr* s = (KamiStr*)v.p;
        KamiList* r = list_new(s->len > 0 ? s->len : 1);
        out->tag = KT_LIST;
        out->p = r;
        for (int64_t i = 0; i < s->len; i++) {
            KamiValue ch{KT_STR, {0}};
            ch.p = str_new(s->data + i, 1);
            r->items[r->len++] = ch;
        }
        return;
    }
    case KT_MAP:
    case KT_SET: {
        KamiMap* m = (KamiMap*)v.p;
        KamiList* r = list_new(m->count);
        out->tag = KT_LIST;
        out->p = r;
        for (int64_t i = 0; i < m->nentries; i++)
            if (m->entries[i].used) r->items[r->len++] = m->entries[i].key;
        return;
    }
    case KT_FILE: {
        KamiFile* f = (KamiFile*)v.p;
        if (f->closed) panic("I/O operation on closed file");
        KamiList* r = list_new(4);
        out->tag = KT_LIST;
        out->p = r;
        std::string line;
        int c;
        FILE* fp = (FILE*)f->fp;
        for (;;) {
            c = fgetc(fp);
            if (c == EOF) {
                if (!line.empty()) {
                    KamiValue s2{KT_STR, {0}};
                    s2.p = str_new(line.data(), (int64_t)line.size());
                    list_push(r, &s2);
                }
                break;
            }
            line += (char)c;
            if (c == '\n') {
                KamiValue s2{KT_STR, {0}};
                s2.p = str_new(line.data(), (int64_t)line.size());
                list_push(r, &s2);
                line.clear();
            }
        }
        return;
    }
    default:
        panic(std::string("'") + type_name(v.tag) + "' object is not iterable");
    }
}

int32_t kami_range_cond(const KamiValue* i, const KamiValue* stop, const KamiValue* step) {
    Lock lk(g_lock);
    if (i->tag != KT_INT || stop->tag != KT_INT || step->tag != KT_INT)
        panic("range() arguments must be integers");
    if (step->i == 0) panic("range() step must not be zero");
    return step->i > 0 ? (i->i < stop->i) : (i->i > stop->i);
}

int32_t kami_iter_cond(const KamiValue* seq, const KamiValue* idx) {
    Lock lk(g_lock);
    int64_t len;
    switch (seq->tag) {
    case KT_LIST: len = ((KamiList*)seq->p)->len; break;
    case KT_STR: len = ((KamiStr*)seq->p)->len; break;
    default:
        panic(std::string("'") + type_name(seq->tag) + "' object is not iterable");
    }
    return idx->i < len;
}

void kami_iter_get(KamiValue* out, const KamiValue* seq, const KamiValue* idx) {
    kami_index_get(out, seq, idx);
}

void kami_call_value(KamiValue* out, const KamiValue* fn, KamiValue** argv, int64_t nargs) {
    KamiFn f = nullptr;
    KamiFn init = nullptr;
    KamiValue* f_caps = nullptr;
    KamiValue* init_caps = nullptr;
    int64_t builtin_id = -1;
    {
        Lock lk(g_lock);
        if (fn->tag == KT_CLASS) {
            KamiClassObj* c = (KamiClassObj*)fn->p;
            KamiInstance* inst = (KamiInstance*)gc_alloc(sizeof(KamiInstance), KT_OBJECT);
            inst->cls = c;
            inst->fields = new std::unordered_map<std::string, KamiValue>();
            out->tag = KT_OBJECT;
            out->p = inst; // rooted caller slot
            if (KamiValue* m = class_lookup(c, "__init__")) {
                KamiFuncObj* fo = (KamiFuncObj*)m->p;
                if (nargs + 1 < fo->min_arity || nargs + 1 > fo->arity)
                    panic(std::string(c->name) + "() takes " + std::to_string(fo->arity - 1) +
                          " argument(s) but " + std::to_string(nargs) + " were given");
                init = (KamiFn)fo->fn;
                init_caps = fo->captures;
            } else if (nargs != 0) {
                panic(std::string(c->name) + "() takes no arguments");
            }
        } else {
            if (fn->tag != KT_FUNC)
                panic(std::string("'") + type_name(fn->tag) + "' object is not callable");
            KamiFuncObj* fo = (KamiFuncObj*)fn->p;
            if (fo->builtin_id >= 0) {
                builtin_id = fo->builtin_id;
            } else {
                if (nargs < fo->min_arity || nargs > fo->arity)
                    panic(std::string(fo->name) + "() takes " + std::to_string(fo->arity) +
                          " argument(s) but " + std::to_string(nargs) + " were given");
                f = (KamiFn)fo->fn;
                f_caps = fo->captures;
            }
        }
    }
    // Invoke WITHOUT holding the lock: callees take the lock themselves.
    if (builtin_id >= 0) {
        kami_builtin(builtin_id, out, argv, nargs);
        return;
    }
    if (init) {
        KamiValue* argv2[17];
        argv2[0] = out; // self
        for (int64_t i = 0; i < nargs; i++) argv2[i + 1] = argv[i];
        KamiValue dummy;
        init(&dummy, argv2, nargs + 1, init_caps);
        return;
    }
    if (f) f(out, argv, nargs, f_caps);
}

void kami_method(KamiValue* out, KamiValue* obj, const char* name, KamiValue** argv,
                 int64_t nargs) {
    KamiFn user_fn = nullptr;
    KamiValue* user_caps = nullptr;
    int64_t user_nargs = 0;
    KamiValue* argv2[17];
    {
        std::unique_lock<std::recursive_mutex> lk(g_lock);
        KamiValue o = *obj;
        std::string m = name;
        if (o.tag == KT_LIST) {
            KamiList* l = (KamiList*)o.p;
            if (m == "append" && nargs == 1) {
                list_push(l, argv[0]);
                out->tag = KT_NONE; out->i = 0;
                return;
            }
            if (m == "pop" && nargs == 0) {
                if (l->len == 0) panic("pop from empty list");
                *out = l->items[--l->len];
                return;
            }
            if (m == "pop" && nargs == 1) {
                if (argv[0]->tag != KT_INT) panic("list.pop(): index must be an int");
                int64_t j = norm_index(argv[0]->i, l->len, "list");
                *out = l->items[j];
                for (int64_t i = j; i + 1 < l->len; i++) l->items[i] = l->items[i + 1];
                l->len--;
                return;
            }
            if (m == "insert" && nargs == 2) {
                if (argv[0]->tag != KT_INT) panic("list.insert(): index must be an int");
                int64_t j = argv[0]->i;
                if (j < 0) j += l->len;
                if (j < 0) j = 0;
                if (j > l->len) j = l->len;
                KamiValue dummy{KT_NONE, {0}};
                list_push(l, &dummy); // grow by one
                for (int64_t i = l->len - 1; i > j; i--) l->items[i] = l->items[i - 1];
                l->items[j] = *argv[1];
                out->tag = KT_NONE; out->i = 0;
                return;
            }
            if (m == "remove" && nargs == 1) {
                for (int64_t i = 0; i < l->len; i++) {
                    if (value_eq(&l->items[i], argv[0])) {
                        for (int64_t k = i; k + 1 < l->len; k++) l->items[k] = l->items[k + 1];
                        l->len--;
                        out->tag = KT_NONE; out->i = 0;
                        return;
                    }
                }
                panic("list.remove(x): x not in list");
            }
            if (m == "index" && nargs == 1) {
                for (int64_t i = 0; i < l->len; i++) {
                    if (value_eq(&l->items[i], argv[0])) {
                        out->tag = KT_INT; out->i = i;
                        return;
                    }
                }
                panic("ValueError: value not in list");
            }
            if (m == "count" && nargs == 1) {
                int64_t n = 0;
                for (int64_t i = 0; i < l->len; i++)
                    if (value_eq(&l->items[i], argv[0])) n++;
                out->tag = KT_INT; out->i = n;
                return;
            }
            if (m == "extend" && nargs == 1) {
                if (argv[0]->tag != KT_LIST) panic("list.extend() expects a list");
                KamiList* src = (KamiList*)argv[0]->p;
                int64_t n = src->len; // snapshot (self-extend safe)
                for (int64_t i = 0; i < n; i++) list_push(l, &src->items[i]);
                out->tag = KT_NONE; out->i = 0;
                return;
            }
            if (m == "reverse" && nargs == 0) {
                for (int64_t i = 0, j = l->len - 1; i < j; i++, j--) {
                    KamiValue t = l->items[i];
                    l->items[i] = l->items[j];
                    l->items[j] = t;
                }
                out->tag = KT_NONE; out->i = 0;
                return;
            }
            if (m == "copy" && nargs == 0) {
                KamiList* r = list_new(l->len);
                out->tag = KT_LIST;
                out->p = r;
                for (int64_t i = 0; i < l->len; i++) r->items[r->len++] = l->items[i];
                return;
            }
            if (m == "sort" && nargs == 0) {
                std::sort(l->items, l->items + l->len,
                          [](const KamiValue& a, const KamiValue& b) {
                              return compare(&a, &b, "sort") < 0;
                          });
                out->tag = KT_NONE; out->i = 0;
                return;
            }
            if (m == "clear" && nargs == 0) {
                l->len = 0;
                out->tag = KT_NONE; out->i = 0;
                return;
            }
        } else if (o.tag == KT_STR) {
            KamiStr* s = (KamiStr*)o.p;
            std::string sv(s->data, (size_t)s->len);
            if ((m == "upper" || m == "lower") && nargs == 0) {
                for (char& c : sv)
                    c = m == "upper" ? (char)toupper((unsigned char)c)
                                     : (char)tolower((unsigned char)c);
                put_str(out, sv.data(), (int64_t)sv.size());
                return;
            }
            if ((m == "strip" || m == "lstrip" || m == "rstrip") && nargs == 0) {
                size_t b = m == "rstrip" ? 0 : sv.find_first_not_of(" \t\r\n");
                size_t e = m == "lstrip" ? (sv.empty() ? std::string::npos : sv.size() - 1)
                                         : sv.find_last_not_of(" \t\r\n");
                std::string r = b == std::string::npos || e == std::string::npos
                                    ? ""
                                    : sv.substr(b, e - b + 1);
                put_str(out, r.data(), (int64_t)r.size());
                return;
            }
            if (m == "capitalize" && nargs == 0) {
                std::string r = sv;
                for (size_t k = 0; k < r.size(); k++)
                    r[k] = k == 0 ? (char)toupper((unsigned char)r[k])
                                  : (char)tolower((unsigned char)r[k]);
                put_str(out, r.data(), (int64_t)r.size());
                return;
            }
            if (m == "title" && nargs == 0) {
                std::string r = sv;
                bool start = true;
                for (char& ch : r) {
                    if (isalpha((unsigned char)ch)) {
                        ch = start ? (char)toupper((unsigned char)ch)
                                   : (char)tolower((unsigned char)ch);
                        start = false;
                    } else {
                        start = true;
                    }
                }
                put_str(out, r.data(), (int64_t)r.size());
                return;
            }
            if ((m == "isupper" || m == "islower" || m == "isspace") && nargs == 0) {
                bool any_alpha = false, ok = !sv.empty();
                for (unsigned char ch : sv) {
                    if (m == "isspace") {
                        if (!isspace(ch)) { ok = false; break; }
                    } else if (isalpha(ch)) {
                        any_alpha = true;
                        if (m == "isupper" ? islower(ch) : isupper(ch)) { ok = false; break; }
                    }
                }
                if (m != "isspace") ok = ok && any_alpha;
                out->tag = KT_BOOL;
                out->i = ok;
                return;
            }
            if (m == "zfill" && nargs == 1) {
                if (argv[0]->tag != KT_INT) panic("str.zfill() expects an int");
                std::string r = sv;
                int64_t w = argv[0]->i;
                std::string sign;
                if (!r.empty() && (r[0] == '-' || r[0] == '+')) {
                    sign = r.substr(0, 1);
                    r = r.substr(1);
                }
                while ((int64_t)(sign.size() + r.size()) < w) r = "0" + r;
                r = sign + r;
                put_str(out, r.data(), (int64_t)r.size());
                return;
            }
            if (m == "split" && nargs <= 1) {
                KamiList* r = list_new(4);
                out->tag = KT_LIST;
                out->p = r; // root before pushes
                KamiValue tmp;
                if (nargs == 0) { // split on runs of whitespace
                    size_t i = 0;
                    while (i < sv.size()) {
                        while (i < sv.size() && isspace((unsigned char)sv[i])) i++;
                        size_t b = i;
                        while (i < sv.size() && !isspace((unsigned char)sv[i])) i++;
                        if (i > b) {
                            put_str(&tmp, sv.data() + b, (int64_t)(i - b));
                            list_push(r, &tmp);
                        }
                    }
                } else {
                    if (argv[0]->tag != KT_STR) panic("str.split(): separator must be a str");
                    KamiStr* sep = (KamiStr*)argv[0]->p;
                    std::string ss(sep->data, (size_t)sep->len);
                    if (ss.empty()) panic("str.split(): empty separator");
                    size_t pos = 0;
                    for (;;) {
                        size_t f = sv.find(ss, pos);
                        std::string piece =
                            f == std::string::npos ? sv.substr(pos) : sv.substr(pos, f - pos);
                        put_str(&tmp, piece.data(), (int64_t)piece.size());
                        list_push(r, &tmp);
                        if (f == std::string::npos) break;
                        pos = f + ss.size();
                    }
                }
                return;
            }
            if (m == "join" && nargs == 1) {
                if (argv[0]->tag != KT_LIST) panic("str.join() expects a list");
                KamiList* l = (KamiList*)argv[0]->p;
                std::string r;
                for (int64_t i = 0; i < l->len; i++) {
                    if (l->items[i].tag != KT_STR) panic("str.join(): list items must be str");
                    if (i) r += sv;
                    KamiStr* it = (KamiStr*)l->items[i].p;
                    r.append(it->data, (size_t)it->len);
                }
                put_str(out, r.data(), (int64_t)r.size());
                return;
            }
            if (m == "replace" && nargs == 2) {
                if (argv[0]->tag != KT_STR || argv[1]->tag != KT_STR)
                    panic("str.replace() expects two strings");
                KamiStr* a = (KamiStr*)argv[0]->p;
                KamiStr* b = (KamiStr*)argv[1]->p;
                std::string sa(a->data, (size_t)a->len), sb(b->data, (size_t)b->len);
                if (sa.empty()) panic("str.replace(): empty search string");
                std::string r;
                size_t pos = 0;
                for (;;) {
                    size_t f = sv.find(sa, pos);
                    if (f == std::string::npos) {
                        r += sv.substr(pos);
                        break;
                    }
                    r += sv.substr(pos, f - pos);
                    r += sb;
                    pos = f + sa.size();
                }
                put_str(out, r.data(), (int64_t)r.size());
                return;
            }
            if ((m == "startswith" || m == "endswith") && nargs == 1) {
                if (argv[0]->tag != KT_STR) panic(("str." + m + "() expects a str").c_str());
                KamiStr* p = (KamiStr*)argv[0]->p;
                std::string sp(p->data, (size_t)p->len);
                bool r;
                if (m == "startswith")
                    r = sv.size() >= sp.size() && sv.compare(0, sp.size(), sp) == 0;
                else
                    r = sv.size() >= sp.size() &&
                        sv.compare(sv.size() - sp.size(), sp.size(), sp) == 0;
                out->tag = KT_BOOL; out->i = r;
                return;
            }
            if (m == "find" && nargs == 1) {
                if (argv[0]->tag != KT_STR) panic("str.find() expects a str");
                KamiStr* p = (KamiStr*)argv[0]->p;
                size_t f = sv.find(std::string(p->data, (size_t)p->len));
                out->tag = KT_INT;
                out->i = f == std::string::npos ? -1 : (int64_t)f;
                return;
            }
            if (m == "count" && nargs == 1) {
                if (argv[0]->tag != KT_STR) panic("str.count() expects a str");
                KamiStr* p = (KamiStr*)argv[0]->p;
                std::string sp(p->data, (size_t)p->len);
                int64_t n = 0;
                if (!sp.empty()) {
                    size_t pos = 0;
                    while ((pos = sv.find(sp, pos)) != std::string::npos) {
                        n++;
                        pos += sp.size();
                    }
                }
                out->tag = KT_INT; out->i = n;
                return;
            }
            if ((m == "isdigit" || m == "isalpha") && nargs == 0) {
                bool r = !sv.empty();
                for (unsigned char c : sv) {
                    if (m == "isdigit" ? !isdigit(c) : !isalpha(c)) {
                        r = false;
                        break;
                    }
                }
                out->tag = KT_BOOL; out->i = r;
                return;
            }
            if ((m == "isalnum" || m == "isnumeric" || m == "isdecimal") && nargs == 0) {
                bool r = !sv.empty();
                for (unsigned char c : sv) {
                    bool okc = m == "isalnum" ? (isalnum(c) != 0) : (isdigit(c) != 0);
                    if (!okc) { r = false; break; }
                }
                out->tag = KT_BOOL; out->i = r;
                return;
            }
            if (m == "casefold" && nargs == 0) { // like lower() for ASCII
                std::string r = sv;
                for (char& c : r) c = (char)tolower((unsigned char)c);
                put_str(out, r.data(), (int64_t)r.size());
                return;
            }
            if (m == "swapcase" && nargs == 0) {
                std::string r = sv;
                for (char& c : r) {
                    if (islower((unsigned char)c)) c = (char)toupper((unsigned char)c);
                    else if (isupper((unsigned char)c)) c = (char)tolower((unsigned char)c);
                }
                put_str(out, r.data(), (int64_t)r.size());
                return;
            }
            if (m == "index" && nargs == 1) { // like find() but raises if absent
                if (argv[0]->tag != KT_STR) panic("str.index() expects a str");
                KamiStr* p = (KamiStr*)argv[0]->p;
                size_t f = sv.find(std::string(p->data, (size_t)p->len));
                if (f == std::string::npos) panic("ValueError: substring not found");
                out->tag = KT_INT;
                out->i = (int64_t)f;
                return;
            }
            if (m == "rfind" && nargs == 1) {
                if (argv[0]->tag != KT_STR) panic("str.rfind() expects a str");
                KamiStr* p = (KamiStr*)argv[0]->p;
                size_t f = sv.rfind(std::string(p->data, (size_t)p->len));
                out->tag = KT_INT;
                out->i = f == std::string::npos ? -1 : (int64_t)f;
                return;
            }
            if ((m == "ljust" || m == "rjust" || m == "center") && nargs >= 1 && nargs <= 2) {
                if (argv[0]->tag != KT_INT) panic("str." + m + "() width must be an int");
                int64_t w = argv[0]->i;
                char fill = ' ';
                if (nargs == 2 && argv[1]->tag == KT_STR) {
                    KamiStr* fs = (KamiStr*)argv[1]->p;
                    if (fs->len >= 1) fill = fs->data[0];
                }
                std::string r = sv;
                int64_t pad = w - (int64_t)sv.size();
                if (pad > 0) {
                    if (m == "ljust") r = sv + std::string((size_t)pad, fill);
                    else if (m == "rjust") r = std::string((size_t)pad, fill) + sv;
                    else {
                        int64_t left = pad / 2, right = pad - left;
                        r = std::string((size_t)left, fill) + sv + std::string((size_t)right, fill);
                    }
                }
                put_str(out, r.data(), (int64_t)r.size());
                return;
            }
            if ((m == "removeprefix" || m == "removesuffix") && nargs == 1) {
                if (argv[0]->tag != KT_STR) panic("str." + m + "() expects a str");
                KamiStr* p = (KamiStr*)argv[0]->p;
                std::string pre(p->data, (size_t)p->len);
                std::string r = sv;
                if (m == "removeprefix") {
                    if (sv.size() >= pre.size() && sv.compare(0, pre.size(), pre) == 0)
                        r = sv.substr(pre.size());
                } else {
                    if (sv.size() >= pre.size() &&
                        sv.compare(sv.size() - pre.size(), pre.size(), pre) == 0)
                        r = sv.substr(0, sv.size() - pre.size());
                }
                put_str(out, r.data(), (int64_t)r.size());
                return;
            }
            if (m == "format") {
                // Supports {}, {0}, {1}, and format specs like {:.2f}, {:5},
                // {0:>8}. Keyword fields ({name}) are not available because
                // method calls don't carry kwargs in this runtime.
                std::string r;
                int64_t auto_idx = 0;
                for (size_t i = 0; i < sv.size(); i++) {
                    if (sv[i] == '{' && i + 1 < sv.size() && sv[i + 1] == '{') { r += '{'; i++; continue; }
                    if (sv[i] == '}' && i + 1 < sv.size() && sv[i + 1] == '}') { r += '}'; i++; continue; }
                    if (sv[i] != '{') { r += sv[i]; continue; }
                    size_t close = sv.find('}', i);
                    if (close == std::string::npos) panic("str.format(): single '{' encountered");
                    std::string field = sv.substr(i + 1, close - i - 1);
                    i = close;
                    std::string spec;
                    size_t colon = field.find(':');
                    if (colon != std::string::npos) {
                        spec = field.substr(colon + 1);
                        field = field.substr(0, colon);
                    }
                    int64_t idx;
                    if (field.empty()) idx = auto_idx++;
                    else idx = (int64_t)strtoll(field.c_str(), nullptr, 10);
                    if (idx < 0 || idx >= nargs)
                        panic("str.format(): index " + std::to_string(idx) + " out of range");
                    r += format_value(argv[idx], spec);
                }
                put_str(out, r.data(), (int64_t)r.size());
                return;
            }
            if (m == "encode" && nargs <= 1) { // no real bytes type: identity str
                put_str(out, sv.data(), (int64_t)sv.size());
                return;
            }
        } else if (o.tag == KT_MAP) {
            KamiMap* mp = (KamiMap*)o.p;
            if (m == "keys" && nargs == 0) {
                KamiList* r = list_new(mp->count);
                for (int64_t i = 0; i < mp->nentries; i++)
                    if (mp->entries[i].used) r->items[r->len++] = mp->entries[i].key;
                out->tag = KT_LIST; out->p = r;
                return;
            }
            if (m == "values" && nargs == 0) {
                KamiList* r = list_new(mp->count);
                for (int64_t i = 0; i < mp->nentries; i++)
                    if (mp->entries[i].used) r->items[r->len++] = mp->entries[i].val;
                out->tag = KT_LIST; out->p = r;
                return;
            }
            if (m == "items" && nargs == 0) {
                KamiList* r = list_new(mp->count);
                out->tag = KT_LIST;
                out->p = r; // root before nested allocations
                for (int64_t i = 0; i < mp->nentries; i++) {
                    if (!mp->entries[i].used) continue;
                    KamiList* pair = list_new(2);
                    pair->items[pair->len++] = mp->entries[i].key;
                    pair->items[pair->len++] = mp->entries[i].val;
                    KamiValue pv;
                    pv.tag = KT_LIST;
                    pv.p = pair;
                    list_push(r, &pv);
                }
                return;
            }
            if (m == "get" && (nargs == 1 || nargs == 2)) {
                if (!map_get(mp, argv[0], out)) {
                    if (nargs == 2) *out = *argv[1];
                    else { out->tag = KT_NONE; out->i = 0; }
                }
                return;
            }
        } else if (o.tag == KT_SET) {
            KamiMap* mp = (KamiMap*)o.p;
            if (m == "add" && nargs == 1) {
                KamiValue none{KT_NONE, {0}};
                map_set(mp, argv[0], &none);
                out->tag = KT_NONE; out->i = 0;
                return;
            }
            if ((m == "remove" || m == "discard") && nargs == 1) {
                bool found = map_del(mp, argv[0]);
                if (!found && m == "remove") panic("KeyError: " + value_repr(argv[0]));
                out->tag = KT_NONE; out->i = 0;
                return;
            }
            if (m == "clear" && nargs == 0) {
                free(mp->entries);
                free(mp->index);
                mp->entries = nullptr;
                mp->index = nullptr;
                mp->ecap = 0;
                mp->nentries = 0;
                mp->icap = 0;
                mp->count = 0;
                out->tag = KT_NONE; out->i = 0;
                return;
            }
            if (m == "copy" && nargs == 0) {
                kami_make_set(out);
                KamiMap* r = (KamiMap*)out->p;
                KamiValue none{KT_NONE, {0}};
                for (int64_t i = 0; i < mp->nentries; i++)
                    if (mp->entries[i].used) map_set(r, &mp->entries[i].key, &none);
                return;
            }
            if (m == "pop" && nargs == 0) {
                for (int64_t i = 0; i < mp->nentries; i++) {
                    if (mp->entries[i].used) {
                        *out = mp->entries[i].key;
                        map_del(mp, &mp->entries[i].key);
                        return;
                    }
                }
                panic("KeyError: 'pop from an empty set'");
            }
            if ((m == "union" || m == "intersection" || m == "difference" ||
                 m == "symmetric_difference") && nargs == 1) {
                if (argv[0]->tag != KT_SET) panic("set." + m + "() expects a set");
                KamiMap* other = (KamiMap*)argv[0]->p;
                kami_make_set(out);
                KamiMap* r = (KamiMap*)out->p;
                KamiValue none{KT_NONE, {0}};
                KamiValue tmp;
                if (m == "union") {
                    for (int64_t i = 0; i < mp->nentries; i++)
                        if (mp->entries[i].used) map_set(r, &mp->entries[i].key, &none);
                    for (int64_t i = 0; i < other->nentries; i++)
                        if (other->entries[i].used) map_set(r, &other->entries[i].key, &none);
                } else if (m == "intersection") {
                    for (int64_t i = 0; i < mp->nentries; i++)
                        if (mp->entries[i].used && map_get(other, &mp->entries[i].key, &tmp))
                            map_set(r, &mp->entries[i].key, &none);
                } else if (m == "difference") {
                    for (int64_t i = 0; i < mp->nentries; i++)
                        if (mp->entries[i].used && !map_get(other, &mp->entries[i].key, &tmp))
                            map_set(r, &mp->entries[i].key, &none);
                } else { // symmetric_difference
                    for (int64_t i = 0; i < mp->nentries; i++)
                        if (mp->entries[i].used && !map_get(other, &mp->entries[i].key, &tmp))
                            map_set(r, &mp->entries[i].key, &none);
                    for (int64_t i = 0; i < other->nentries; i++)
                        if (other->entries[i].used && !map_get(mp, &other->entries[i].key, &tmp))
                            map_set(r, &other->entries[i].key, &none);
                }
                return;
            }
            if ((m == "update" || m == "difference_update" ||
                 m == "intersection_update") && nargs == 1) {
                if (argv[0]->tag != KT_SET) panic("set." + m + "() expects a set");
                KamiMap* other = (KamiMap*)argv[0]->p;
                KamiValue none{KT_NONE, {0}};
                KamiValue tmp;
                if (m == "update") {
                    for (int64_t i = 0; i < other->nentries; i++)
                        if (other->entries[i].used) map_set(mp, &other->entries[i].key, &none);
                } else if (m == "difference_update") {
                    for (int64_t i = 0; i < other->nentries; i++)
                        if (other->entries[i].used) map_del(mp, &other->entries[i].key);
                } else { // intersection_update
                    std::vector<KamiValue> drop;
                    for (int64_t i = 0; i < mp->nentries; i++)
                        if (mp->entries[i].used && !map_get(other, &mp->entries[i].key, &tmp))
                            drop.push_back(mp->entries[i].key);
                    for (auto& k : drop) map_del(mp, &k);
                }
                out->tag = KT_NONE; out->i = 0;
                return;
            }
            if (m == "isdisjoint" && nargs == 1) {
                if (argv[0]->tag != KT_SET) panic("set.isdisjoint() expects a set");
                KamiMap* other = (KamiMap*)argv[0]->p;
                KamiValue tmp;
                bool disjoint = true;
                for (int64_t i = 0; i < mp->nentries; i++)
                    if (mp->entries[i].used && map_get(other, &mp->entries[i].key, &tmp)) {
                        disjoint = false;
                        break;
                    }
                out->tag = KT_BOOL; out->i = disjoint;
                return;
            }
            if ((m == "issubset" || m == "issuperset") && nargs == 1) {
                if (argv[0]->tag != KT_SET) panic("set." + m + "() expects a set");
                KamiMap* other = (KamiMap*)argv[0]->p;
                KamiMap* a = m == "issubset" ? mp : other;
                KamiMap* b = m == "issubset" ? other : mp;
                KamiValue tmp;
                bool ok = true;
                for (int64_t i = 0; i < a->nentries; i++)
                    if (a->entries[i].used && !map_get(b, &a->entries[i].key, &tmp)) {
                        ok = false;
                        break;
                    }
                out->tag = KT_BOOL; out->i = ok;
                return;
            }
        } else if (o.tag == KT_FILE) {
            KamiFile* f = (KamiFile*)o.p;
            FILE* fp = (FILE*)f->fp;
            if (m != "close" && f->closed) panic("I/O operation on closed file");
            if (m == "read" && nargs <= 1) {
                std::string data;
                if (nargs == 1) {
                    if (argv[0]->tag != KT_INT) panic("file.read() expects an int");
                    data.resize((size_t)argv[0]->i);
                    size_t got = fread(data.data(), 1, data.size(), fp);
                    data.resize(got);
                } else {
                    char buf[4096];
                    size_t got;
                    while ((got = fread(buf, 1, sizeof buf, fp)) > 0) data.append(buf, got);
                }
                put_str(out, data.data(), (int64_t)data.size());
                return;
            }
            if (m == "readline" && nargs == 0) {
                std::string line;
                int c;
                while ((c = fgetc(fp)) != EOF) {
                    line += (char)c;
                    if (c == '\n') break;
                }
                put_str(out, line.data(), (int64_t)line.size());
                return;
            }
            if (m == "readlines" && nargs == 0) {
                kami_iter_prep(out, &o); // recursive lock is fine
                return;
            }
            if (m == "write" && nargs == 1) {
                if (argv[0]->tag != KT_STR) panic("file.write() expects a str");
                KamiStr* s2 = (KamiStr*)argv[0]->p;
                size_t got = fwrite(s2->data, 1, (size_t)s2->len, fp);
                out->tag = KT_INT;
                out->i = (int64_t)got;
                return;
            }
            if (m == "flush" && nargs == 0) {
                fflush(fp);
                out->tag = KT_NONE; out->i = 0;
                return;
            }
            if (m == "close" && nargs == 0) {
                if (!f->closed) {
                    fclose(fp);
                    f->closed = true;
                }
                out->tag = KT_NONE; out->i = 0;
                return;
            }
        } else if (o.tag == KT_OBJECT) {
            KamiInstance* in = (KamiInstance*)o.p;
            KamiValue* mv = nullptr;
            bool bound = false;
            auto it = in->fields->find(m);
            if (it != in->fields->end()) {
                mv = &it->second;
            } else if ((mv = class_lookup(in->cls, m)) != nullptr) {
                bound = true; // class method: prepend self
            }
            if (!mv)
                panic(std::string("AttributeError: '") + in->cls->name +
                      "' object has no attribute '" + m + "'");
            if (mv->tag != KT_FUNC)
                panic(std::string("'") + in->cls->name + "." + m + "' is not callable");
            KamiFuncObj* fo = (KamiFuncObj*)mv->p;
            int64_t want = bound ? nargs + 1 : nargs;
            if (want < fo->min_arity || want > fo->arity)
                panic(m + "() takes " + std::to_string(fo->arity - (bound ? 1 : 0)) +
                      " argument(s) but " + std::to_string(nargs) + " were given");
            user_fn = (KamiFn)fo->fn;
            user_caps = fo->captures;
            int64_t k = 0;
            if (bound) argv2[k++] = obj;
            for (int64_t i = 0; i < nargs; i++) argv2[k++] = argv[i];
            user_nargs = k;
        } else if (o.tag == KT_CLASS) {
            // unbound call: ClassName.method(self, args...)
            KamiClassObj* c = (KamiClassObj*)o.p;
            KamiValue* mv = class_lookup(c, m);
            if (!mv || mv->tag != KT_FUNC)
                panic(std::string("class '") + c->name + "' has no method '" + m + "'");
            KamiFuncObj* fo = (KamiFuncObj*)mv->p;
            if (nargs < fo->min_arity || nargs > fo->arity)
                panic(m + "() takes " + std::to_string(fo->arity) + " argument(s) but " +
                      std::to_string(nargs) + " were given");
            user_fn = (KamiFn)fo->fn;
            user_caps = fo->captures;
            for (int64_t i = 0; i < nargs; i++) argv2[i] = argv[i];
            user_nargs = nargs;
        }
        if (!user_fn)
            panic(std::string("'") + type_name(o.tag) + "' object has no method '" + m + "'(" +
                  std::to_string(nargs) + " args)");
    }
    // Invoke user method WITHOUT holding the lock.
    user_fn(out, argv2, user_nargs, user_caps);
}

} // extern "C"
