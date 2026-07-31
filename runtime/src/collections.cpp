// Lists and maps (dicts).
#include "rt_internal.h"

#include <cstdlib>
#include <cstring>

namespace kami {

KamiList* list_new(int64_t cap) {
    if (cap < 4) cap = 4;
    KamiList* l = (KamiList*)gc_alloc(sizeof(KamiList), KT_LIST);
    l->items = (KamiValue*)calloc((size_t)cap, sizeof(KamiValue));
    if (!l->items) panic("out of memory");
    l->len = 0;
    l->cap = cap;
    gc_track_extra((uint64_t)cap * sizeof(KamiValue));
    return l;
}

void list_push(KamiList* l, const KamiValue* v) {
    if (l->len == l->cap) {
        int64_t ncap = l->cap * 2;
        KamiValue* ni = (KamiValue*)calloc((size_t)ncap, sizeof(KamiValue));
        if (!ni) panic("out of memory");
        memcpy(ni, l->items, (size_t)l->len * sizeof(KamiValue));
        free(l->items);
        l->items = ni;
        l->cap = ncap;
        gc_track_extra((uint64_t)ncap * sizeof(KamiValue));
    }
    l->items[l->len++] = *v;
}

uint64_t value_hash(const KamiValue* v) {
    switch (v->tag) {
    case KT_NONE: return 0x9e3779b97f4a7c15ull;
    case KT_BOOL:
    case KT_INT: {
        uint64_t x = (uint64_t)v->i;
        x ^= x >> 33; x *= 0xff51afd7ed558ccdull; x ^= x >> 33;
        return x;
    }
    case KT_FLOAT: {
        double d = v->f;
        if (d == (double)(int64_t)d) { // hash(2.0) == hash(2)
            KamiValue t{KT_INT, {.i = (int64_t)d}};
            return value_hash(&t);
        }
        uint64_t x;
        memcpy(&x, &d, 8);
        x ^= x >> 33; x *= 0xff51afd7ed558ccdull; x ^= x >> 33;
        return x;
    }
    case KT_STR: return ((KamiStr*)v->p)->hash;
    default: panic(std::string("unhashable type: '") + type_name(v->tag) + "'");
    }
}

bool value_eq(const KamiValue* a, const KamiValue* b) {
    // numeric cross-type equality
    if ((a->tag == KT_INT || a->tag == KT_BOOL) && (b->tag == KT_INT || b->tag == KT_BOOL))
        return a->i == b->i;
    if (a->tag == KT_FLOAT && (b->tag == KT_INT || b->tag == KT_BOOL))
        return a->f == (double)b->i;
    if ((a->tag == KT_INT || a->tag == KT_BOOL) && b->tag == KT_FLOAT)
        return (double)a->i == b->f;
    if (a->tag != b->tag) return false;
    switch (a->tag) {
    case KT_NONE: return true;
    case KT_FLOAT: return a->f == b->f;
    case KT_STR: {
        KamiStr *x = (KamiStr*)a->p, *y = (KamiStr*)b->p;
        return x->len == y->len && memcmp(x->data, y->data, (size_t)x->len) == 0;
    }
    case KT_LIST: {
        KamiList *x = (KamiList*)a->p, *y = (KamiList*)b->p;
        if (x->len != y->len) return false;
        for (int64_t i = 0; i < x->len; i++)
            if (!value_eq(&x->items[i], &y->items[i])) return false;
        return true;
    }
    default: return a->p == b->p; // identity for maps/functions/threads
    }
}

KamiMap* map_new() {
    KamiMap* m = (KamiMap*)gc_alloc(sizeof(KamiMap), KT_MAP);
    m->count = 0;
    m->cap = 0;
    m->entries = nullptr;
    return m;
}

static void map_grow(KamiMap* m) {
    int64_t ncap = m->cap ? m->cap * 2 : 8;
    MapEntry* ne = (MapEntry*)calloc((size_t)ncap, sizeof(MapEntry));
    if (!ne) panic("out of memory");
    for (int64_t i = 0; i < m->cap; i++) {
        if (!m->entries[i].used) continue;
        uint64_t j = m->entries[i].hash & (uint64_t)(ncap - 1);
        while (ne[j].used) j = (j + 1) & (uint64_t)(ncap - 1);
        ne[j] = m->entries[i];
    }
    free(m->entries);
    m->entries = ne;
    m->cap = ncap;
    gc_track_extra((uint64_t)ncap * sizeof(MapEntry));
}

void map_set(KamiMap* m, const KamiValue* k, const KamiValue* v) {
    if (m->cap == 0 || m->count * 4 >= m->cap * 3) map_grow(m);
    uint64_t h = value_hash(k);
    uint64_t j = h & (uint64_t)(m->cap - 1);
    while (m->entries[j].used) {
        if (m->entries[j].hash == h && value_eq(&m->entries[j].key, k)) {
            m->entries[j].val = *v;
            return;
        }
        j = (j + 1) & (uint64_t)(m->cap - 1);
    }
    m->entries[j].used = true;
    m->entries[j].hash = h;
    m->entries[j].key = *k;
    m->entries[j].val = *v;
    m->count++;
}

bool map_get(KamiMap* m, const KamiValue* k, KamiValue* out) {
    if (m->cap == 0) return false;
    uint64_t h = value_hash(k);
    uint64_t j = h & (uint64_t)(m->cap - 1);
    while (m->entries[j].used) {
        if (m->entries[j].hash == h && value_eq(&m->entries[j].key, k)) {
            *out = m->entries[j].val;
            return true;
        }
        j = (j + 1) & (uint64_t)(m->cap - 1);
    }
    return false;
}

} // namespace kami
