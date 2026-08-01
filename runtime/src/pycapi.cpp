// CPython C-API compatibility layer (Part I.3).
//
// Heavy C extension modules (_hashlib, _ssl, _sqlite3, zlib, ...) are not
// re-implemented in C++: instead the runtime loads the *real* CPython shared
// library (libpython3.x.so / python3xx.dll) at runtime, initializes an
// embedded interpreter, and lets CPython's own import machinery load and link
// the original .so/.pyd extension files. Values crossing the boundary are
// converted (int/float/bool/str/list/dict/None) or wrapped as KT_PYOBJ.
//
// Everything is resolved with dlopen/dlsym against the stable C API — kamipy
// itself does not link against libpython, so compiled programs run unchanged
// on machines without Python (they only fail if the program actually imports
// a CPython extension module there).
#include "rt_internal.h"

#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#else
#include <dlfcn.h>
#include <filesystem>
#endif

namespace kami {
namespace {

using ssz = intptr_t;

// Minimal stable view of a PyObject (ob_refcnt, ob_type) — only used for
// exact-type comparisons; reference counting goes through Py_IncRef/Py_DecRef.
struct PyObj {
    ssz refcnt;
    void* type;
};

struct PyAPI {
    bool ready = false;

    void (*Py_InitializeEx)(int) = nullptr;
    ssz (*PyEval_SaveThread)() = nullptr;
    int (*PyGILState_Ensure)() = nullptr;
    void (*PyGILState_Release)(int) = nullptr;
    void (*Py_IncRef)(PyObj*) = nullptr;
    void (*Py_DecRef)(PyObj*) = nullptr;

    PyObj* (*PyImport_ImportModule)(const char*) = nullptr;
    PyObj* (*PyObject_GetAttrString)(PyObj*, const char*) = nullptr;
    int (*PyObject_HasAttrString)(PyObj*, const char*) = nullptr;
    PyObj* (*PyObject_CallObject)(PyObj*, PyObj*) = nullptr;
    int (*PyCallable_Check)(PyObj*) = nullptr;
    PyObj* (*PyObject_Str)(PyObj*) = nullptr;
    int (*PyObject_IsTrue)(PyObj*) = nullptr;

    PyObj* (*PyTuple_New)(ssz) = nullptr;
    int (*PyTuple_SetItem)(PyObj*, ssz, PyObj*) = nullptr;
    ssz (*PyTuple_Size)(PyObj*) = nullptr;
    PyObj* (*PyTuple_GetItem)(PyObj*, ssz) = nullptr;
    PyObj* (*PyDict_New)() = nullptr;
    int (*PyDict_SetItem)(PyObj*, PyObj*, PyObj*) = nullptr;
    int (*PyDict_Next)(PyObj*, ssz*, PyObj**, PyObj**) = nullptr;
    PyObj* (*PyList_New)(ssz) = nullptr;
    int (*PyList_SetItem)(PyObj*, ssz, PyObj*) = nullptr;
    ssz (*PyList_Size)(PyObj*) = nullptr;
    PyObj* (*PyList_GetItem)(PyObj*, ssz) = nullptr;

    PyObj* (*PyLong_FromLongLong)(long long) = nullptr;
    long long (*PyLong_AsLongLong)(PyObj*) = nullptr;
    PyObj* (*PyFloat_FromDouble)(double) = nullptr;
    double (*PyFloat_AsDouble)(PyObj*) = nullptr;
    PyObj* (*PyBool_FromLong)(long) = nullptr;
    PyObj* (*PyUnicode_FromStringAndSize)(const char*, ssz) = nullptr;
    const char* (*PyUnicode_AsUTF8AndSize)(PyObj*, ssz*) = nullptr;
    PyObj* (*PyBytes_FromStringAndSize)(const char*, ssz) = nullptr;
    int (*PyBytes_AsStringAndSize)(PyObj*, char**, ssz*) = nullptr;

    PyObj* (*PyErr_Occurred)() = nullptr;
    void (*PyErr_Clear)() = nullptr;
    void (*PyErr_Fetch)(PyObj**, PyObj**, PyObj**) = nullptr;

    // exact-type checks (exported type objects / singletons)
    void* t_long = nullptr;
    void* t_float = nullptr;
    void* t_bool = nullptr;
    void* t_unicode = nullptr;
    void* t_bytes = nullptr;
    void* t_list = nullptr;
    void* t_tuple = nullptr;
    void* t_dict = nullptr;
    PyObj* none = nullptr;
};

PyAPI g_py;

void* open_libpython() {
#ifdef _WIN32
    for (int minor = 14; minor >= 8; minor--) {
        char name[64];
        snprintf(name, sizeof name, "python3%d.dll", minor);
        if (HMODULE h = LoadLibraryA(name)) return (void*)h;
    }
    if (HMODULE h = LoadLibraryA("python3.dll")) return (void*)h;
    return nullptr;
#else
    for (int minor = 14; minor >= 8; minor--) {
        for (const char* fmt : {"libpython3.%d.so.1.0", "libpython3.%d.so"}) {
            char name[64];
            snprintf(name, sizeof name, fmt, minor);
            // RTLD_GLOBAL: extension modules resolve Py* symbols from here
            if (void* h = dlopen(name, RTLD_NOW | RTLD_GLOBAL)) return h;
        }
    }
    // scan common library directories for any libpython3.*
    namespace fs = std::filesystem;
    std::error_code ec;
    for (const char* dir : {"/usr/lib/x86_64-linux-gnu", "/usr/lib/aarch64-linux-gnu",
                            "/usr/lib64", "/usr/lib", "/usr/local/lib"}) {
        if (!fs::is_directory(dir, ec)) continue;
        for (auto& e : fs::directory_iterator(dir, ec)) {
            std::string fn = e.path().filename().string();
            if (fn.rfind("libpython3.", 0) == 0 && fn.find(".so") != std::string::npos)
                if (void* h = dlopen(e.path().c_str(), RTLD_NOW | RTLD_GLOBAL)) return h;
        }
    }
    return nullptr;
#endif
}

void* sym(void* lib, const char* name) {
#ifdef _WIN32
    return (void*)GetProcAddress((HMODULE)lib, name);
#else
    return dlsym(lib, name);
#endif
}

// Lock must be held (Lock ordering: kami runtime lock, then the GIL).
bool py_init() {
    if (g_py.ready) return true;
    static bool failed = false;
    if (failed) return false;
    void* lib = open_libpython();
    if (!lib) {
        failed = true;
        return false;
    }
#define S(f)                                                                     \
    do {                                                                         \
        *(void**)&g_py.f = sym(lib, #f);                                         \
        if (!g_py.f) { failed = true; return false; }                            \
    } while (0)
    S(Py_InitializeEx);
    S(PyEval_SaveThread);
    S(PyGILState_Ensure);
    S(PyGILState_Release);
    S(Py_IncRef);
    S(Py_DecRef);
    S(PyImport_ImportModule);
    S(PyObject_GetAttrString);
    S(PyObject_HasAttrString);
    S(PyObject_CallObject);
    S(PyCallable_Check);
    S(PyObject_Str);
    S(PyObject_IsTrue);
    S(PyTuple_New);
    S(PyTuple_SetItem);
    S(PyTuple_Size);
    S(PyTuple_GetItem);
    S(PyDict_New);
    S(PyDict_SetItem);
    S(PyDict_Next);
    S(PyList_New);
    S(PyList_SetItem);
    S(PyList_Size);
    S(PyList_GetItem);
    S(PyLong_FromLongLong);
    S(PyLong_AsLongLong);
    S(PyFloat_FromDouble);
    S(PyFloat_AsDouble);
    S(PyBool_FromLong);
    S(PyUnicode_FromStringAndSize);
    S(PyUnicode_AsUTF8AndSize);
    S(PyBytes_FromStringAndSize);
    S(PyBytes_AsStringAndSize);
    S(PyErr_Occurred);
    S(PyErr_Clear);
    S(PyErr_Fetch);
#undef S
    g_py.t_long = sym(lib, "PyLong_Type");
    g_py.t_float = sym(lib, "PyFloat_Type");
    g_py.t_bool = sym(lib, "PyBool_Type");
    g_py.t_unicode = sym(lib, "PyUnicode_Type");
    g_py.t_bytes = sym(lib, "PyBytes_Type");
    g_py.t_list = sym(lib, "PyList_Type");
    g_py.t_tuple = sym(lib, "PyTuple_Type");
    g_py.t_dict = sym(lib, "PyDict_Type");
    g_py.none = (PyObj*)sym(lib, "_Py_NoneStruct");
    if (!g_py.t_long || !g_py.none) {
        failed = true;
        return false;
    }
    g_py.Py_InitializeEx(0);   // no signal handlers: kami owns the process
    g_py.PyEval_SaveThread();  // release the GIL; every call re-acquires it
    g_py.ready = true;
    return true;
}

struct GIL {
    int state;
    GIL() { state = g_py.PyGILState_Ensure(); }
    ~GIL() { g_py.PyGILState_Release(state); }
};

std::string py_error_message() {
    PyObj *type = nullptr, *value = nullptr, *tb = nullptr;
    g_py.PyErr_Fetch(&type, &value, &tb);
    std::string msg = "Python error";
    if (value) {
        if (PyObj* s = g_py.PyObject_Str(value)) {
            ssz n = 0;
            if (const char* u = g_py.PyUnicode_AsUTF8AndSize(s, &n))
                msg.assign(u, (size_t)n);
            g_py.Py_DecRef(s);
        }
    }
    if (type) g_py.Py_DecRef(type);
    if (value) g_py.Py_DecRef(value);
    if (tb) g_py.Py_DecRef(tb);
    g_py.PyErr_Clear();
    return msg;
}

// wrap an OWNED PyObject reference as a rooted kami value
void wrap_pyobj(KamiValue* out, PyObj* o) {
    KamiPyObj* w = (KamiPyObj*)gc_alloc(sizeof(KamiPyObj), KT_PYOBJ);
    w->obj = o;
    out->tag = KT_PYOBJ;
    out->p = w;
}

// kami → Python. Returns a NEW reference (GIL held). `as_bytes` converts
// strings to bytes (retry path for APIs that require buffers, e.g. hashing).
PyObj* to_py(const KamiValue* v, bool as_bytes, int depth = 0) {
    if (depth > 16) return nullptr;
    switch (v->tag) {
    case KT_NONE:
        g_py.Py_IncRef(g_py.none);
        return g_py.none;
    case KT_BOOL: return g_py.PyBool_FromLong(v->i ? 1 : 0);
    case KT_INT: return g_py.PyLong_FromLongLong((long long)v->i);
    case KT_FLOAT: return g_py.PyFloat_FromDouble(v->f);
    case KT_STR: {
        KamiStr* s = (KamiStr*)v->p;
        if (as_bytes) return g_py.PyBytes_FromStringAndSize(s->data, (ssz)s->len);
        PyObj* u = g_py.PyUnicode_FromStringAndSize(s->data, (ssz)s->len);
        if (!u) { // not valid UTF-8: it was binary data (b"..." literal)
            g_py.PyErr_Clear();
            u = g_py.PyBytes_FromStringAndSize(s->data, (ssz)s->len);
        }
        return u;
    }
    case KT_LIST: {
        KamiList* l = (KamiList*)v->p;
        PyObj* r = g_py.PyList_New((ssz)l->len);
        if (!r) return nullptr;
        for (int64_t i = 0; i < l->len; i++) {
            PyObj* it = to_py(&l->items[i], as_bytes, depth + 1);
            if (!it) { g_py.Py_DecRef(r); return nullptr; }
            g_py.PyList_SetItem(r, (ssz)i, it); // steals the reference
        }
        return r;
    }
    case KT_MAP: {
        KamiMap* m = (KamiMap*)v->p;
        PyObj* r = g_py.PyDict_New();
        if (!r) return nullptr;
        for (int64_t i = 0; i < m->nentries; i++) {
            if (!m->entries[i].used) continue;
            PyObj* k = to_py(&m->entries[i].key, as_bytes, depth + 1);
            PyObj* val = k ? to_py(&m->entries[i].val, as_bytes, depth + 1) : nullptr;
            if (!k || !val) {
                if (k) g_py.Py_DecRef(k);
                g_py.Py_DecRef(r);
                return nullptr;
            }
            g_py.PyDict_SetItem(r, k, val);
            g_py.Py_DecRef(k);
            g_py.Py_DecRef(val);
        }
        return r;
    }
    case KT_PYOBJ: {
        PyObj* o = (PyObj*)((KamiPyObj*)v->p)->obj;
        g_py.Py_IncRef(o);
        return o;
    }
    default: return nullptr; // unconvertible (function, thread, ...)
    }
}

// Python → kami (GIL held; runtime lock held for allocations). Consumes
// nothing: caller keeps its reference to `o`.
void from_py(KamiValue* out, PyObj* o, int depth = 0) {
    if (o == g_py.none) {
        out->tag = KT_NONE;
        out->i = 0;
        return;
    }
    void* t = o->type;
    if (t == g_py.t_bool) {
        out->tag = KT_BOOL;
        out->i = g_py.PyObject_IsTrue(o) ? 1 : 0;
        return;
    }
    if (t == g_py.t_long) {
        long long v = g_py.PyLong_AsLongLong(o);
        if (g_py.PyErr_Occurred()) { // > 64 bit: fall back to a wrapped object
            g_py.PyErr_Clear();
        } else {
            out->tag = KT_INT;
            out->i = (int64_t)v;
            return;
        }
    }
    if (t == g_py.t_float) {
        out->tag = KT_FLOAT;
        out->f = g_py.PyFloat_AsDouble(o);
        return;
    }
    if (t == g_py.t_unicode) {
        ssz n = 0;
        const char* u = g_py.PyUnicode_AsUTF8AndSize(o, &n);
        if (u) {
            out->tag = KT_STR;
            out->p = str_new(u, (int64_t)n);
            return;
        }
        g_py.PyErr_Clear();
    }
    if (t == g_py.t_bytes) {
        char* buf = nullptr;
        ssz n = 0;
        if (g_py.PyBytes_AsStringAndSize(o, &buf, &n) == 0) {
            out->tag = KT_STR;
            out->p = str_new(buf, (int64_t)n);
            return;
        }
        g_py.PyErr_Clear();
    }
    if (depth <= 8 && (t == g_py.t_list || t == g_py.t_tuple)) {
        bool tup = t == g_py.t_tuple;
        ssz n = tup ? g_py.PyTuple_Size(o) : g_py.PyList_Size(o);
        KamiList* l = list_new(n > 0 ? n : 1);
        out->tag = KT_LIST;
        out->p = l; // root before converting children (their allocs may GC)
        for (ssz i = 0; i < n; i++) {
            PyObj* it = tup ? g_py.PyTuple_GetItem(o, i) : g_py.PyList_GetItem(o, i);
            KamiValue tmp{KT_NONE, {0}};
            l->items[l->len++] = tmp; // grow first: the slot is now GC-visible
            from_py(&l->items[l->len - 1], it, depth + 1);
        }
        return;
    }
    if (depth <= 8 && t == g_py.t_dict) {
        KamiMap* m = map_new();
        out->tag = KT_MAP;
        out->p = m;
        ssz pos = 0;
        PyObj *k = nullptr, *v = nullptr;
        while (g_py.PyDict_Next(o, &pos, &k, &v)) {
            KamiValue kk{KT_NONE, {0}}, vv{KT_NONE, {0}};
            PinGuard pk(&kk, 1), pv(&vv, 1);
            from_py(&kk, k, depth + 1);
            from_py(&vv, v, depth + 1);
            map_set(m, &kk, &vv);
        }
        return;
    }
    // anything else stays a live Python object
    g_py.Py_IncRef(o);
    wrap_pyobj(out, o);
}

// Calls `callable(args...)`, converting arguments and the result. Retries
// once with str→bytes when the callee demands a buffer (e.g. hashlib).
void call_py(KamiValue* out, PyObj* callable, KamiValue** argv, int64_t nargs) {
    for (int attempt = 0; attempt < 2; attempt++) {
        bool as_bytes = attempt == 1;
        PyObj* tup = g_py.PyTuple_New((ssz)nargs);
        if (!tup) panic(py_error_message());
        bool ok = true;
        for (int64_t i = 0; i < nargs; i++) {
            PyObj* a = to_py(argv[i], as_bytes);
            if (!a) {
                ok = false;
                break;
            }
            g_py.PyTuple_SetItem(tup, (ssz)i, a); // steals
        }
        if (!ok) {
            g_py.Py_DecRef(tup);
            panic("cannot convert argument for a CPython call");
        }
        PyObj* res = g_py.PyObject_CallObject(callable, tup);
        g_py.Py_DecRef(tup);
        if (res) {
            from_py(out, res);
            g_py.Py_DecRef(res);
            return;
        }
        std::string msg = py_error_message();
        bool bytes_wanted = msg.find("bytes") != std::string::npos ||
                            msg.find("buffer") != std::string::npos ||
                            msg.find("encoded") != std::string::npos;
        bool has_str = false;
        for (int64_t i = 0; i < nargs; i++)
            if (argv[i]->tag == KT_STR) has_str = true;
        if (attempt == 0 && bytes_wanted && has_str) continue;
        panic(msg);
    }
}

} // namespace

// GC sweep hook: release the Python reference.
void pyobj_finalize(ObjHeader* h) {
    if (!g_py.ready) return;
    GIL gil;
    g_py.Py_DecRef((PyObj*)((KamiPyObj*)h)->obj);
}

void pyobj_attr_get(KamiValue* out, const KamiValue* obj, const char* name) {
    GIL gil;
    PyObj* o = (PyObj*)((KamiPyObj*)obj->p)->obj;
    PyObj* a = g_py.PyObject_GetAttrString(o, name);
    if (!a) panic(py_error_message());
    from_py(out, a);
    g_py.Py_DecRef(a);
}

void pyobj_method(KamiValue* out, KamiValue* obj, const std::string& m, KamiValue** argv,
                  int64_t nargs) {
    GIL gil;
    PyObj* o = (PyObj*)((KamiPyObj*)obj->p)->obj;
    PyObj* fn = g_py.PyObject_GetAttrString(o, m.c_str());
    if (!fn) panic(py_error_message());
    call_py(out, fn, argv, nargs);
    g_py.Py_DecRef(fn);
}

void pyobj_call(KamiValue* out, const KamiValue* fnv, KamiValue** argv, int64_t nargs) {
    GIL gil;
    PyObj* fn = (PyObj*)((KamiPyObj*)fnv->p)->obj;
    call_py(out, fn, argv, nargs);
}

std::string pyobj_str(const KamiValue* v) {
    if (!g_py.ready) return "<pyobject>";
    GIL gil;
    PyObj* o = (PyObj*)((KamiPyObj*)v->p)->obj;
    std::string r = "<pyobject>";
    if (PyObj* s = g_py.PyObject_Str(o)) {
        ssz n = 0;
        if (const char* u = g_py.PyUnicode_AsUTF8AndSize(s, &n)) r.assign(u, (size_t)n);
        g_py.Py_DecRef(s);
    } else {
        g_py.PyErr_Clear();
    }
    return r;
}

int32_t pyobj_truthy(const KamiValue* v) {
    GIL gil;
    return g_py.PyObject_IsTrue((PyObj*)((KamiPyObj*)v->p)->obj) ? 1 : 0;
}

extern "C" {

void kami_pyext_import(KamiValue* out, const char* name) {
    Lock lk(g_lock);
    if (!py_init())
        panic(std::string("cannot import C extension module '") + name +
              "': no CPython shared library (libpython3.x) found on this machine");
    GIL gil;
    PyObj* mod = g_py.PyImport_ImportModule(name);
    if (!mod) panic("ImportError: " + py_error_message());
    wrap_pyobj(out, mod);
}

void kami_pyext_getattr(KamiValue* out, const KamiValue* module, const char* name) {
    Lock lk(g_lock);
    if (module->tag != KT_PYOBJ) panic("pyext: not a CPython module");
    pyobj_attr_get(out, module, name);
}

} // extern "C"

} // namespace kami
