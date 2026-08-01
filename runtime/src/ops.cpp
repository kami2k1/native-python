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

// ---------------- %-style string formatting ----------------
std::string percent_format(const std::string& fmt, const KamiValue* args, int64_t nargs) {
    std::string out;
    int64_t ai = 0;
    // "%(name)s" % {...}: a single mapping argument is looked up by key.
    const KamiMap* mapping =
        (nargs == 1 && args[0].tag == KT_MAP) ? (const KamiMap*)args[0].p : nullptr;
    std::string cur_key;
    auto next = [&]() -> const KamiValue* {
        if (!cur_key.empty()) {
            if (!mapping) panic("format requires a mapping");
            for (int64_t k = 0; k < mapping->nentries; k++) {
                if (!mapping->entries[k].used) continue;
                const KamiValue& kk = mapping->entries[k].key;
                if (kk.tag != KT_STR) continue;
                KamiStr* ks = (KamiStr*)kk.p;
                if ((size_t)ks->len == cur_key.size() &&
                    memcmp(ks->data, cur_key.data(), cur_key.size()) == 0)
                    return &mapping->entries[k].val;
            }
            panic("KeyError: '" + cur_key + "'");
        }
        if (ai >= nargs) panic("not enough arguments for format string");
        return &args[ai++];
    };
    for (size_t i = 0; i < fmt.size(); i++) {
        char c = fmt[i];
        if (c != '%') {
            out += c;
            continue;
        }
        if (i + 1 >= fmt.size()) panic("incomplete format");
        // parse %[(key)][flags][width][.prec]type
        std::string key;
        size_t j = i + 1;
        if (j < fmt.size() && fmt[j] == '(') {
            size_t close = fmt.find(')', j);
            if (close == std::string::npos) panic("incomplete format key");
            key = fmt.substr(j + 1, close - j - 1);
            j = close + 1;
        }
        std::string spec;
        while (j < fmt.size() && (isdigit((unsigned char)fmt[j]) || fmt[j] == '-' ||
                                  fmt[j] == '+' || fmt[j] == '.' || fmt[j] == ' ' ||
                                  fmt[j] == '0'))
            spec += fmt[j++];
        if (j >= fmt.size()) panic("incomplete format");
        char t = fmt[j];
        i = j;
        cur_key = key;
        char buf[128];
        switch (t) {
        case '%': out += '%'; break;
        case 's':
        case 'r': {
            const KamiValue* v = next();
            std::string s2 = t == 's' ? value_str(v) : value_repr(v);
            if (!spec.empty()) {
                std::string f2 = "%" + spec + "s";
                std::vector<char> big(s2.size() + 64);
                snprintf(big.data(), big.size(), f2.c_str(), s2.c_str());
                out += big.data();
            } else {
                out += s2;
            }
            break;
        }
        case 'd':
        case 'i': {
            const KamiValue* v = next();
            int64_t x;
            if (v->tag == KT_INT || v->tag == KT_BOOL) x = v->i;
            else if (v->tag == KT_FLOAT) x = (int64_t)v->f;
            else panic("%d format: a number is required");
            std::string f2 = "%" + spec + "lld";
            snprintf(buf, sizeof buf, f2.c_str(), (long long)x);
            out += buf;
            break;
        }
        case 'x':
        case 'X':
        case 'o': {
            const KamiValue* v = next();
            if (v->tag != KT_INT && v->tag != KT_BOOL) panic("%x format: an int is required");
            std::string f2 = "%" + spec + "ll";
            f2 += t;
            snprintf(buf, sizeof buf, f2.c_str(), (long long)v->i);
            out += buf;
            break;
        }
        case 'f':
        case 'F':
        case 'e':
        case 'E':
        case 'g':
        case 'G': {
            const KamiValue* v = next();
            double d;
            if (v->tag == KT_FLOAT) d = v->f;
            else if (v->tag == KT_INT || v->tag == KT_BOOL) d = (double)v->i;
            else panic("%f format: a number is required");
            std::string f2 = "%" + spec;
            f2 += t;
            snprintf(buf, sizeof buf, f2.c_str(), d);
            out += buf;
            break;
        }
        default:
            panic(std::string("unsupported format character '%") + t + "'");
        }
    }
    return out;
}

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
                out->tag = KT_STR;
                out->p = str_new(s.data(), (int64_t)s.size());
            } else {
                KamiList* x = (KamiList*)seq->p;
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
        if (a->tag == KT_STR) {
            // printf-style formatting: "%s=%d" % (name, n)
            KamiStr* f = (KamiStr*)a->p;
            std::string fmt(f->data, (size_t)f->len);
            std::string res;
            if (b->tag == KT_LIST) {
                KamiList* l = (KamiList*)b->p;
                res = percent_format(fmt, l->items, l->len);
            } else {
                res = percent_format(fmt, b, 1);
            }
            out->tag = KT_STR;
            out->p = str_new(res.data(), (int64_t)res.size());
            return;
        }
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

namespace kami {

// Rearranges a dynamic call's arguments into the layout the callee's prologue
// expects. Fast path: plain function, plain call. Slow path: pack extra
// positionals into a *args tuple / keywords into a **kwargs dict (allocated
// into `packed`, which the caller must keep pinned across the invocation).
int64_t prep_user_argv(KamiFuncObj* fo, KamiValue** argv, int64_t nargs, KamiMap* kwmap,
                       KamiValue** argv2, KamiValue* packed) {
    const bool hv = (fo->flags & KFN_VARARG) != 0, hk = (fo->flags & KFN_KWARG) != 0;
    const int64_t P = fo->arity;
    const std::string what = fo->name ? fo->name : "<function>";
    if (!hv && !hk && (!kwmap || kwmap->count == 0)) {
        if (nargs < fo->min_arity || nargs > P)
            panic(what + "() takes " + std::to_string(P) + " argument(s) but " +
                  std::to_string(nargs) + " were given");
        for (int64_t i = 0; i < nargs; i++) argv2[i] = argv[i];
        return nargs;
    }
    // fixed-parameter table
    std::vector<KamiValue*> fixed((size_t)P, nullptr);
    int64_t kwonly = fo->kwonly > P ? P : fo->kwonly;
    int64_t npos = nargs < kwonly ? nargs : kwonly;
    if (nargs > kwonly && !hv)
        panic(what + "() takes " + std::to_string(kwonly) + " positional argument(s) but " +
              std::to_string(nargs) + " were given");
    for (int64_t i = 0; i < npos; i++) fixed[(size_t)i] = argv[i];
    // keyword matching against the recorded parameter names
    std::vector<std::string> names;
    if (kwmap && kwmap->count > 0) {
        std::string all = fo->param_names ? fo->param_names : "";
        size_t start = 0;
        while (start <= all.size() && !all.empty()) {
            size_t c = all.find(',', start);
            names.push_back(all.substr(start, c == std::string::npos ? c : c - start));
            if (c == std::string::npos) break;
            start = c + 1;
        }
    }
    KamiMap* kwleft = nullptr;
    if (kwmap && kwmap->count > 0) {
        for (int64_t ei = 0; ei < kwmap->nentries; ei++) {
            MapEntry& en = kwmap->entries[ei];
            if (!en.used) continue;
            if (en.key.tag != KT_STR) panic(what + "(): keyword names must be strings");
            std::string kw(((KamiStr*)en.key.p)->data, (size_t)((KamiStr*)en.key.p)->len);
            int64_t found = -1;
            for (size_t i = 0; i < names.size(); i++)
                if (names[i] == kw) { found = (int64_t)i; break; }
            if (found >= 0) {
                if (fixed[(size_t)found])
                    panic(what + "() got multiple values for argument '" + kw + "'");
                fixed[(size_t)found] = &en.val;
            } else if (hk) {
                if (!kwleft) {
                    kwleft = map_new();
                    packed[1].tag = KT_MAP;
                    packed[1].p = kwleft; // pinned by the caller
                }
                map_set(kwleft, &en.key, &en.val);
            } else {
                panic(what + "() got an unexpected keyword argument '" + kw + "'");
            }
        }
    }
    int64_t nextra = nargs - npos; // extra positionals → *args tuple
    bool pass_tuple = false, pass_dict = kwleft && kwleft->count > 0;
    if (hv && (nextra > 0 || pass_dict)) pass_tuple = true;
    // trailing holes may stay unfilled only when nothing packed follows them
    int64_t pass_n = P;
    if (!pass_tuple && !pass_dict) {
        while (pass_n > 0 && !fixed[(size_t)pass_n - 1]) pass_n--;
    }
    for (int64_t i = 0; i < pass_n; i++)
        if (!fixed[(size_t)i])
            panic(what + "(): argument " + std::to_string(i + 1) +
                  " is missing (defaults before supplied arguments cannot be filled in "
                  "dynamic calls)");
    if (pass_n < fo->min_arity)
        panic(what + "() takes at least " + std::to_string(fo->min_arity) +
              " argument(s) but " + std::to_string(pass_n) + " were given");
    for (int64_t i = 0; i < pass_n; i++) argv2[i] = fixed[(size_t)i];
    int64_t n2 = pass_n;
    if (pass_tuple) {
        KamiList* l = list_new(nextra > 0 ? nextra : 1);
        packed[0].tag = KT_LIST;
        packed[0].p = l; // pinned by the caller
        for (int64_t i = 0; i < nextra; i++) list_push(l, argv[npos + i]);
        argv2[n2++] = &packed[0];
    }
    if (pass_dict) argv2[n2++] = &packed[1];
    return n2;
}


} // namespace kami

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
    int64_t n2 = 0;
    KamiValue* argv2[19];
    KamiValue packed[2] = {{KT_NONE, {0}}, {KT_NONE, {0}}};
    PinGuard pin(packed, 2);
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
                KamiValue* argv1[19];
                argv1[0] = out; // self
                for (int64_t i = 0; i < nargs && i < 18; i++) argv1[i + 1] = argv[i];
                n2 = prep_user_argv(fo, argv1, nargs + 1, nullptr, argv2, packed);
                init = (KamiFn)fo->fn;
                init_caps = fo->captures;
            } else if (nargs != 0) {
                panic(std::string(c->name) + "() takes no arguments");
            }
        } else if (fn->tag == KT_PYOBJ) {
            pyobj_call(out, fn, argv, nargs);
            return;
        } else {
            if (fn->tag != KT_FUNC)
                panic(std::string("'") + type_name(fn->tag) + "' object is not callable");
            KamiFuncObj* fo = (KamiFuncObj*)fn->p;
            if (fo->builtin_id >= 0) {
                builtin_id = fo->builtin_id;
            } else {
                n2 = prep_user_argv(fo, argv, nargs, nullptr, argv2, packed);
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
        KamiValue dummy;
        init(&dummy, argv2, n2, init_caps);
        return;
    }
    if (f) f(out, argv2, n2, f_caps);
}

void kami_call_star(KamiValue* out, const KamiValue* fn, const KamiValue* pos,
                    const KamiValue* kw) {
    if (pos->tag != KT_LIST) panic("argument after * must be an iterable");
    if (kw->tag != KT_MAP) panic("argument after ** must be a mapping");
    // Copy the positional values into pinned storage: the source list is a
    // caller-visible object that user code (another thread) could mutate.
    std::vector<KamiValue> vals;
    {
        Lock lk(g_lock);
        KamiList* l = (KamiList*)pos->p;
        vals.assign(l->items, l->items + l->len);
    }
    PinGuard pinv(vals.data(), (int64_t)vals.size());
    std::vector<KamiValue*> argv(vals.size());
    for (size_t i = 0; i < vals.size(); i++) argv[i] = &vals[i];
    int64_t nargs = (int64_t)vals.size();
    KamiMap* kwmap = (KamiMap*)kw->p;

    KamiFn f = nullptr;
    KamiFn init = nullptr;
    KamiValue* f_caps = nullptr;
    KamiValue* init_caps = nullptr;
    int64_t builtin_id = -1;
    int64_t n2 = 0;
    std::vector<KamiValue*> argv2(vals.size() + 3);
    KamiValue packed[2] = {{KT_NONE, {0}}, {KT_NONE, {0}}};
    PinGuard pin(packed, 2);
    {
        Lock lk(g_lock);
        if (fn->tag == KT_CLASS) {
            KamiClassObj* c = (KamiClassObj*)fn->p;
            KamiInstance* inst = (KamiInstance*)gc_alloc(sizeof(KamiInstance), KT_OBJECT);
            inst->cls = c;
            inst->fields = new std::unordered_map<std::string, KamiValue>();
            out->tag = KT_OBJECT;
            out->p = inst;
            if (KamiValue* m = class_lookup(c, "__init__")) {
                KamiFuncObj* fo = (KamiFuncObj*)m->p;
                std::vector<KamiValue*> argv1(vals.size() + 1);
                argv1[0] = out; // self
                for (size_t i = 0; i < vals.size(); i++) argv1[i + 1] = argv[i];
                n2 = prep_user_argv(fo, argv1.data(), nargs + 1, kwmap, argv2.data(), packed);
                init = (KamiFn)fo->fn;
                init_caps = fo->captures;
            } else if (nargs != 0 || kwmap->count != 0) {
                panic(std::string(c->name) + "() takes no arguments");
            }
        } else {
            if (fn->tag != KT_FUNC)
                panic(std::string("'") + type_name(fn->tag) + "' object is not callable");
            KamiFuncObj* fo = (KamiFuncObj*)fn->p;
            if (fo->builtin_id >= 0) {
                if (kwmap->count != 0)
                    panic(std::string(fo->name ? fo->name : "<builtin>") +
                          "() does not accept keyword arguments through **kwargs");
                if (nargs > 16) panic("too many arguments in *args call to a builtin");
                builtin_id = fo->builtin_id;
            } else {
                n2 = prep_user_argv(fo, argv.data(), nargs, kwmap, argv2.data(), packed);
                f = (KamiFn)fo->fn;
                f_caps = fo->captures;
            }
        }
    }
    if (builtin_id >= 0) {
        kami_builtin(builtin_id, out, argv.data(), nargs);
        return;
    }
    if (init) {
        KamiValue dummy;
        init(&dummy, argv2.data(), n2, init_caps);
        return;
    }
    if (f) f(out, argv2.data(), n2, f_caps);
}

void kami_map_merge(KamiValue* dst, const KamiValue* src) {
    Lock lk(g_lock);
    if (dst->tag != KT_MAP || src->tag != KT_MAP)
        panic("argument after ** must be a mapping");
    KamiMap* d = (KamiMap*)dst->p;
    KamiMap* s = (KamiMap*)src->p;
    for (int64_t i = 0; i < s->nentries; i++) {
        MapEntry& en = s->entries[i];
        if (!en.used) continue;
        map_set(d, &en.key, &en.val);
    }
}

static void method_impl(KamiValue* out, KamiValue* obj, const char* name, KamiValue** argv,
                        int64_t nargs, KamiMap* kwmap) {
    KamiFn user_fn = nullptr;
    KamiValue* user_caps = nullptr;
    int64_t user_nargs = 0;
    KamiValue* argv2[19];
    KamiValue packed[2] = {{KT_NONE, {0}}, {KT_NONE, {0}}};
    PinGuard pin(packed, 2);
    {
        std::unique_lock<std::recursive_mutex> lk(g_lock);
        KamiValue o = *obj;
        std::string m = name;
        if (kwmap && kwmap->count && o.tag != KT_OBJECT && o.tag != KT_CLASS)
            panic(m + "() does not accept keyword arguments");
        if (o.tag == KT_PYOBJ) { // bridged CPython object: dispatch through the C-API
            pyobj_method(out, obj, m, argv, nargs);
            return;
        }
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
                out->tag = KT_STR;
                out->p = str_new(sv.data(), (int64_t)sv.size());
                return;
            }
            if ((m == "strip" || m == "lstrip" || m == "rstrip") && nargs <= 1) {
                std::string cut = " \t\r\n\f\v";
                if (nargs == 1) {
                    if (argv[0]->tag != KT_STR) panic(m + "() expects a str");
                    KamiStr* cs = (KamiStr*)argv[0]->p;
                    cut.assign(cs->data, (size_t)cs->len);
                }
                size_t b = m == "rstrip" ? 0 : sv.find_first_not_of(cut);
                size_t e = m == "lstrip" ? (sv.empty() ? std::string::npos : sv.size() - 1)
                                         : sv.find_last_not_of(cut);
                std::string r = b == std::string::npos || e == std::string::npos
                                    ? ""
                                    : sv.substr(b, e - b + 1);
                out->tag = KT_STR;
                out->p = str_new(r.data(), (int64_t)r.size());
                return;
            }
            if (m == "splitlines" && nargs <= 1) {
                bool keepends = nargs == 1 && kami_truthy(argv[0]);
                KamiList* r = list_new(4);
                out->tag = KT_LIST;
                out->p = r; // root before allocating the pieces
                size_t start = 0;
                for (size_t k = 0; k <= sv.size(); k++) {
                    if (k == sv.size()) {
                        if (start < sv.size()) {
                            KamiValue v{KT_STR, {0}};
                            v.p = str_new(sv.data() + start, (int64_t)(k - start));
                            list_push((KamiList*)out->p, &v);
                        }
                        break;
                    }
                    if (sv[k] != '\n') continue;
                    size_t stop = keepends ? k + 1 : k;
                    if (!keepends && stop > start && sv[stop - 1] == '\r') stop--;
                    KamiValue v{KT_STR, {0}};
                    v.p = str_new(sv.data() + start, (int64_t)(stop - start));
                    list_push((KamiList*)out->p, &v);
                    start = k + 1;
                }
                return;
            }
            if ((m == "partition" || m == "rpartition") && nargs == 1) {
                if (argv[0]->tag != KT_STR) panic(m + "() expects a str separator");
                KamiStr* ss = (KamiStr*)argv[0]->p;
                std::string sep(ss->data, (size_t)ss->len);
                if (sep.empty()) panic("empty separator");
                size_t at = m == "partition" ? sv.find(sep) : sv.rfind(sep);
                KamiList* r = list_new(3);
                out->tag = KT_LIST;
                out->p = r;
                std::string parts[3];
                if (at == std::string::npos) {
                    if (m == "partition") parts[0] = sv;
                    else parts[2] = sv;
                } else {
                    parts[0] = sv.substr(0, at);
                    parts[1] = sep;
                    parts[2] = sv.substr(at + sep.size());
                }
                for (int k = 0; k < 3; k++) {
                    KamiValue v{KT_STR, {0}};
                    v.p = str_new(parts[k].data(), (int64_t)parts[k].size());
                    list_push((KamiList*)out->p, &v);
                }
                return;
            }
            if (m == "capitalize" && nargs == 0) {
                std::string r = sv;
                for (size_t k = 0; k < r.size(); k++)
                    r[k] = k == 0 ? (char)toupper((unsigned char)r[k])
                                  : (char)tolower((unsigned char)r[k]);
                out->tag = KT_STR;
                out->p = str_new(r.data(), (int64_t)r.size());
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
                out->tag = KT_STR;
                out->p = str_new(r.data(), (int64_t)r.size());
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
                out->tag = KT_STR;
                out->p = str_new(r.data(), (int64_t)r.size());
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
                            tmp.tag = KT_STR;
                            tmp.p = str_new(sv.data() + b, (int64_t)(i - b));
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
                        tmp.tag = KT_STR;
                        tmp.p = str_new(piece.data(), (int64_t)piece.size());
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
                out->tag = KT_STR;
                out->p = str_new(r.data(), (int64_t)r.size());
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
                out->tag = KT_STR;
                out->p = str_new(r.data(), (int64_t)r.size());
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
            if ((m == "find" || m == "index") && nargs >= 1 && nargs <= 3) {
                if (argv[0]->tag != KT_STR) panic("str." + m + "() expects a str");
                KamiStr* p = (KamiStr*)argv[0]->p;
                // optional start/end window, like CPython
                int64_t from = 0, to = (int64_t)sv.size();
                if (nargs >= 2 && argv[1]->tag == KT_INT) from = argv[1]->i;
                if (nargs >= 3 && argv[2]->tag == KT_INT) to = argv[2]->i;
                if (from < 0) from += (int64_t)sv.size();
                if (to < 0) to += (int64_t)sv.size();
                if (from < 0) from = 0;
                if (to > (int64_t)sv.size()) to = (int64_t)sv.size();
                int64_t r = -1;
                if (from <= to) {
                    size_t f = sv.substr(0, (size_t)to)
                                   .find(std::string(p->data, (size_t)p->len), (size_t)from);
                    if (f != std::string::npos) r = (int64_t)f;
                }
                if (r < 0 && m == "index") panic("ValueError: substring not found");
                out->tag = KT_INT;
                out->i = r;
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
                out->tag = KT_STR;
                out->p = str_new(r.data(), (int64_t)r.size());
                return;
            }
            if (m == "swapcase" && nargs == 0) {
                std::string r = sv;
                for (char& c : r) {
                    if (islower((unsigned char)c)) c = (char)toupper((unsigned char)c);
                    else if (isupper((unsigned char)c)) c = (char)tolower((unsigned char)c);
                }
                out->tag = KT_STR;
                out->p = str_new(r.data(), (int64_t)r.size());
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
            if ((m == "rfind" || m == "rindex") && nargs >= 1 && nargs <= 3) {
                if (argv[0]->tag != KT_STR) panic("str." + m + "() expects a str");
                KamiStr* p = (KamiStr*)argv[0]->p;
                int64_t from = 0, to = (int64_t)sv.size();
                if (nargs >= 2 && argv[1]->tag == KT_INT) from = argv[1]->i;
                if (nargs >= 3 && argv[2]->tag == KT_INT) to = argv[2]->i;
                if (from < 0) from += (int64_t)sv.size();
                if (to < 0) to += (int64_t)sv.size();
                if (from < 0) from = 0;
                if (to > (int64_t)sv.size()) to = (int64_t)sv.size();
                int64_t r = -1;
                if (from <= to) {
                    std::string needle(p->data, (size_t)p->len);
                    size_t f = sv.substr(0, (size_t)to).rfind(needle);
                    if (f != std::string::npos && (int64_t)f >= from) r = (int64_t)f;
                }
                if (r < 0 && m == "rindex") panic("ValueError: substring not found");
                out->tag = KT_INT;
                out->i = r;
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
                out->tag = KT_STR;
                out->p = str_new(r.data(), (int64_t)r.size());
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
                out->tag = KT_STR;
                out->p = str_new(r.data(), (int64_t)r.size());
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
                out->tag = KT_STR;
                out->p = str_new(r.data(), (int64_t)r.size());
                return;
            }
            if (m == "encode" && nargs <= 1) { // no real bytes type: identity str
                out->tag = KT_STR;
                out->p = str_new(sv.data(), (int64_t)sv.size());
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
            if (m == "setdefault" && (nargs == 1 || nargs == 2)) {
                if (map_get(mp, argv[0], out)) return;
                KamiValue dflt{KT_NONE, {0}};
                if (nargs == 2) dflt = *argv[1];
                map_set(mp, argv[0], &dflt);
                *out = dflt;
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
        } else if (o.tag == KT_SOCKET) {
            socket_method(lk, out, obj, m, argv, nargs);
            return;
        } else if (o.tag == KT_LOCK) {
            KamiLock* l = (KamiLock*)o.p;
            if (m == "acquire" && nargs <= 2) {
                double tmo = -1.0;
                bool blocking = nargs < 1 || kami_truthy(argv[0]);
                if (nargs == 2) {
                    if (argv[1]->tag == KT_FLOAT) tmo = argv[1]->f;
                    else if (argv[1]->tag == KT_INT) tmo = (double)argv[1]->i;
                }
                if (!blocking) tmo = 0.0;
                out->tag = KT_BOOL;
                out->i = lock_acquire(lk, l, tmo);
                return;
            }
            if (m == "release" && nargs == 0) {
                lock_release(l);
                out->tag = KT_NONE; out->i = 0;
                return;
            }
            if (m == "locked" && nargs == 0) {
                out->tag = KT_BOOL;
                out->i = l->depth > 0;
                return;
            }
            if (m == "__enter__" && nargs == 0) {
                lock_acquire(lk, l, -1.0);
                *out = o;
                return;
            }
            if (m == "__exit__") {
                lock_release(l);
                out->tag = KT_NONE; out->i = 0;
                return;
            }
            panic("'lock' object has no method '" + m + "'");
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
                out->tag = KT_STR;
                out->p = str_new(data.data(), (int64_t)data.size());
                return;
            }
            if (m == "readline" && nargs == 0) {
                std::string line;
                int c;
                while ((c = fgetc(fp)) != EOF) {
                    line += (char)c;
                    if (c == '\n') break;
                }
                out->tag = KT_STR;
                out->p = str_new(line.data(), (int64_t)line.size());
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
                if (!f->closed && !f->no_close) {
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
            KamiValue* argv1[19];
            int64_t k = 0;
            if (bound) argv1[k++] = obj;
            for (int64_t i = 0; i < nargs && k < 19; i++) argv1[k++] = argv[i];
            user_nargs = prep_user_argv(fo, argv1, k, kwmap, argv2, packed);
            user_fn = (KamiFn)fo->fn;
            user_caps = fo->captures;
        } else if (o.tag == KT_CLASS) {
            // unbound call: ClassName.method(self, args...)
            KamiClassObj* c = (KamiClassObj*)o.p;
            KamiValue* mv = class_lookup(c, m);
            if (!mv || mv->tag != KT_FUNC)
                panic(std::string("class '") + c->name + "' has no method '" + m + "'");
            KamiFuncObj* fo = (KamiFuncObj*)mv->p;
            user_nargs = prep_user_argv(fo, argv, nargs, kwmap, argv2, packed);
            user_fn = (KamiFn)fo->fn;
            user_caps = fo->captures;
        }
        if (!user_fn)
            panic(std::string("'") + type_name(o.tag) + "' object has no method '" + m + "'(" +
                  std::to_string(nargs) + " args)");
    }
    // Invoke user method WITHOUT holding the lock.
    user_fn(out, argv2, user_nargs, user_caps);
}

void kami_method(KamiValue* out, KamiValue* obj, const char* name, KamiValue** argv,
                 int64_t nargs) {
    method_impl(out, obj, name, argv, nargs, nullptr);
}

// obj.m(*args, **kwargs): expand the packed positional list and forward the
// keyword dict into the normal method dispatch.
void kami_method_star(KamiValue* out, KamiValue* obj, const char* name,
                      const KamiValue* pos, const KamiValue* kw) {
    if (pos->tag != KT_LIST) panic("argument after * must be an iterable");
    if (kw->tag != KT_MAP) panic("argument after ** must be a mapping");
    std::vector<KamiValue> vals;
    {
        Lock lk(g_lock);
        KamiList* l = (KamiList*)pos->p;
        vals.assign(l->items, l->items + l->len);
    }
    PinGuard pinv(vals.data(), (int64_t)vals.size());
    std::vector<KamiValue*> argv(vals.size());
    for (size_t i = 0; i < vals.size(); i++) argv[i] = &vals[i];
    method_impl(out, obj, name, argv.data(), (int64_t)vals.size(), (KamiMap*)kw->p);
}

} // extern "C"
