// Filesystem and process bindings for the `os` / `os.path` modules: thin
// wrappers over libc and std::filesystem, i.e. the C-ABI layer that Python code
// cannot express by itself. Everything with any policy in it (json, logging,
// re, requests) is Python and lives in runtime/pylib/.
#include "../include/kami_builtins.h"
#include "rt_internal.h"

#include <cctype>
#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <algorithm>
#include <filesystem>

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h> // GetCurrentProcessId, ...
#include <io.h>
#include <process.h>
#else
#include <sys/wait.h>
#include <unistd.h>
#endif

namespace fs = std::filesystem;

using namespace kami;

// Extended builtins dispatcher — called from the main dispatcher in
// builtins.cpp for ids it does not handle itself.
namespace kami {

bool dispatch_oslayer(std::unique_lock<std::recursive_mutex>& lk, int64_t id, KamiValue* out,
                    KamiValue** argv, int64_t nargs) {
    auto arg_str = [&](int i, const char* fn) -> std::string {
        if (argv[i]->tag != KT_STR) panic(std::string(fn) + "() expected a str");
        KamiStr* s = (KamiStr*)argv[i]->p;
        return std::string(s->data, (size_t)s->len);
    };
    auto set_str = [&](const std::string& s) {
        out->tag = KT_STR;
        out->p = str_new(s.data(), (int64_t)s.size());
    };
    auto set_bool = [&](bool b) {
        out->tag = KT_BOOL;
        out->i = b;
    };
    auto set_none = [&]() {
        out->tag = KT_NONE;
        out->i = 0;
    };
    auto unpin = [&](KamiValue* p) {
        for (size_t i = g_pins.size(); i-- > 0;)
            if (g_pins[i].first == p) { g_pins.erase(g_pins.begin() + (long)i); break; }
    };
    std::error_code ec;
    switch (id) {
    // ---- os ----
    case KB_OS_GETCWD: set_str(fs::current_path(ec).string()); return true;
    case KB_OS_LISTDIR: {
        std::string p = nargs >= 1 ? arg_str(0, "os.listdir") : ".";
        KamiList* r = list_new(8);
        out->tag = KT_LIST;
        out->p = r;
        std::vector<std::string> names;
        for (auto& e : fs::directory_iterator(p, ec)) names.push_back(e.path().filename().string());
        if (ec) panic("os.listdir: cannot open '" + p + "'");
        std::sort(names.begin(), names.end());
        for (auto& n : names) {
            KamiValue v{KT_STR, {0}};
            v.p = str_new(n.data(), (int64_t)n.size());
            list_push(r, &v);
        }
        return true;
    }
    case KB_OS_REMOVE:
        if (!fs::remove(arg_str(0, "os.remove"), ec) || ec)
            panic("os.remove: cannot remove file");
        set_none();
        return true;
    case KB_OS_MKDIR:
        if (!fs::create_directory(arg_str(0, "os.mkdir"), ec) || ec)
            panic("os.mkdir: cannot create directory");
        set_none();
        return true;
    case KB_OS_MAKEDIRS:
        fs::create_directories(arg_str(0, "os.makedirs"), ec);
        if (ec) panic("os.makedirs: cannot create directories");
        set_none();
        return true;
    case KB_OS_RMDIR:
        if (!fs::remove(arg_str(0, "os.rmdir"), ec) || ec) panic("os.rmdir failed");
        set_none();
        return true;
    case KB_OS_RENAME:
        fs::rename(arg_str(0, "os.rename"), arg_str(1, "os.rename"), ec);
        if (ec) panic("os.rename failed");
        set_none();
        return true;
    case KB_OS_SYSTEM: {
        std::string cmd = arg_str(0, "os.system");
        lk.unlock();
        int rc = system(cmd.c_str());
        lk.lock();
        out->tag = KT_INT;
        out->i = rc;
        return true;
    }
    case KB_OS_GETENV: {
        const char* v = getenv(arg_str(0, "os.getenv").c_str());
        if (v) set_str(v);
        else if (nargs == 2) *out = *argv[1];
        else set_none();
        return true;
    }
    case KB_OS_CHDIR:
        fs::current_path(arg_str(0, "os.chdir"), ec);
        if (ec) panic("os.chdir: " + ec.message());
        set_none();
        return true;
    case KB_OS_GETPID:
#ifdef _WIN32
        out->tag = KT_INT;
        out->i = (int64_t)GetCurrentProcessId();
#else
        out->tag = KT_INT;
        out->i = (int64_t)getpid();
#endif
        return true;
    case KB_OS_URANDOM: {
        int64_t n = argv[0]->tag == KT_INT ? argv[0]->i : 0;
        if (n < 0) panic("os.urandom: negative count");
        std::string bytes;
        bytes.resize((size_t)n);
        FILE* f = fopen("/dev/urandom", "rb");
        if (f) {
            size_t got = fread(&bytes[0], 1, (size_t)n, f);
            fclose(f);
            (void)got;
        } else {
            for (int64_t i = 0; i < n; i++) bytes[(size_t)i] = (char)(rand() & 0xff);
        }
        out->tag = KT_STR;
        out->p = str_new(bytes.data(), (int64_t)bytes.size());
        return true;
    }
    case KB_OS_WALK: {
        // Returns a list of (dirpath, [dirnames], [filenames]) triples (as lists).
        std::string root = arg_str(0, "os.walk");
        KamiList* result = list_new(4);
        out->tag = KT_LIST;
        out->p = result;
        std::error_code wec;
        std::vector<std::string> dirs{root};
        while (!dirs.empty()) {
            std::string cur = dirs.front();
            dirs.erase(dirs.begin());
            KamiList* dnames = list_new(4);
            KamiList* fnames = list_new(4);
            for (fs::directory_iterator it(cur, wec), end; !wec && it != end; it.increment(wec)) {
                std::string name = it->path().filename().string();
                KamiValue nv;
                nv.tag = KT_STR;
                nv.p = str_new(name.data(), (int64_t)name.size());
                if (it->is_directory(wec)) {
                    list_push(dnames, &nv);
                    dirs.push_back(it->path().string());
                } else {
                    list_push(fnames, &nv);
                }
            }
            KamiList* triple = list_new(3);
            KamiValue cv;
            cv.tag = KT_STR;
            cv.p = str_new(cur.data(), (int64_t)cur.size());
            list_push(triple, &cv);
            KamiValue dv{KT_LIST, {0}};
            dv.p = dnames;
            list_push(triple, &dv);
            KamiValue fv{KT_LIST, {0}};
            fv.p = fnames;
            list_push(triple, &fv);
            KamiValue tv{KT_LIST, {0}};
            tv.p = triple;
            list_push(result, &tv);
        }
        return true;
    }
    // ---- os.path ----
    case KB_OSP_EXISTS: set_bool(fs::exists(arg_str(0, "os.path.exists"), ec)); return true;
    case KB_OSP_ISFILE:
        set_bool(fs::is_regular_file(arg_str(0, "os.path.isfile"), ec));
        return true;
    case KB_OSP_ISDIR: set_bool(fs::is_directory(arg_str(0, "os.path.isdir"), ec)); return true;
    case KB_OSP_JOIN: {
        fs::path p = arg_str(0, "os.path.join");
        for (int i = 1; i < nargs; i++) p /= arg_str(i, "os.path.join");
        set_str(p.string());
        return true;
    }
    case KB_OSP_BASENAME:
        set_str(fs::path(arg_str(0, "os.path.basename")).filename().string());
        return true;
    case KB_OSP_DIRNAME:
        set_str(fs::path(arg_str(0, "os.path.dirname")).parent_path().string());
        return true;
    case KB_OSP_GETSIZE: {
        auto sz = fs::file_size(arg_str(0, "os.path.getsize"), ec);
        if (ec) panic("os.path.getsize: cannot stat file");
        out->tag = KT_INT;
        out->i = (int64_t)sz;
        return true;
    }
    case KB_OSP_ABSPATH:
        set_str(fs::absolute(arg_str(0, "os.path.abspath"), ec).string());
        return true;
    case KB_OSP_EXPANDUSER: {
        std::string p = arg_str(0, "os.path.expanduser");
        if (!p.empty() && p[0] == '~') {
            const char* home = getenv("HOME");
#ifdef _WIN32
            if (!home) home = getenv("USERPROFILE");
#endif
            if (home) p = std::string(home) + p.substr(1);
        }
        set_str(p);
        return true;
    }
    case KB_OSP_ISABS: {
        std::string p = arg_str(0, "os.path.isabs");
        set_bool(fs::path(p).is_absolute());
        return true;
    }
    case KB_OSP_SPLITEXT: {
        fs::path p = arg_str(0, "os.path.splitext");
        std::string full = p.string();
        std::string ext = p.extension().string();
        std::string root = ext.empty() ? full : full.substr(0, full.size() - ext.size());
        KamiList* r = list_new(2);
        out->tag = KT_LIST;
        out->p = r;
        KamiValue a, b;
        a.tag = KT_STR;
        a.p = str_new(root.data(), (int64_t)root.size());
        list_push(r, &a);
        b.tag = KT_STR;
        b.p = str_new(ext.data(), (int64_t)ext.size());
        list_push(r, &b);
        return true;
    }
    case KB_OSP_SPLIT: {
        fs::path p = arg_str(0, "os.path.split");
        std::string head = p.parent_path().string();
        std::string tail = p.filename().string();
        KamiList* r = list_new(2);
        out->tag = KT_LIST;
        out->p = r;
        KamiValue a, b;
        a.tag = KT_STR;
        a.p = str_new(head.data(), (int64_t)head.size());
        list_push(r, &a);
        b.tag = KT_STR;
        b.p = str_new(tail.data(), (int64_t)tail.size());
        list_push(r, &b);
        return true;
    }
    // ---- socket ----
    case KB_SOCKET_SOCKET:
        if (!socket_create(out)) panic("OSError: socket() failed");
        return true;
    // ---- functional: map / filter (eager; return lists) ----
    case KB_MAP:
    case KB_FILTER: {
        const char* fn = id == KB_MAP ? "map" : "filter";
        if (nargs != 2) panic(std::string(fn) + "() takes exactly 2 arguments");
        KamiValue fnv = *argv[0];
        KamiValue seqv;
        kami_iter_prep(&seqv, argv[1]);
        g_pins.push_back({&seqv, 1});
        KamiList* src = (KamiList*)seqv.p;
        KamiList* r = list_new(src->len);
        out->tag = KT_LIST;
        out->p = r; // rooted
        for (int64_t i = 0; i < src->len; i++) {
            KamiValue res;
            KamiValue* a1[1] = {&src->items[i]};
            if (id == KB_MAP && fnv.tag == KT_NONE) {
                res = src->items[i];
            } else {
                lk.unlock();
                kami_call_value(&res, &fnv, a1, 1);
                lk.lock();
                src = (KamiList*)seqv.p; // re-read (GC may have run)
                r = (KamiList*)out->p;
            }
            if (id == KB_MAP) {
                list_push(r, &res);
            } else {
                if (kami_truthy(&res)) list_push(r, &src->items[i]);
            }
        }
        unpin(&seqv);
        return true;
    }
    case KB_KWARGS_UNSUPPORTED:
        panic("keyword arguments are not supported on this call");
    default:
        return false;
    }
}

} // namespace kami
