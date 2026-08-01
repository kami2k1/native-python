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
    case KT_STR: return str_hash((KamiStr*)v->p);
    case KT_LIST: { // tuples are represented as lists; hash deep like a tuple
        KamiList* l = (KamiList*)v->p;
        uint64_t h = 0x345678ull ^ (uint64_t)l->len;
        for (int64_t i = 0; i < l->len; i++)
            h = h * 1000003ull ^ value_hash(&l->items[i]);
        return h;
    }
    default: panic(std::string("unhashable type: '") + type_name(v->tag) + "'");
    }
}

bool map_get(KamiMap* m, const KamiValue* k, KamiValue* out); // defined below

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
    case KT_MAP: {
        // Python compares dicts by content: same keys mapping to equal values.
        KamiMap *x = (KamiMap*)a->p, *y = (KamiMap*)b->p;
        if (x == y) return true;
        if (x->count != y->count) return false;
        for (int64_t i = 0; i < x->nentries; i++) {
            if (!x->entries[i].used) continue;
            KamiValue other;
            if (!map_get(y, &x->entries[i].key, &other)) return false;
            if (!value_eq(&x->entries[i].val, &other)) return false;
        }
        return true;
    }
    case KT_SET: {
        // Sets are maps with None values: equal when the key sets match.
        KamiMap *x = (KamiMap*)a->p, *y = (KamiMap*)b->p;
        if (x == y) return true;
        if (x->count != y->count) return false;
        for (int64_t i = 0; i < x->nentries; i++) {
            if (!x->entries[i].used) continue;
            KamiValue other;
            if (!map_get(y, &x->entries[i].key, &other)) return false;
        }
        return true;
    }
    default: return a->p == b->p; // identity for functions/threads/objects
    }
}

KamiMap* map_new() {
    KamiMap* m = (KamiMap*)gc_alloc(sizeof(KamiMap), KT_MAP);
    m->count = 0;
    m->nentries = 0;
    m->ecap = 0;
    m->entries = nullptr;
    m->index = nullptr;
    m->icap = 0;
    return m;
}

static void index_rebuild(KamiMap* m, int64_t icap) {
    free(m->index);
    m->index = (int64_t*)calloc((size_t)icap, sizeof(int64_t));
    if (!m->index) panic("out of memory");
    m->icap = icap;
    for (int64_t i = 0; i < m->nentries; i++) {
        if (!m->entries[i].used) continue;
        uint64_t j = m->entries[i].hash & (uint64_t)(icap - 1);
        while (m->index[j] != 0) j = (j + 1) & (uint64_t)(icap - 1);
        m->index[j] = i + 1;
    }
}

// Compact away tombstones, preserving insertion order (does not touch index).
static void map_compact(KamiMap* m) {
    int64_t w = 0;
    for (int64_t i = 0; i < m->nentries; i++)
        if (m->entries[i].used) m->entries[w++] = m->entries[i];
    m->nentries = w;
}

void map_set(KamiMap* m, const KamiValue* k, const KamiValue* v) {
    uint64_t h = value_hash(k);
    if (m->icap == 0) index_rebuild(m, 8);
    // lookup
    uint64_t j = h & (uint64_t)(m->icap - 1);
    while (m->index[j] != 0) {
        int64_t ei = m->index[j] - 1;
        if (m->entries[ei].used && m->entries[ei].hash == h &&
            value_eq(&m->entries[ei].key, k)) {
            m->entries[ei].val = *v;
            return;
        }
        j = (j + 1) & (uint64_t)(m->icap - 1);
    }
    // insert new entry (append)
    if (m->nentries == m->ecap) {
        int64_t ncap = m->ecap ? m->ecap * 2 : 8;
        MapEntry* ne = (MapEntry*)calloc((size_t)ncap, sizeof(MapEntry));
        if (!ne) panic("out of memory");
        if (m->entries) memcpy(ne, m->entries, (size_t)m->nentries * sizeof(MapEntry));
        free(m->entries);
        m->entries = ne;
        m->ecap = ncap;
        gc_track_extra((uint64_t)ncap * sizeof(MapEntry));
    }
    int64_t ei = m->nentries++;
    m->entries[ei].hash = h;
    m->entries[ei].used = true;
    m->entries[ei].key = *k;
    m->entries[ei].val = *v;
    m->count++;
    // keep the index load factor healthy; rebuild (dropping tombstones) grows
    // the table and re-inserts every live entry, so no manual slot write needed.
    if (m->nentries * 4 >= m->icap * 3) {
        int64_t target = m->icap;
        while (m->count * 4 >= target * 3) target *= 2;
        map_compact(m);
        index_rebuild(m, target);
        return;
    }
    m->index[j] = ei + 1;
}

bool map_get(KamiMap* m, const KamiValue* k, KamiValue* out) {
    if (m->icap == 0) return false;
    uint64_t h = value_hash(k);
    uint64_t j = h & (uint64_t)(m->icap - 1);
    while (m->index[j] != 0) {
        int64_t ei = m->index[j] - 1;
        if (m->entries[ei].used && m->entries[ei].hash == h &&
            value_eq(&m->entries[ei].key, k)) {
            *out = m->entries[ei].val;
            return true;
        }
        j = (j + 1) & (uint64_t)(m->icap - 1);
    }
    return false;
}

bool map_del(KamiMap* m, const KamiValue* k) {
    if (m->icap == 0) return false;
    uint64_t h = value_hash(k);
    uint64_t j = h & (uint64_t)(m->icap - 1);
    while (m->index[j] != 0) {
        int64_t ei = m->index[j] - 1;
        if (m->entries[ei].used && m->entries[ei].hash == h &&
            value_eq(&m->entries[ei].key, k)) {
            m->entries[ei].used = false;
            m->count--;
            index_rebuild(m, m->icap); // drop the slot; keep order
            return true;
        }
        j = (j + 1) & (uint64_t)(m->icap - 1);
    }
    return false;
}

} // namespace kami
