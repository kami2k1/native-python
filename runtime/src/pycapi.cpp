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
    int (*PyObject_SetAttrString)(PyObj*, const char*, PyObj*) = nullptr;
    PyObj* (*PyObject_CallObject)(PyObj*, PyObj*) = nullptr;
    PyObj* (*PyObject_Call)(PyObj*, PyObj*, PyObj*) = nullptr;
    PyObj* (*PyObject_GetItem)(PyObj*, PyObj*) = nullptr;
    int (*PyObject_SetItem)(PyObj*, PyObj*, PyObj*) = nullptr;
    PyObj* (*PyObject_GetIter)(PyObj*) = nullptr;
    PyObj* (*PyIter_Next)(PyObj*) = nullptr;
    ssz (*PyObject_Size)(PyObj*) = nullptr;
    void (*PyErr_SetString)(PyObj*, const char*) = nullptr;
    PyObj* (*PyCapsule_New)(void*, const char*, void*) = nullptr;
    void* (*PyCapsule_GetPointer)(PyObj*, const char*) = nullptr;
    PyObj* (*PyCFunction_NewEx)(void*, PyObj*, PyObj*) = nullptr;
    PyObj* (*PyInstanceMethod_New)(PyObj*) = nullptr;
    PyObj* (*PySys_GetObject)(const char*) = nullptr;
    int (*PyList_Append)(PyObj*, PyObj*) = nullptr;
    void* t_type = nullptr; // PyType_Type: callable as type(name, bases, dict)
    void* (*PyEval_SaveThread_p)() = nullptr;  // typed alias (returns PyThreadState*)
    void (*PyEval_RestoreThread)(void*) = nullptr;
    void** exc_runtime_error_addr = nullptr; // deref AFTER Py_InitializeEx
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
    // 1) explicit override
    if (const char* env = getenv("KAMIPY_LIBPYTHON")) {
#ifdef _WIN32
        if (HMODULE h = LoadLibraryA(env)) return (void*)h;
#else
        if (void* h = dlopen(env, RTLD_NOW | RTLD_GLOBAL)) return h;
#endif
    }
#ifdef _WIN32
    // 2) the python.exe on PATH: load the python3XX.dll that sits NEXT TO it,
    // so `import flask` sees the same site-packages as `python app.py`.
    char exe[MAX_PATH];
    if (SearchPathA(nullptr, "python", ".exe", MAX_PATH, exe, nullptr) > 0) {
        std::string dir = exe;
        size_t slash = dir.find_last_of("\\/");
        if (slash != std::string::npos) dir.resize(slash);
        WIN32_FIND_DATAA fd;
        HANDLE find = FindFirstFileA((dir + "\\python3*.dll").c_str(), &fd);
        if (find != INVALID_HANDLE_VALUE) {
            HMODULE best = nullptr;
            do {
                // skip the stub "python3.dll"; prefer the real python3XX.dll
                if (strlen(fd.cFileName) > strlen("python3.dll"))
                    if (HMODULE h = LoadLibraryA((dir + "\\" + fd.cFileName).c_str()))
                        best = h;
            } while (FindNextFileA(find, &fd));
            FindClose(find);
            if (best) return (void*)best;
        }
    }
    for (int minor = 14; minor >= 8; minor--) {
        char name[64];
        snprintf(name, sizeof name, "python3%d.dll", minor);
        if (HMODULE h = LoadLibraryA(name)) return (void*)h;
    }
    if (HMODULE h = LoadLibraryA("python3.dll")) return (void*)h;
    return nullptr;
#else
    // 2) the interpreter `python3` on PATH decides the version: that is the
    // installation whose site-packages the user's pip populated.
    {
        namespace fs = std::filesystem;
        std::error_code ec;
        const char* path = getenv("PATH");
        std::string found;
        if (path) {
            std::string dir;
            for (const char* c = path;; c++) {
                if (*c == ':' || *c == '\0') {
                    for (const char* n : {"python3", "python"}) {
                        fs::path exe = fs::path(dir) / n;
                        if (found.empty() && fs::exists(exe, ec)) {
                            fs::path real = fs::weakly_canonical(exe, ec);
                            std::string fn = real.filename().string(); // python3.11
                            if (fn.rfind("python3.", 0) == 0) found = fn.substr(6);
                        }
                    }
                    dir.clear();
                    if (*c == '\0') break;
                } else {
                    dir += *c;
                }
            }
        }
        if (!found.empty()) { // found = "3.11"
            for (const char* fmt : {"libpython%s.so.1.0", "libpython%s.so"}) {
                char name[64];
                snprintf(name, sizeof name, fmt, found.c_str());
                if (void* h = dlopen(name, RTLD_NOW | RTLD_GLOBAL)) return h;
            }
        }
    }
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
    S(PyObject_SetAttrString);
    S(PyObject_CallObject);
    S(PyObject_Call);
    S(PyObject_GetItem);
    S(PyObject_SetItem);
    S(PyObject_GetIter);
    S(PyIter_Next);
    S(PyObject_Size);
    S(PyErr_SetString);
    S(PyCapsule_New);
    S(PyCapsule_GetPointer);
    S(PyCFunction_NewEx);
    S(PyInstanceMethod_New);
    S(PySys_GetObject);
    S(PyList_Append);
    S(PyEval_RestoreThread);
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
    g_py.t_type = sym(lib, "PyType_Type");
    g_py.none = (PyObj*)sym(lib, "_Py_NoneStruct");
    *(void**)&g_py.PyEval_SaveThread_p = sym(lib, "PyEval_SaveThread");
    g_py.exc_runtime_error_addr = (void**)sym(lib, "PyExc_RuntimeError");
    if (!g_py.t_long || !g_py.none) {
        failed = true;
        return false;
    }
    g_py.Py_InitializeEx(0);   // no signal handlers: kami owns the process
    // Extra package directories (mirrors the compiler's search): make the
    // embedded interpreter see the same site-packages the build resolved.
    if (const char* extra = getenv("KAMIPY_SITE_PACKAGES")) {
        PyObj* path = g_py.PySys_GetObject("path"); // borrowed
        if (path) {
            std::string cur;
#ifdef _WIN32
            const char SEP = ';';
#else
            const char SEP = ':';
#endif
            for (const char* c = extra;; c++) {
                if (*c == SEP || *c == '\0') {
                    if (!cur.empty()) {
                        if (PyObj* s =
                                g_py.PyUnicode_FromStringAndSize(cur.data(), (ssz)cur.size())) {
                            g_py.PyList_Append(path, s);
                            g_py.Py_DecRef(s);
                        }
                        cur.clear();
                    }
                    if (*c == '\0') break;
                } else {
                    cur += *c;
                }
            }
        }
    }
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
PyObj* wrap_kami_callable(const KamiValue* v); // below (needs to_py)
PyObj* to_py(const KamiValue* v, bool as_bytes, int depth = 0, bool as_tuple = false) {
    if (depth > 16) return nullptr;
    switch (v->tag) {
    case KT_NONE:
        g_py.Py_IncRef(g_py.none);
        return g_py.none;
    case KT_FUNC:
        // Compiled functions cross INTO Python as real callables — this is
        // what makes `@app.route("/")` decorators and callbacks work.
        return wrap_kami_callable(v);
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
        // kami has one sequence type; some CPython APIs insist on tuples
        // (PIL colors, dict keys) — the caller retries with as_tuple set.
        KamiList* l = (KamiList*)v->p;
        PyObj* r = as_tuple ? g_py.PyTuple_New((ssz)l->len) : g_py.PyList_New((ssz)l->len);
        if (!r) return nullptr;
        for (int64_t i = 0; i < l->len; i++) {
            PyObj* it = to_py(&l->items[i], as_bytes, depth + 1, as_tuple);
            if (!it) { g_py.Py_DecRef(r); return nullptr; }
            if (as_tuple) g_py.PyTuple_SetItem(r, (ssz)i, it); // steals
            else g_py.PyList_SetItem(r, (ssz)i, it);           // steals
        }
        return r;
    }
    case KT_MAP: {
        KamiMap* m = (KamiMap*)v->p;
        PyObj* r = g_py.PyDict_New();
        if (!r) return nullptr;
        for (int64_t i = 0; i < m->nentries; i++) {
            if (!m->entries[i].used) continue;
            // dict keys must be hashable: kami "tuples" are lists → tuples
            PyObj* k = to_py(&m->entries[i].key, as_bytes, depth + 1, true);
            PyObj* val = k ? to_py(&m->entries[i].val, as_bytes, depth + 1, as_tuple) : nullptr;
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
//
// LOCKING: the caller holds NEITHER the kami runtime lock nor the GIL. The
// process-wide order is "kami lock, then GIL"; the Python call itself runs
// with the GIL only, so a callable that blocks forever (app.run!) does not
// stall every other kami thread.
void call_py(KamiValue* out, PyObj* callable, KamiValue** argv, int64_t nargs,
             KamiMap* kwmap = nullptr) {
    bool as_bytes = false, as_tuple = false;
    for (int attempt = 0; attempt < 3; attempt++) {
        PyObj* tup = nullptr;
        PyObj* kw = nullptr;
        {
            // phase 1: kami → Python argument conversion (reads kami values,
            // creates Python objects: both locks, in order)
            Lock lk(g_lock);
            GIL gil;
            tup = g_py.PyTuple_New((ssz)nargs);
            if (!tup) panic(py_error_message());
            bool ok = true;
            for (int64_t i = 0; i < nargs; i++) {
                PyObj* a = to_py(argv[i], as_bytes, 0, as_tuple);
                if (!a) {
                    ok = false;
                    break;
                }
                g_py.PyTuple_SetItem(tup, (ssz)i, a); // steals
            }
            if (ok && kwmap && kwmap->count > 0) { // keyword arguments → dict
                kw = g_py.PyDict_New();
                for (int64_t i = 0; ok && i < kwmap->nentries; i++) {
                    if (!kwmap->entries[i].used) continue;
                    PyObj* k = to_py(&kwmap->entries[i].key, false);
                    PyObj* val = k ? to_py(&kwmap->entries[i].val, as_bytes, 0, as_tuple)
                                   : nullptr;
                    if (!k || !val) {
                        if (k) g_py.Py_DecRef(k);
                        ok = false;
                        break;
                    }
                    g_py.PyDict_SetItem(kw, k, val);
                    g_py.Py_DecRef(k);
                    g_py.Py_DecRef(val);
                }
            }
            if (!ok) {
                g_py.Py_DecRef(tup);
                if (kw) g_py.Py_DecRef(kw);
                panic("cannot convert argument for a CPython call");
            }
        }
        // phase 2: the actual call — GIL only (Python releases it internally
        // around blocking I/O; other kami threads keep running)
        PyObj* res;
        std::string msg;
        {
            GIL gil;
            res = kw ? g_py.PyObject_Call(callable, tup, kw)
                     : g_py.PyObject_CallObject(callable, tup);
            g_py.Py_DecRef(tup);
            if (kw) g_py.Py_DecRef(kw);
            if (!res) msg = py_error_message();
        }
        if (res) {
            // phase 3: Python → kami result conversion
            Lock lk(g_lock);
            GIL gil;
            from_py(out, res);
            g_py.Py_DecRef(res);
            return;
        }
        bool bytes_wanted = msg.find("bytes") != std::string::npos ||
                            msg.find("buffer") != std::string::npos ||
                            msg.find("encoded") != std::string::npos;
        bool tuple_wanted = msg.find("tuple") != std::string::npos;
        bool has_str = false, has_list = false;
        for (int64_t i = 0; i < nargs; i++) {
            if (argv[i]->tag == KT_STR) has_str = true;
            if (argv[i]->tag == KT_LIST) has_list = true;
        }
        if (!as_bytes && bytes_wanted && has_str) { as_bytes = true; continue; }
        if (!as_tuple && tuple_wanted && has_list) { as_tuple = true; continue; }
        panic(msg);
    }
}

// ---------------------------------------------------------------------------
// Reverse direction: a compiled kami function as a Python callable.
//
// Python (Flask, callbacks, sorted(key=...)) receives a PyCFunction whose
// `self` is a capsule holding a permanently-pinned copy of the KamiValue.
// The trampoline re-establishes the process-wide lock order (kami runtime
// lock BEFORE the GIL) by dropping the GIL first — otherwise a thread holding
// the kami lock while waiting for the GIL could deadlock against us.
// ---------------------------------------------------------------------------

PyObj* kami_py_trampoline(PyObj* capsule, PyObj* args, PyObj* kwargs) {
    // entered from Python: GIL held, no kami lock
    void* ts = g_py.PyEval_SaveThread_p(); // drop the GIL (lock order!)
    KamiValue slots[4]; // fn, positional list, kwargs dict, result
    for (auto& s : slots) s = KamiValue{KT_NONE, {0}};
    PinGuard pin(slots, 4);
    bool convert_ok = true;
    {
        Lock lk(g_lock); // kami lock first ...
        GIL gil;         // ... then the GIL: consistent everywhere
        void* cell = g_py.PyCapsule_GetPointer(capsule, "kami.function");
        if (cell) slots[0] = *(KamiValue*)cell;
        else convert_ok = false;
        if (convert_ok) {
            from_py(&slots[1], args);              // tuple → list
            if (kwargs) from_py(&slots[2], kwargs); // dict → map
        }
    }
    if (slots[2].tag != KT_MAP) { // no kwargs: pass an empty dict
        Lock lk(g_lock);
        slots[2].tag = KT_MAP;
        slots[2].p = map_new();
    }
    std::string err;
    if (convert_ok && slots[0].tag == KT_FUNC && slots[1].tag == KT_LIST) {
        try {
            kami_call_star(&slots[3], &slots[0], &slots[1], &slots[2]);
        } catch (KamiError& e) {
            err = e.msg;
        } catch (std::exception& e) {
            err = e.what();
        }
    } else {
        err = "kami trampoline: bad callable capsule";
    }
    PyObj* res = nullptr;
    {
        Lock lk(g_lock);
        GIL gil;
        if (err.empty()) {
            res = to_py(&slots[3], false);
            if (!res) { // unconvertible result (e.g. a socket): map to None
                g_py.PyErr_Clear();
                g_py.Py_IncRef(g_py.none);
                res = g_py.none;
            }
        } else if (g_py.exc_runtime_error_addr && *g_py.exc_runtime_error_addr) {
            g_py.PyErr_SetString((PyObj*)*g_py.exc_runtime_error_addr, err.c_str());
        }
    }
    g_py.PyEval_RestoreThread(ts); // caller expects the GIL back
    return res; // nullptr + exception set on error
}

// {"kami_function", trampoline, METH_VARARGS | METH_KEYWORDS, doc}
struct PyMethodDefShim {
    const char* ml_name;
    void* ml_meth;
    int ml_flags;
    const char* ml_doc;
};
PyObj* wrap_kami_callable(const KamiValue* v) {
    // Python may hold the callable indefinitely (route tables, callbacks):
    // pin the function object for the life of the process.
    KamiValue* cell = new KamiValue(*v);
    g_pins.push_back({cell, 1});
    g_multithreaded.store(true, std::memory_order_seq_cst); // Python threads may call it
    // Per-wrapper PyMethodDef: frameworks key on the callable's __name__
    // (Flask endpoints), so the compiled function's real name must survive.
    KamiFuncObj* fo = (KamiFuncObj*)v->p;
    const char* name = fo->name && fo->name[0] ? fo->name : "kami_function";
    auto* def = new PyMethodDefShim{strdup(name), (void*)kami_py_trampoline,
                                    0x0001 | 0x0002 /*VARARGS|KEYWORDS*/,
                                    "compiled KamiPython function"};
    PyObj* cap = g_py.PyCapsule_New(cell, "kami.function", nullptr);
    if (!cap) return nullptr;
    PyObj* fn = g_py.PyCFunction_NewEx(def, cap, nullptr);
    g_py.Py_DecRef(cap); // PyCFunction_NewEx holds its own reference to self
    return fn;
}

} // namespace

// GC sweep hook: release the Python reference.
void pyobj_finalize(ObjHeader* h) {
    if (!g_py.ready) return;
    GIL gil;
    g_py.Py_DecRef((PyObj*)((KamiPyObj*)h)->obj);
}

void pyobj_attr_get(KamiValue* out, const KamiValue* obj, const char* name) {
    // caller holds neither lock (properties may run arbitrary Python)
    PyObj* o = (PyObj*)((KamiPyObj*)obj->p)->obj;
    PyObj* a;
    std::string msg;
    {
        GIL gil;
        a = g_py.PyObject_GetAttrString(o, name);
        if (!a) msg = py_error_message();
    }
    if (!a) panic(msg);
    Lock lk(g_lock);
    GIL gil;
    from_py(out, a);
    g_py.Py_DecRef(a);
}

void pyobj_method(KamiValue* out, KamiValue* obj, const std::string& m, KamiValue** argv,
                  int64_t nargs, KamiMap* kwmap) {
    // caller holds neither lock: the method may block forever (app.run)
    PyObj* o = (PyObj*)((KamiPyObj*)obj->p)->obj;
    PyObj* fn;
    std::string msg;
    {
        GIL gil;
        fn = g_py.PyObject_GetAttrString(o, m.c_str());
        if (!fn) msg = py_error_message();
    }
    if (!fn) panic(msg);
    try {
        call_py(out, fn, argv, nargs, kwmap);
    } catch (...) {
        GIL gil;
        g_py.Py_DecRef(fn);
        throw;
    }
    GIL gil;
    g_py.Py_DecRef(fn);
}

void pyobj_call(KamiValue* out, const KamiValue* fnv, KamiValue** argv, int64_t nargs,
                KamiMap* kwmap) {
    PyObj* fn = (PyObj*)((KamiPyObj*)fnv->p)->obj;
    call_py(out, fn, argv, nargs, kwmap);
}

void pyobj_attr_set(const KamiValue* obj, const char* name, const KamiValue* val) {
    PyObj* o = (PyObj*)((KamiPyObj*)obj->p)->obj;
    PyObj* v;
    {
        Lock lk(g_lock);
        GIL gil;
        v = to_py(val, false);
    }
    if (!v) panic("cannot convert value for a CPython attribute assignment");
    std::string msg;
    {
        GIL gil;
        int rc = g_py.PyObject_SetAttrString(o, name, v);
        g_py.Py_DecRef(v);
        if (rc != 0) msg = py_error_message();
    }
    if (!msg.empty()) panic(msg);
}

void pyobj_index_get(KamiValue* out, const KamiValue* obj, const KamiValue* idx) {
    PyObj* o = (PyObj*)((KamiPyObj*)obj->p)->obj;
    PyObj* k;
    {
        Lock lk(g_lock);
        GIL gil;
        k = to_py(idx, false);
    }
    if (!k) panic("cannot convert index for a CPython [] access");
    PyObj* r;
    std::string msg;
    {
        GIL gil;
        r = g_py.PyObject_GetItem(o, k);
        g_py.Py_DecRef(k);
        if (!r) msg = py_error_message();
    }
    if (!r) panic(msg);
    Lock lk(g_lock);
    GIL gil;
    from_py(out, r);
    g_py.Py_DecRef(r);
}

void pyobj_index_set(const KamiValue* obj, const KamiValue* idx, const KamiValue* val) {
    PyObj* o = (PyObj*)((KamiPyObj*)obj->p)->obj;
    PyObj* k = nullptr;
    PyObj* v = nullptr;
    {
        Lock lk(g_lock);
        GIL gil;
        k = to_py(idx, false);
        v = k ? to_py(val, false) : nullptr;
        if (!k || !v) {
            if (k) g_py.Py_DecRef(k);
        }
    }
    if (!k || !v) panic("cannot convert value for a CPython [] assignment");
    std::string msg;
    {
        GIL gil;
        int rc = g_py.PyObject_SetItem(o, k, v);
        g_py.Py_DecRef(k);
        g_py.Py_DecRef(v);
        if (rc != 0) msg = py_error_message();
    }
    if (!msg.empty()) panic(msg);
}

// materialize iter(pyobj) into a kami list (for-loops, list(), ...)
void pyobj_iter_list(KamiValue* out, const KamiValue* obj) {
    PyObj* o = (PyObj*)((KamiPyObj*)obj->p)->obj;
    PyObj* it;
    std::string msg;
    {
        GIL gil;
        it = g_py.PyObject_GetIter(o);
        if (!it) msg = py_error_message();
    }
    if (!it) panic(msg);
    {
        Lock lk(g_lock);
        KamiList* l = list_new(4);
        out->tag = KT_LIST;
        out->p = l; // rooted caller slot
    }
    for (;;) {
        PyObj* item;
        {
            GIL gil; // each step may run Python (generators)
            item = g_py.PyIter_Next(it);
            if (!item && g_py.PyErr_Occurred()) msg = py_error_message();
        }
        if (!item) break;
        Lock lk(g_lock);
        GIL gil;
        KamiValue tmp{KT_NONE, {0}};
        list_push((KamiList*)out->p, &tmp); // grow first: the slot is GC-visible
        from_py(&((KamiList*)out->p)->items[((KamiList*)out->p)->len - 1], item);
        g_py.Py_DecRef(item);
    }
    {
        GIL gil;
        g_py.Py_DecRef(it);
    }
    if (!msg.empty()) panic(msg);
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
    {
        Lock lk(g_lock);
        if (!py_init())
            panic(std::string("cannot import CPython module '") + name +
                  "': no CPython shared library (libpython3.x) found on this machine");
    }
    PyObj* mod;
    std::string msg;
    {
        GIL gil; // imports run arbitrary Python: GIL only
        mod = g_py.PyImport_ImportModule(name);
        if (!mod) msg = py_error_message();
    }
    if (!mod) panic("ImportError: " + msg);
    Lock lk(g_lock);
    GIL gil;
    wrap_pyobj(out, mod);
}

void kami_pyext_getattr(KamiValue* out, const KamiValue* module, const char* name) {
    if (module->tag != KT_PYOBJ) panic("pyext: not a CPython module");
    // `from PIL import Image`: try getattr first, then fall back to importing
    // the submodule — exactly what CPython's from-import machinery does.
    PyObj* o = (PyObj*)((KamiPyObj*)module->p)->obj;
    PyObj* a;
    std::string msg;
    {
        GIL gil;
        a = g_py.PyObject_GetAttrString(o, name);
        if (!a) {
            g_py.PyErr_Clear();
            PyObj* nm = g_py.PyObject_GetAttrString(o, "__name__");
            if (nm) {
                ssz n = 0;
                if (const char* u = g_py.PyUnicode_AsUTF8AndSize(nm, &n)) {
                    std::string dotted(u, (size_t)n);
                    dotted += ".";
                    dotted += name;
                    a = g_py.PyImport_ImportModule(dotted.c_str());
                }
                g_py.Py_DecRef(nm);
            }
            if (!a) {
                msg = py_error_message();
                if (msg.empty() || msg == "Python error")
                    msg = std::string("cannot import name '") + name + "'";
            }
        }
    }
    if (!a) panic("ImportError: " + msg);
    Lock lk(g_lock);
    GIL gil;
    from_py(out, a);
    g_py.Py_DecRef(a);
}

void kami_pyext_subclass(KamiValue* out, const char* name, const KamiValue* base) {
    if (base->tag != KT_PYOBJ) panic("base class is not a CPython class");
    PyObj* basep = (PyObj*)((KamiPyObj*)base->p)->obj;
    PyObj* cls;
    std::string msg;
    {
        GIL gil;
        PyObj* bases = g_py.PyTuple_New(1);
        g_py.Py_IncRef(basep);
        g_py.PyTuple_SetItem(bases, 0, basep); // steals
        PyObj* dict = g_py.PyDict_New();
        PyObj* nm = g_py.PyUnicode_FromStringAndSize(name, (ssz)strlen(name));
        PyObj* args = g_py.PyTuple_New(3);
        g_py.PyTuple_SetItem(args, 0, nm);
        g_py.PyTuple_SetItem(args, 1, bases);
        g_py.PyTuple_SetItem(args, 2, dict);
        cls = g_py.PyObject_CallObject((PyObj*)g_py.t_type, args);
        g_py.Py_DecRef(args);
        if (!cls) msg = py_error_message();
    }
    if (!cls) panic("cannot create subclass '" + std::string(name) + "': " + msg);
    Lock lk(g_lock);
    GIL gil;
    wrap_pyobj(out, cls);
}

void kami_pyext_class_method(KamiValue* cls, const char* name, const KamiValue* fn) {
    if (cls->tag != KT_PYOBJ) panic("kami_pyext_class_method: not a CPython class");
    PyObj* w;
    {
        Lock lk(g_lock); // wrap_kami_callable pins the function object
        GIL gil;
        w = to_py(fn, false);
    }
    if (!w) panic("cannot wrap method for a CPython subclass");
    std::string msg;
    {
        GIL gil;
        // PyCFunctions are not descriptors: PyInstanceMethod makes attribute
        // access bind `self` exactly like a plain Python function would.
        PyObj* im = g_py.PyInstanceMethod_New(w);
        g_py.Py_DecRef(w);
        if (!im) {
            msg = py_error_message();
        } else {
            PyObj* o = (PyObj*)((KamiPyObj*)cls->p)->obj;
            if (g_py.PyObject_SetAttrString(o, name, im) != 0) msg = py_error_message();
            g_py.Py_DecRef(im);
        }
    }
    if (!msg.empty()) panic(msg);
}

} // extern "C"

} // namespace kami
