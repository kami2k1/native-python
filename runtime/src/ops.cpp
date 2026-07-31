// Dynamic operators, indexing, iteration and calls.
#include "rt_internal.h"

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
    case KT_MAP: {
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
        num_result_float(out, as_num(a) - as_num(b));
        return;
    case KOP_MUL:
        if (a->tag == KT_INT && b->tag == KT_INT) { num_result_int(out, a->i * b->i); return; }
        if (is_num(a) && is_num(b)) { num_result_float(out, as_num(a) * as_num(b)); return; }
        if (a->tag == KT_STR && b->tag == KT_INT) { // "ab" * 3
            KamiStr* x = (KamiStr*)a->p;
            std::string s;
            for (int64_t i = 0; i < b->i; i++) s.append(x->data, (size_t)x->len);
            out->tag = KT_STR;
            out->p = str_new(s.data(), (int64_t)s.size());
            return;
        }
        panic(std::string("unsupported operand types for *: '") + type_name(a->tag) +
              "' and '" + type_name(b->tag) + "'");
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
    KamiFn f;
    {
        Lock lk(g_lock);
        if (fn->tag != KT_FUNC)
            panic(std::string("'") + type_name(fn->tag) + "' object is not callable");
        KamiFuncObj* fo = (KamiFuncObj*)fn->p;
        if (fo->arity != nargs)
            panic(std::string(fo->name) + "() takes " + std::to_string(fo->arity) +
                  " argument(s) but " + std::to_string(nargs) + " were given");
        f = (KamiFn)fo->fn;
    }
    // Invoke WITHOUT holding the lock: the callee makes its own runtime calls.
    f(out, argv);
}

void kami_method(KamiValue* out, KamiValue* obj, const char* name, KamiValue** argv,
                 int64_t nargs) {
    Lock lk(g_lock);
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
    } else if (o.tag == KT_STR) {
        KamiStr* s = (KamiStr*)o.p;
        if ((m == "upper" || m == "lower") && nargs == 0) {
            std::string r(s->data, (size_t)s->len);
            for (char& c : r)
                c = m == "upper" ? (char)toupper((unsigned char)c) : (char)tolower((unsigned char)c);
            out->tag = KT_STR;
            out->p = str_new(r.data(), (int64_t)r.size());
            return;
        }
    } else if (o.tag == KT_MAP) {
        KamiMap* mp = (KamiMap*)o.p;
        if (m == "keys" && nargs == 0) {
            KamiList* r = list_new(mp->count);
            for (int64_t i = 0; i < mp->cap; i++)
                if (mp->entries[i].used) r->items[r->len++] = mp->entries[i].key;
            out->tag = KT_LIST; out->p = r;
            return;
        }
        if (m == "values" && nargs == 0) {
            KamiList* r = list_new(mp->count);
            for (int64_t i = 0; i < mp->cap; i++)
                if (mp->entries[i].used) r->items[r->len++] = mp->entries[i].val;
            out->tag = KT_LIST; out->p = r;
            return;
        }
        if (m == "get" && (nargs == 1 || nargs == 2)) {
            if (!map_get(mp, argv[0], out)) {
                if (nargs == 2) *out = *argv[1];
                else { out->tag = KT_NONE; out->i = 0; }
            }
            return;
        }
    }
    panic(std::string("'") + type_name(o.tag) + "' object has no method '" + m + "'(" +
          std::to_string(nargs) + " args)");
}

} // extern "C"
