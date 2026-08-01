// Classes, instances, attributes, and the exception machinery.
#include "rt_internal.h"

#include <cstdio>
#include <cstdlib>

namespace kami {

KamiValue* class_lookup(KamiClassObj* c, const std::string& name) {
    while (c) {
        auto it = c->members->find(name);
        if (it != c->members->end()) return &it->second;
        c = c->parent;
    }
    return nullptr;
}

} // namespace kami

using namespace kami;

extern "C" {

void kami_global_make_class(int64_t idx, const char* name, int64_t parent_gidx) {
    Lock lk(g_lock);
    KamiClassObj* parent = nullptr;
    if (parent_gidx >= 0) {
        KamiValue& pv = g_globals[(size_t)parent_gidx];
        if (pv.tag != KT_CLASS) panic(std::string("base of class '") + name + "' is not a class");
        parent = (KamiClassObj*)pv.p;
    }
    KamiClassObj* c = (KamiClassObj*)gc_alloc(sizeof(KamiClassObj), KT_CLASS);
    c->name = name;
    c->parent = parent;
    c->members = new std::unordered_map<std::string, KamiValue>();
    g_globals[(size_t)idx].tag = KT_CLASS;
    g_globals[(size_t)idx].p = c;
}

void kami_class_add_method(int64_t cls_gidx, const char* name, void* fnptr,
                           int64_t min_arity, int64_t arity, int64_t kwonly,
                           int64_t flags, const char* param_names) {
    Lock lk(g_lock);
    KamiClassObj* c = (KamiClassObj*)g_globals[(size_t)cls_gidx].p;
    KamiFuncObj* f = (KamiFuncObj*)gc_alloc(sizeof(KamiFuncObj), KT_FUNC);
    f->fn = fnptr;
    f->min_arity = min_arity;
    f->arity = arity;
    f->builtin_id = -1;
    f->captures = nullptr;
    f->ncaptures = 0;
    f->kwonly = kwonly;
    f->flags = flags;
    f->param_names = param_names;
    f->name = name;
    KamiValue v;
    v.tag = KT_FUNC;
    v.p = f;
    (*c->members)[name] = v;
}

void kami_attr_get(KamiValue* out, const KamiValue* obj, const char* name) {
    Lock lk(g_lock);
    KamiValue o = *obj;
    if (o.tag == KT_OBJECT) {
        KamiInstance* in = (KamiInstance*)o.p;
        auto it = in->fields->find(name);
        if (it != in->fields->end()) {
            *out = it->second;
            return;
        }
        if (KamiValue* m = class_lookup(in->cls, name)) {
            *out = *m; // unbound; direct method calls go through kami_method
            return;
        }
        panic(std::string("AttributeError: '") + in->cls->name + "' object has no attribute '" +
              name + "'");
    }
    if (o.tag == KT_CLASS) {
        KamiClassObj* c = (KamiClassObj*)o.p;
        if (KamiValue* m = class_lookup(c, name)) {
            *out = *m;
            return;
        }
        panic(std::string("AttributeError: class '") + c->name + "' has no attribute '" + name +
              "'");
    }
    panic(std::string("AttributeError: '") + type_name(o.tag) + "' object has no attribute '" +
          name + "'");
}

void kami_attr_set(KamiValue* obj, const char* name, const KamiValue* val) {
    Lock lk(g_lock);
    KamiValue o = *obj;
    if (o.tag == KT_OBJECT) {
        (*((KamiInstance*)o.p)->fields)[name] = *val;
        return;
    }
    if (o.tag == KT_CLASS) {
        (*((KamiClassObj*)o.p)->members)[name] = *val;
        return;
    }
    panic(std::string("cannot set attribute on '") + type_name(o.tag) + "' object");
}

void kami_unpack(KamiValue* out, const KamiValue* seq, int64_t idx, int64_t expect_len) {
    Lock lk(g_lock);
    if (seq->tag != KT_LIST)
        panic(std::string("cannot unpack '") + type_name(seq->tag) + "' object");
    KamiList* l = (KamiList*)seq->p;
    if (l->len != expect_len)
        panic("unpack expected " + std::to_string(expect_len) + " values, got " +
              std::to_string(l->len));
    *out = l->items[idx];
}

void kami_slice(KamiValue* out, const KamiValue* obj, const KamiValue* start,
                const KamiValue* stop, const KamiValue* step) {
    Lock lk(g_lock);
    KamiValue o = *obj;
    int64_t len;
    if (o.tag == KT_LIST) len = ((KamiList*)o.p)->len;
    else if (o.tag == KT_STR) len = ((KamiStr*)o.p)->len;
    else panic(std::string("'") + type_name(o.tag) + "' object is not sliceable");

    auto as_idx = [&](const KamiValue* v, int64_t dflt) -> int64_t {
        if (v->tag == KT_NONE) return dflt;
        if (v->tag != KT_INT) panic("slice indices must be integers or None");
        return v->i;
    };
    int64_t stp = as_idx(step, 1);
    if (stp == 0) panic("slice step cannot be zero");
    int64_t lo, hi;
    if (stp > 0) {
        lo = as_idx(start, 0);
        hi = as_idx(stop, len);
    } else {
        lo = as_idx(start, len - 1);
        hi = as_idx(stop, -len - 1);
    }
    auto clamp = [&](int64_t i, bool is_start) -> int64_t {
        if (i < 0) i += len;
        if (stp > 0) {
            if (i < 0) i = 0;
            if (i > len) i = len;
        } else {
            if (i < -1) i = -1;
            if (i >= len) i = len - 1;
        }
        (void)is_start;
        return i;
    };
    // handle the "-len-1" sentinel default for negative-step stop
    int64_t b = clamp(lo, true);
    int64_t e2;
    if (stp < 0 && stop->tag == KT_NONE) e2 = -1;
    else e2 = clamp(hi, false);

    if (o.tag == KT_STR) {
        KamiStr* s = (KamiStr*)o.p;
        std::string r;
        if (stp > 0)
            for (int64_t i = b; i < e2; i += stp) r += s->data[i];
        else
            for (int64_t i = b; i > e2; i += stp) r += s->data[i];
        out->tag = KT_STR;
        out->p = str_new(r.data(), (int64_t)r.size());
        return;
    }
    KamiList* src = (KamiList*)o.p;
    // collect into a temporary C++ buffer first (list_new may trigger GC, and
    // reading src after alloc is fine: src is rooted via obj)
    KamiList* r = list_new(4);
    out->tag = KT_LIST;
    out->p = r;
    if (stp > 0)
        for (int64_t i = b; i < e2; i += stp) list_push(r, &src->items[i]);
    else
        for (int64_t i = b; i > e2; i += stp) list_push(r, &src->items[i]);
}

// ---- exceptions ----

int64_t kami_try(void* body_fn, KamiValue* frame) {
    using BodyFn = int64_t (*)(KamiValue*);
    size_t depth;
    {
        Lock lk(g_lock);
        depth = tls_frames()->frames.size();
    }
    try {
        return ((BodyFn)body_fn)(frame);
    } catch (KamiError& e) {
        Lock lk(g_lock);
        // Unwinding skipped kami_frame_pop calls of frames inside the body:
        // repair the root registry so the GC never scans dead stack memory.
        FrameStack* fs = tls_frames();
        if (fs->frames.size() > depth) fs->frames.resize(depth);
        tls_error() = e.msg;
        return -1;
    }
}

void kami_last_error(KamiValue* out) {
    Lock lk(g_lock);
    const std::string& m = tls_error();
    KamiStr* s = str_new(m.data(), (int64_t)m.size());
    out->tag = KT_STR;
    out->p = s;
}

void kami_raise(const KamiValue* msg) {
    std::string m;
    {
        Lock lk(g_lock);
        m = value_str(msg);
    }
    throw KamiError{m};
}

void kami_rethrow(void) {
    std::string m;
    {
        Lock lk(g_lock);
        m = tls_error();
    }
    throw KamiError{m};
}

void kami_run_module(void* module_fn) {
    using ModFn = void (*)(KamiValue*, KamiValue**, int64_t, KamiValue*);
    KamiValue ret{KT_NONE, {0}};
    try {
        ((ModFn)module_fn)(&ret, nullptr, 0, nullptr);
    } catch (KamiError& e) {
        fflush(stdout);
        fprintf(stderr, "KamiPython runtime error: %s\n", e.msg.c_str());
        fflush(stderr);
        _Exit(1);
    }
}

} // extern "C"
