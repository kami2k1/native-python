// Value constructors, copies, truthiness and string conversion.
#include "rt_internal.h"

#include <cstdio>
#include <cstring>

namespace kami {

const char* type_name(int64_t tag) {
    switch (tag) {
    case KT_NONE: return "NoneType";
    case KT_BOOL: return "bool";
    case KT_INT: return "int";
    case KT_FLOAT: return "float";
    case KT_STR: return "str";
    case KT_LIST: return "list";
    case KT_MAP: return "dict";
    case KT_FUNC: return "function";
    case KT_THREAD: return "thread";
    case KT_CLASS: return "type";
    case KT_OBJECT: return "object";
    case KT_SET: return "set";
    case KT_FILE: return "file";
    case KT_SOCKET: return "socket";
    case KT_LOCK: return "lock";
    default: return "?";
    }
}

KamiStr* str_new(const char* data, int64_t len) {
    KamiStr* s = (KamiStr*)gc_alloc(sizeof(KamiStr) + (uint64_t)len, KT_STR);
    s->len = len;
    if (len) memcpy(s->data, data, (size_t)len);
    s->data[len] = '\0';
    uint64_t h = 1469598103934665603ull; // FNV-1a
    for (int64_t i = 0; i < len; i++) h = (h ^ (unsigned char)data[i]) * 1099511628211ull;
    s->hash = h;
    return s;
}

std::string format_float(double d) {
    char buf[64];
    snprintf(buf, sizeof buf, "%.12g", d);
    std::string s = buf;
    if (s.find_first_of(".eEni") == std::string::npos) s += ".0"; // int-valued floats: "3.0"
    return s;
}

std::string value_str(const KamiValue* v) {
    switch (v->tag) {
    case KT_NONE: return "None";
    case KT_BOOL: return v->i ? "True" : "False";
    case KT_INT: return std::to_string(v->i);
    case KT_FLOAT: return format_float(v->f);
    case KT_STR: {
        KamiStr* s = (KamiStr*)v->p;
        return std::string(s->data, (size_t)s->len);
    }
    case KT_LIST: {
        KamiList* l = (KamiList*)v->p;
        std::string out = "[";
        for (int64_t i = 0; i < l->len; i++) {
            if (i) out += ", ";
            out += value_repr(&l->items[i]);
        }
        return out + "]";
    }
    case KT_MAP: {
        KamiMap* m = (KamiMap*)v->p;
        std::string out = "{";
        bool first = true;
        for (int64_t i = 0; i < m->nentries; i++) {
            if (!m->entries[i].used) continue;
            if (!first) out += ", ";
            first = false;
            out += value_repr(&m->entries[i].key);
            out += ": ";
            out += value_repr(&m->entries[i].val);
        }
        return out + "}";
    }
    case KT_SET: {
        KamiMap* m = (KamiMap*)v->p;
        if (m->count == 0) return "set()";
        std::string out = "{";
        bool first = true;
        for (int64_t i = 0; i < m->nentries; i++) {
            if (!m->entries[i].used) continue;
            if (!first) out += ", ";
            first = false;
            out += value_repr(&m->entries[i].key);
        }
        return out + "}";
    }
    case KT_FILE: return "<file>";
    case KT_FUNC: {
        KamiFuncObj* f = (KamiFuncObj*)v->p;
        return std::string("<function ") + f->name + ">";
    }
    case KT_THREAD: return "<thread>";
    case KT_CLASS: return std::string("<class '") + ((KamiClassObj*)v->p)->name + "'>";
    case KT_OBJECT:
        return std::string("<") + ((KamiInstance*)v->p)->cls->name + " object>";
    default: return "<?>";
    }
}

std::string value_repr(const KamiValue* v) {
    if (v->tag != KT_STR) return value_str(v);
    // Python's repr(): quote and escape so that the result can be read back.
    KamiStr* s = (KamiStr*)v->p;
    bool has_single = memchr(s->data, '\'', (size_t)s->len) != nullptr;
    bool has_double = memchr(s->data, '"', (size_t)s->len) != nullptr;
    char q = (has_single && !has_double) ? '"' : '\'';
    std::string out(1, q);
    for (int64_t i = 0; i < s->len; i++) {
        unsigned char c = (unsigned char)s->data[i];
        if (c == '\\') out += "\\\\";
        else if (c == (unsigned char)q) { out += '\\'; out += (char)c; }
        else if (c == '\n') out += "\\n";
        else if (c == '\r') out += "\\r";
        else if (c == '\t') out += "\\t";
        else if (c < 0x20 || c == 0x7f) {
            static const char* hex = "0123456789abcdef";
            out += "\\x";
            out += hex[c >> 4];
            out += hex[c & 15];
        } else {
            out += (char)c;
        }
    }
    out += q;
    return out;
}

} // namespace kami

using namespace kami;

extern "C" {

void kami_copy(KamiValue* dst, const KamiValue* src) {
    Lock lk(g_lock);
    KamiValue tmp = *src;
    *dst = tmp;
}

void kami_make_none(KamiValue* out) {
    Lock lk(g_lock);
    out->tag = KT_NONE;
    out->i = 0;
}

void kami_make_bool(KamiValue* out, int64_t b) {
    Lock lk(g_lock);
    out->tag = KT_BOOL;
    out->i = b ? 1 : 0;
}

void kami_make_int(KamiValue* out, int64_t v) {
    Lock lk(g_lock);
    out->tag = KT_INT;
    out->i = v;
}

void kami_make_float(KamiValue* out, double v) {
    Lock lk(g_lock);
    out->tag = KT_FLOAT;
    out->f = v;
}

void kami_make_str(KamiValue* out, const char* data, int64_t len) {
    Lock lk(g_lock);
    KamiStr* s = str_new(data, len);
    out->tag = KT_STR;
    out->p = s;
}

void kami_make_list(KamiValue* out, KamiValue** items, int64_t n) {
    Lock lk(g_lock);
    KamiList* l = list_new(n > 4 ? n : 4);
    for (int64_t i = 0; i < n; i++) l->items[l->len++] = *items[i];
    out->tag = KT_LIST;
    out->p = l;
}

// `def f(a, *rest)` prologue: collect argv[first..nargs) into a fresh list and
// store it in the `rest` slot (already a registered GC root).
void kami_pack_varargs(KamiValue* out, KamiValue** argv, int64_t nargs, int64_t first) {
    Lock lk(g_lock);
    int64_t n = nargs > first ? nargs - first : 0;
    KamiList* l = list_new(n > 4 ? n : 4);
    out->tag = KT_LIST;
    out->p = l; // root before anything else can allocate
    for (int64_t i = 0; i < n; i++) l->items[l->len++] = *argv[first + i];
}

void kami_make_map(KamiValue* out) {
    Lock lk(g_lock);
    KamiMap* m = map_new();
    out->tag = KT_MAP;
    out->p = m;
}

void kami_make_builtin_func(KamiValue* out, int64_t builtin_id, const char* name) {
    Lock lk(g_lock);
    KamiFuncObj* f = (KamiFuncObj*)gc_alloc(sizeof(KamiFuncObj), KT_FUNC);
    f->fn = nullptr;
    f->min_arity = 0;
    f->arity = 16;
    f->builtin_id = builtin_id;
    f->name = name;
    f->captures = nullptr;
    f->ncaptures = 0;
    out->tag = KT_FUNC;
    out->p = f;
}

void kami_make_closure(KamiValue* out, void* fnptr, int64_t min_arity, int64_t arity,
                       const char* name, KamiValue** capture_slots, int64_t ncap) {
    Lock lk(g_lock);
    KamiFuncObj* f = (KamiFuncObj*)gc_alloc(sizeof(KamiFuncObj), KT_FUNC);
    f->fn = fnptr;
    f->min_arity = min_arity;
    f->arity = arity;
    f->builtin_id = -1;
    f->name = name;
    f->ncaptures = ncap;
    f->captures = nullptr;
    out->tag = KT_FUNC;
    out->p = f; // root before the capture array alloc (which can GC)
    if (ncap > 0) {
        f->captures = (KamiValue*)malloc(sizeof(KamiValue) * (size_t)ncap);
        if (!f->captures) panic("out of memory");
        for (int64_t i = 0; i < ncap; i++) f->captures[i] = *capture_slots[i];
        gc_track_extra((uint64_t)ncap * sizeof(KamiValue));
    }
}

void kami_make_set(KamiValue* out) {
    Lock lk(g_lock);
    KamiMap* m = map_new();
    m->h.type = KT_SET;
    out->tag = KT_SET;
    out->p = m;
}

void kami_set_add(KamiValue* set, const KamiValue* v) {
    Lock lk(g_lock);
    if (set->tag != KT_SET) panic("internal: set_add on non-set");
    KamiValue none{KT_NONE, {0}};
    map_set((KamiMap*)set->p, v, &none);
}

int32_t kami_truthy(const KamiValue* v) {
    Lock lk(g_lock);
    switch (v->tag) {
    case KT_NONE: return 0;
    case KT_BOOL:
    case KT_INT: return v->i != 0;
    case KT_FLOAT: return v->f != 0.0;
    case KT_STR: return ((KamiStr*)v->p)->len != 0;
    case KT_LIST: return ((KamiList*)v->p)->len != 0;
    case KT_MAP:
    case KT_SET: return ((KamiMap*)v->p)->count != 0;
    default: return 1;
    }
}

} // extern "C"
