// os / os.path, logging, json, socket, requests — the "real world" runtime.
#include "../include/kami_builtins.h"
#include "kami_regex.h"
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
#include <winsock2.h>
#include <ws2tcpip.h>
#include <io.h>
#pragma comment(lib, "ws2_32.lib")
typedef SOCKET native_sock;
#define CLOSESOCK closesocket
#else
#include <arpa/inet.h>
#include <netdb.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <sys/wait.h>
#include <unistd.h>
typedef int native_sock;
#define CLOSESOCK ::close
#endif

namespace fs = std::filesystem;

namespace kami {

// ---------------- internal classes (Response, ...) ----------------
static KamiValue g_internal_roots[4]; // pinned once
static bool g_internal_pinned = false;

KamiClassObj* internal_class(const char* name) {
    if (!g_internal_pinned) {
        for (auto& v : g_internal_roots) v = KamiValue{KT_NONE, {0}};
        g_pins.push_back({g_internal_roots, 4});
        g_internal_pinned = true;
    }
    for (auto& v : g_internal_roots) {
        if (v.tag == KT_CLASS && strcmp(((KamiClassObj*)v.p)->name, name) == 0)
            return (KamiClassObj*)v.p;
    }
    KamiClassObj* c = (KamiClassObj*)gc_alloc(sizeof(KamiClassObj), KT_CLASS);
    c->name = name; // string literal
    c->parent = nullptr;
    c->members = new std::unordered_map<std::string, KamiValue>();
    for (auto& v : g_internal_roots) {
        if (v.tag == KT_NONE) {
            v.tag = KT_CLASS;
            v.p = c;
            return c;
        }
    }
    panic("internal class table full");
}

// ---------------- %-style string formatting ----------------
std::string percent_format(const std::string& fmt, const KamiValue* args, int64_t nargs) {
    std::string out;
    int64_t ai = 0;
    auto next = [&]() -> const KamiValue* {
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
        // parse %[flags][width][.prec]type
        std::string spec;
        size_t j = i + 1;
        while (j < fmt.size() && (isdigit((unsigned char)fmt[j]) || fmt[j] == '-' ||
                                  fmt[j] == '+' || fmt[j] == '.' || fmt[j] == ' ' ||
                                  fmt[j] == '0'))
            spec += fmt[j++];
        if (j >= fmt.size()) panic("incomplete format");
        char t = fmt[j];
        i = j;
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

// ---------------- json ----------------
namespace {
struct JsonParser {
    const std::string& s;
    size_t i = 0;
    explicit JsonParser(const std::string& t) : s(t) {}
    void ws() {
        while (i < s.size() && (s[i] == ' ' || s[i] == '\t' || s[i] == '\n' || s[i] == '\r'))
            i++;
    }
    [[noreturn]] void fail(const std::string& m) {
        panic("json.loads: " + m + " at offset " + std::to_string(i));
    }
    char peek() { return i < s.size() ? s[i] : '\0'; }
    bool lit(const char* w) {
        size_t n = strlen(w);
        if (s.compare(i, n, w) == 0) {
            i += n;
            return true;
        }
        return false;
    }
    // Values are written into rooted storage (list items / map values reachable
    // from the caller's out slot) before any further allocation.
    void parse(KamiValue* out) {
        ws();
        char c = peek();
        if (c == '{') {
            i++;
            kami_make_map(out); // rooted via out
            KamiMap* m = (KamiMap*)out->p;
            ws();
            if (peek() == '}') { i++; return; }
            for (;;) {
                ws();
                if (peek() != '"') fail("expected string key");
                KamiValue k{KT_NONE, {0}}, holder{KT_NONE, {0}};
                parse_string(&k);
                // keep key alive across value allocation: stash in map now
                map_set(m, &k, &holder);
                ws();
                if (peek() != ':') fail("expected ':'");
                i++;
                KamiValue v{KT_NONE, {0}};
                // parse into a temp; k is rooted via the map entry
                parse(&v);
                map_set(m, &k, &v);
                ws();
                if (peek() == ',') { i++; continue; }
                if (peek() == '}') { i++; return; }
                fail("expected ',' or '}'");
            }
        }
        if (c == '[') {
            i++;
            KamiValue lv{KT_NONE, {0}};
            {
                KamiList* l = list_new(4);
                lv.tag = KT_LIST;
                lv.p = l;
            }
            *out = lv; // root the list via out before parsing elements
            ws();
            if (peek() == ']') { i++; return; }
            for (;;) {
                KamiValue v{KT_NONE, {0}};
                KamiValue dummy{KT_NONE, {0}};
                KamiList* l = (KamiList*)out->p;
                list_push(l, &dummy); // reserve rooted slot
                parse(&v);
                ((KamiList*)out->p)->items[((KamiList*)out->p)->len - 1] = v;
                ws();
                if (peek() == ',') { i++; continue; }
                if (peek() == ']') { i++; return; }
                fail("expected ',' or ']'");
            }
        }
        if (c == '"') { parse_string(out); return; }
        if (lit("true")) { out->tag = KT_BOOL; out->i = 1; return; }
        if (lit("false")) { out->tag = KT_BOOL; out->i = 0; return; }
        if (lit("null")) { out->tag = KT_NONE; out->i = 0; return; }
        // number
        size_t start = i;
        if (peek() == '-') i++;
        while (isdigit((unsigned char)peek())) i++;
        bool isf = false;
        if (peek() == '.') {
            isf = true;
            i++;
            while (isdigit((unsigned char)peek())) i++;
        }
        if (peek() == 'e' || peek() == 'E') {
            isf = true;
            i++;
            if (peek() == '+' || peek() == '-') i++;
            while (isdigit((unsigned char)peek())) i++;
        }
        if (i == start || (i == start + 1 && s[start] == '-')) fail("unexpected character");
        std::string num = s.substr(start, i - start);
        if (isf) {
            out->tag = KT_FLOAT;
            out->f = strtod(num.c_str(), nullptr);
        } else {
            out->tag = KT_INT;
            out->i = strtoll(num.c_str(), nullptr, 10);
        }
    }
    void parse_string(KamiValue* out) {
        if (peek() != '"') fail("expected string");
        i++;
        std::string r;
        for (;;) {
            if (i >= s.size()) fail("unterminated string");
            char c = s[i++];
            if (c == '"') break;
            if (c == '\\') {
                if (i >= s.size()) fail("bad escape");
                char e = s[i++];
                switch (e) {
                case 'n': r += '\n'; break;
                case 't': r += '\t'; break;
                case 'r': r += '\r'; break;
                case 'b': r += '\b'; break;
                case 'f': r += '\f'; break;
                case '/': r += '/'; break;
                case '\\': r += '\\'; break;
                case '"': r += '"'; break;
                case 'u': {
                    if (i + 4 > s.size()) fail("bad \\u escape");
                    unsigned cp = (unsigned)strtoul(s.substr(i, 4).c_str(), nullptr, 16);
                    i += 4;
                    // UTF-8 encode (BMP only; surrogate pairs joined best-effort)
                    if (cp >= 0xD800 && cp <= 0xDBFF && i + 6 <= s.size() && s[i] == '\\' &&
                        s[i + 1] == 'u') {
                        unsigned lo = (unsigned)strtoul(s.substr(i + 2, 4).c_str(), nullptr, 16);
                        if (lo >= 0xDC00 && lo <= 0xDFFF) {
                            cp = 0x10000 + ((cp - 0xD800) << 10) + (lo - 0xDC00);
                            i += 6;
                        }
                    }
                    if (cp < 0x80) r += (char)cp;
                    else if (cp < 0x800) {
                        r += (char)(0xC0 | (cp >> 6));
                        r += (char)(0x80 | (cp & 0x3F));
                    } else if (cp < 0x10000) {
                        r += (char)(0xE0 | (cp >> 12));
                        r += (char)(0x80 | ((cp >> 6) & 0x3F));
                        r += (char)(0x80 | (cp & 0x3F));
                    } else {
                        r += (char)(0xF0 | (cp >> 18));
                        r += (char)(0x80 | ((cp >> 12) & 0x3F));
                        r += (char)(0x80 | ((cp >> 6) & 0x3F));
                        r += (char)(0x80 | (cp & 0x3F));
                    }
                    break;
                }
                default: fail("bad escape");
                }
            } else {
                r += c;
            }
        }
        out->tag = KT_STR;
        out->p = str_new(r.data(), (int64_t)r.size());
    }
};
} // namespace

void json_loads(KamiValue* out, const std::string& text) {
    JsonParser p(text);
    p.parse(out);
    p.ws();
    if (p.i != text.size()) p.fail("trailing data");
}

static void json_escape(std::string& out, const std::string& s) {
    out += '"';
    for (unsigned char c : s) {
        switch (c) {
        case '"': out += "\\\""; break;
        case '\\': out += "\\\\"; break;
        case '\n': out += "\\n"; break;
        case '\t': out += "\\t"; break;
        case '\r': out += "\\r"; break;
        default:
            if (c < 0x20) {
                char buf[8];
                snprintf(buf, sizeof buf, "\\u%04x", c);
                out += buf;
            } else {
                out += (char)c;
            }
        }
    }
    out += '"';
}

std::string json_dumps(const KamiValue* v) {
    switch (v->tag) {
    case KT_NONE: return "null";
    case KT_BOOL: return v->i ? "true" : "false";
    case KT_INT: return std::to_string(v->i);
    case KT_FLOAT: return format_float(v->f);
    case KT_STR: {
        KamiStr* s = (KamiStr*)v->p;
        std::string out;
        json_escape(out, std::string(s->data, (size_t)s->len));
        return out;
    }
    case KT_LIST: {
        KamiList* l = (KamiList*)v->p;
        std::string out = "[";
        for (int64_t i = 0; i < l->len; i++) {
            if (i) out += ", ";
            out += json_dumps(&l->items[i]);
        }
        return out + "]";
    }
    case KT_MAP: {
        KamiMap* m = (KamiMap*)v->p;
        std::string out = "{";
        bool first = true;
        for (int64_t i = 0; i < m->cap; i++) {
            if (!m->entries[i].used) continue;
            if (!first) out += ", ";
            first = false;
            if (m->entries[i].key.tag != KT_STR)
                panic("json.dumps: dict keys must be strings");
            KamiStr* k = (KamiStr*)m->entries[i].key.p;
            json_escape(out, std::string(k->data, (size_t)k->len));
            out += ": ";
            out += json_dumps(&m->entries[i].val);
        }
        return out + "}";
    }
    default:
        panic(std::string("json.dumps: cannot serialize '") + type_name(v->tag) + "'");
    }
}

// ---------------- logging ----------------
static int64_t g_log_level = 30; // WARNING (Python default)
static std::string g_log_format = "%(levelname)s:%(name)s:%(message)s";

static const char* level_name(int64_t lv) {
    if (lv >= 50) return "CRITICAL";
    if (lv >= 40) return "ERROR";
    if (lv >= 30) return "WARNING";
    if (lv >= 20) return "INFO";
    return "DEBUG";
}

static void log_emit(int64_t level, const std::string& msg) {
    if (level < g_log_level) return;
    std::string out;
    const std::string& f = g_log_format;
    for (size_t i = 0; i < f.size(); i++) {
        if (f[i] == '%' && i + 1 < f.size() && f[i + 1] == '(') {
            size_t close = f.find(')', i);
            if (close != std::string::npos && close + 1 < f.size() && f[close + 1] == 's') {
                std::string field = f.substr(i + 2, close - i - 2);
                if (field == "message") out += msg;
                else if (field == "levelname") out += level_name(level);
                else if (field == "name") out += "root";
                else if (field == "asctime") {
                    time_t t = time(nullptr);
                    struct tm tmv;
#ifdef _WIN32
                    localtime_s(&tmv, &t);
#else
                    localtime_r(&t, &tmv);
#endif
                    char buf[64];
                    strftime(buf, sizeof buf, "%Y-%m-%d %H:%M:%S", &tmv);
                    out += buf;
                } else {
                    out += "?";
                }
                i = close + 1;
                continue;
            }
        }
        out += f[i];
    }
    out += "\n";
    fflush(stdout);
    fwrite(out.data(), 1, out.size(), stderr);
    fflush(stderr);
}

// ---------------- sockets ----------------
static void sock_startup() {
#ifdef _WIN32
    static bool done = false;
    if (!done) {
        WSADATA w;
        WSAStartup(MAKEWORD(2, 2), &w);
        done = true;
    }
#endif
}

static KamiSocket* sock_new(int64_t fd) {
    KamiSocket* s = (KamiSocket*)gc_alloc(sizeof(KamiSocket), KT_SOCKET);
    s->fd = fd;
    s->closed = false;
    s->timeout = -1.0;
    return s;
}

// ---------------- subprocess capture (for requests via curl) ----------------
#ifndef _WIN32
static int capture_process(const std::vector<std::string>& args, std::string& out) {
    int fds[2];
    if (pipe(fds) != 0) return -1;
    pid_t pid = fork();
    if (pid < 0) return -1;
    if (pid == 0) {
        dup2(fds[1], 1);
        ::close(fds[0]);
        ::close(fds[1]);
        std::vector<char*> argv;
        for (auto& a : args) argv.push_back(const_cast<char*>(a.c_str()));
        argv.push_back(nullptr);
        execvp(argv[0], argv.data());
        _exit(127);
    }
    ::close(fds[1]);
    char buf[4096];
    ssize_t n;
    while ((n = read(fds[0], buf, sizeof buf)) > 0) out.append(buf, (size_t)n);
    ::close(fds[0]);
    int status = 0;
    waitpid(pid, &status, 0);
    return WIFEXITED(status) ? WEXITSTATUS(status) : 128;
}
#else
static int capture_process(const std::vector<std::string>& args, std::string& out) {
    std::string cmd;
    for (auto& a : args) {
        if (!cmd.empty()) cmd += " ";
        if (a.find_first_of(" \t\"") != std::string::npos) {
            cmd += "\"";
            for (char c : a) {
                if (c == '"') cmd += "\\\"";
                else cmd += c;
            }
            cmd += "\"";
        } else {
            cmd += a;
        }
    }
    FILE* p = _popen(cmd.c_str(), "rb");
    if (!p) return -1;
    char buf[4096];
    size_t n;
    while ((n = fread(buf, 1, sizeof buf, p)) > 0) out.append(buf, n);
    return _pclose(p);
}
#endif

} // namespace kami

using namespace kami;

// Extended builtins dispatcher — called from the main dispatcher in
// builtins.cpp for ids it does not handle itself.
namespace kami {

bool dispatch_netio(std::unique_lock<std::recursive_mutex>& lk, int64_t id, KamiValue* out,
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
    // Build a Match object (internal class) holding the text and group spans.
    auto make_match = [&](KamiValue* o, const std::string& text, const std::vector<int>& gs,
                          const std::vector<int>& ge) {
        KamiClassObj* cls = internal_class("Match");
        KamiInstance* inst = (KamiInstance*)gc_alloc(sizeof(KamiInstance), KT_OBJECT);
        inst->cls = cls;
        inst->fields = new std::unordered_map<std::string, KamiValue>();
        o->tag = KT_OBJECT;
        o->p = inst; // rooted
        // store groups as a list of [start, end, text] triples
        KamiList* groups = list_new((int64_t)gs.size());
        for (size_t g = 0; g < gs.size(); g++) {
            KamiList* tri = list_new(3);
            tri->items[0].tag = KT_INT; tri->items[0].i = gs[g];
            tri->items[1].tag = KT_INT; tri->items[1].i = ge[g];
            tri->items[2].tag = KT_STR;
            tri->items[2].p = gs[g] < 0 ? str_new("", 0)
                                        : str_new(text.data() + gs[g], ge[g] - gs[g]);
            tri->len = 3;
            KamiValue tv{KT_LIST, {0}};
            tv.p = tri;
            groups->items[groups->len++] = tv;
        }
        KamiValue gv{KT_LIST, {0}};
        gv.p = groups;
        (*inst->fields)["__groups__"] = gv;
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
    // ---- logging ----
    case KB_LOG_BASICCONFIG: {
        // args (from sema): [level|None, format|None]
        if (nargs >= 1 && argv[0]->tag != KT_NONE) {
            if (argv[0]->tag == KT_INT || argv[0]->tag == KT_BOOL) g_log_level = argv[0]->i;
            else if (argv[0]->tag == KT_FLOAT) g_log_level = (int64_t)argv[0]->f;
            else panic("logging.basicConfig: level must be an int");
        }
        if (nargs >= 2 && argv[1]->tag != KT_NONE) {
            if (argv[1]->tag != KT_STR) panic("logging.basicConfig: format must be a str");
            KamiStr* s = (KamiStr*)argv[1]->p;
            g_log_format.assign(s->data, (size_t)s->len);
        }
        set_none();
        return true;
    }
    case KB_LOG_LOG: {
        // args: [level, msg, fmt-args...]
        if (nargs < 2) panic("logging: missing message");
        int64_t level = argv[0]->i;
        std::string msg = value_str(argv[1]);
        if (nargs > 2) {
            std::vector<KamiValue> rest;
            for (int64_t i = 2; i < nargs; i++) rest.push_back(*argv[i]);
            msg = percent_format(msg, rest.data(), (int64_t)rest.size());
        }
        log_emit(level, msg);
        set_none();
        return true;
    }
    // ---- json ----
    case KB_JSON_LOADS: json_loads(out, arg_str(0, "json.loads")); return true;
    case KB_JSON_DUMPS: set_str(json_dumps(argv[0])); return true;
    // ---- socket ----
    case KB_SOCKET_SOCKET: {
        sock_startup();
        native_sock fd = ::socket(AF_INET, SOCK_STREAM, 0);
        if ((int64_t)fd < 0) panic("socket(): cannot create socket");
        int one = 1;
        setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, (const char*)&one, sizeof one);
        KamiSocket* s = sock_new((int64_t)fd);
        out->tag = KT_SOCKET;
        out->p = s;
        return true;
    }
    // ---- requests (via curl: supports http + https) ----
    case KB_REQUESTS_GET:
    case KB_REQUESTS_POST: {
        std::string url = arg_str(0, "requests");
        // -s (no progress) without -S: connection errors surface as a Python
        // RequestException, not as stray curl noise on stderr.
        std::vector<std::string> cmd = {"curl", "-s", "-w",
                                        "\n__KAMI_HTTP_STATUS__:%{http_code}"};
        double timeout = 0;
        if (id == KB_REQUESTS_GET) {
            if (nargs >= 2 && argv[1]->tag != KT_NONE) {
                if (argv[1]->tag == KT_FLOAT) timeout = argv[1]->f;
                else if (argv[1]->tag == KT_INT) timeout = (double)argv[1]->i;
            }
        } else {
            if (nargs >= 2 && argv[1]->tag != KT_NONE) { // data or json payload
                std::string body;
                if (argv[1]->tag == KT_STR) {
                    KamiStr* s = (KamiStr*)argv[1]->p;
                    body.assign(s->data, (size_t)s->len);
                } else {
                    body = json_dumps(argv[1]);
                    cmd.push_back("-H");
                    cmd.push_back("Content-Type: application/json");
                }
                cmd.push_back("--data-binary");
                cmd.push_back(body);
                cmd.push_back("-X");
                cmd.push_back("POST");
            } else {
                cmd.push_back("-X");
                cmd.push_back("POST");
            }
            if (nargs >= 3 && argv[2]->tag != KT_NONE) {
                if (argv[2]->tag == KT_FLOAT) timeout = argv[2]->f;
                else if (argv[2]->tag == KT_INT) timeout = (double)argv[2]->i;
            }
        }
        if (timeout > 0) {
            cmd.push_back("--max-time");
            char buf[32];
            snprintf(buf, sizeof buf, "%.3f", timeout);
            cmd.push_back(buf);
        }
        cmd.push_back(url);
        std::string raw;
        lk.unlock();
        int rc = capture_process(cmd, raw);
        lk.lock();
        size_t mark = raw.rfind("\n__KAMI_HTTP_STATUS__:");
        if (rc != 0 || mark == std::string::npos)
            panic("RequestException: request to '" + url + "' failed (curl exit " +
                  std::to_string(rc) + ")");
        std::string body = raw.substr(0, mark);
        long code = strtol(raw.c_str() + mark + strlen("\n__KAMI_HTTP_STATUS__:"), nullptr, 10);
        // build Response object
        KamiClassObj* cls = internal_class("Response");
        KamiInstance* inst = (KamiInstance*)gc_alloc(sizeof(KamiInstance), KT_OBJECT);
        inst->cls = cls;
        inst->fields = new std::unordered_map<std::string, KamiValue>();
        out->tag = KT_OBJECT;
        out->p = inst; // rooted via out
        KamiValue v;
        v.tag = KT_INT;
        v.i = code;
        (*inst->fields)["status_code"] = v;
        v.tag = KT_BOOL;
        v.i = code >= 200 && code < 400;
        (*inst->fields)["ok"] = v;
        v.tag = KT_STR;
        v.p = str_new(body.data(), (int64_t)body.size());
        (*inst->fields)["text"] = v;
        return true;
    }
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
    // ---- re ----
    case KB_RE_MATCH:
    case KB_RE_SEARCH:
    case KB_RE_FULLMATCH: {
        std::string pat = arg_str(0, "re");
        std::string text = arg_str(1, "re");
        re::Regex rx(pat);
        if (!rx.ok()) panic("re.error: " + rx.err());
        std::vector<int> gs, ge;
        bool matched = false;
        int base = 0;
        if (id == KB_RE_SEARCH) {
            int at = rx.search(text, 0, gs, ge);
            matched = at >= 0;
        } else {
            matched = rx.matchAt(text, 0, gs, ge, id == KB_RE_FULLMATCH);
        }
        (void)base;
        if (!matched) { set_none(); return true; }
        make_match(out, text, gs, ge);
        return true;
    }
    case KB_RE_FINDALL: {
        std::string pat = arg_str(0, "re.findall");
        std::string text = arg_str(1, "re.findall");
        re::Regex rx(pat);
        if (!rx.ok()) panic("re.error: " + rx.err());
        KamiList* r = list_new(4);
        out->tag = KT_LIST;
        out->p = r;
        std::vector<int> gs, ge;
        int from = 0;
        int n = (int)text.size();
        while (from <= n) {
            int at = rx.search(text, from, gs, ge);
            if (at < 0) break;
            // findall returns group(1) if one group, tuple of groups if many,
            // else whole match — matching Python semantics.
            KamiValue v;
            int ng = rx.group_count();
            auto sub = [&](int g) {
                if (gs[g] < 0) return str_new("", 0);
                return str_new(text.data() + gs[g], ge[g] - gs[g]);
            };
            if (ng == 0) {
                v.tag = KT_STR;
                v.p = sub(0);
            } else if (ng == 1) {
                v.tag = KT_STR;
                v.p = sub(1);
            } else {
                KamiList* tup = list_new(ng);
                for (int g = 1; g <= ng; g++) {
                    KamiValue sv{KT_STR, {0}};
                    sv.p = sub(g);
                    tup->items[tup->len++] = sv;
                }
                v.tag = KT_LIST;
                v.p = tup;
            }
            r = (KamiList*)out->p;
            list_push(r, &v);
            int me = ge[0];
            from = (me == at) ? me + 1 : me; // advance past empty matches
        }
        return true;
    }
    case KB_RE_SUB: {
        std::string pat = arg_str(0, "re.sub");
        std::string repl = arg_str(1, "re.sub");
        std::string text = arg_str(2, "re.sub");
        int count = 0;
        if (nargs >= 4 && argv[3]->tag == KT_INT) count = (int)argv[3]->i;
        re::Regex rx(pat);
        if (!rx.ok()) panic("re.error: " + rx.err());
        std::string result;
        std::vector<int> gs, ge;
        int from = 0, done = 0;
        int n = (int)text.size();
        while (from <= n) {
            int at = rx.search(text, from, gs, ge);
            if (at < 0) break;
            result.append(text, (size_t)from, (size_t)(at - from));
            // expand backreferences \1..\9 and \g<n>
            for (size_t i = 0; i < repl.size(); i++) {
                if (repl[i] == '\\' && i + 1 < repl.size()) {
                    char c = repl[i + 1];
                    if (isdigit((unsigned char)c)) {
                        int g = c - '0';
                        if (g <= rx.group_count() && gs[g] >= 0)
                            result.append(text, (size_t)gs[g], (size_t)(ge[g] - gs[g]));
                        i++;
                        continue;
                    }
                    if (c == 'n') { result += '\n'; i++; continue; }
                    if (c == 't') { result += '\t'; i++; continue; }
                    if (c == '\\') { result += '\\'; i++; continue; }
                }
                result += repl[i];
            }
            int me = ge[0];
            if (me == at) { // empty match: emit one char, advance
                if (at < n) result += text[at];
                from = at + 1;
            } else {
                from = me;
            }
            done++;
            if (count > 0 && done >= count) break;
        }
        if (from < n) result.append(text, (size_t)from, (size_t)(n - from));
        out->tag = KT_STR;
        out->p = str_new(result.data(), (int64_t)result.size());
        return true;
    }
    case KB_RE_SPLIT: {
        std::string pat = arg_str(0, "re.split");
        std::string text = arg_str(1, "re.split");
        re::Regex rx(pat);
        if (!rx.ok()) panic("re.error: " + rx.err());
        KamiList* r = list_new(4);
        out->tag = KT_LIST;
        out->p = r;
        std::vector<int> gs, ge;
        int from = 0, last = 0, n = (int)text.size();
        while (from <= n) {
            int at = rx.search(text, from, gs, ge);
            if (at < 0) break;
            if (ge[0] == at) { // empty match
                from = at + 1;
                if (from > n) break;
                continue;
            }
            KamiValue v{KT_STR, {0}};
            v.p = str_new(text.data() + last, at - last);
            r = (KamiList*)out->p;
            list_push(r, &v);
            last = ge[0];
            from = ge[0];
        }
        KamiValue tail{KT_STR, {0}};
        tail.p = str_new(text.data() + last, n - last);
        r = (KamiList*)out->p;
        list_push(r, &tail);
        return true;
    }
    case KB_KWARGS_UNSUPPORTED:
        panic("keyword arguments are not supported on this call");
    default:
        return false;
    }
}

// socket methods, dispatched from kami_method for KT_SOCKET values.
void socket_method(std::unique_lock<std::recursive_mutex>& lk, KamiValue* out, KamiValue* obj,
                   const std::string& m, KamiValue** argv, int64_t nargs) {
    KamiSocket* s = (KamiSocket*)obj->p;
    if (m != "close" && s->closed) panic("socket is closed");
    native_sock fd = (native_sock)s->fd;
    auto host_port = [&](const KamiValue* v, std::string& host, int& port) {
        if (v->tag != KT_LIST || ((KamiList*)v->p)->len != 2)
            panic("expected (host, port) tuple");
        KamiList* l = (KamiList*)v->p;
        if (l->items[0].tag != KT_STR || l->items[1].tag != KT_INT)
            panic("expected (host:str, port:int)");
        KamiStr* h = (KamiStr*)l->items[0].p;
        host.assign(h->data, (size_t)h->len);
        port = (int)l->items[1].i;
    };
    auto set_none = [&]() {
        out->tag = KT_NONE;
        out->i = 0;
    };
    if (m == "bind" && nargs == 1) {
        std::string host;
        int port;
        host_port(argv[0], host, port);
        sockaddr_in addr{};
        addr.sin_family = AF_INET;
        addr.sin_port = htons((uint16_t)port);
        if (host.empty() || host == "0.0.0.0") addr.sin_addr.s_addr = INADDR_ANY;
        else inet_pton(AF_INET, host.c_str(), &addr.sin_addr);
        if (::bind(fd, (sockaddr*)&addr, sizeof addr) != 0)
            panic("OSError: bind failed on port " + std::to_string(port));
        set_none();
        return;
    }
    if (m == "listen" && nargs <= 1) {
        int backlog = nargs == 1 && argv[0]->tag == KT_INT ? (int)argv[0]->i : 8;
        if (::listen(fd, backlog) != 0) panic("OSError: listen failed");
        set_none();
        return;
    }
    if (m == "accept" && nargs == 0) {
        sockaddr_in peer{};
        socklen_t plen = sizeof peer;
        lk.unlock(); // blocking: release the GIL
        native_sock cfd = ::accept(fd, (sockaddr*)&peer, &plen);
        lk.lock();
        if ((int64_t)cfd < 0) panic("OSError: accept failed");
        // result: [conn_socket, [host, port]]
        KamiList* r = list_new(2);
        out->tag = KT_LIST;
        out->p = r; // root before more allocations
        r->len = 2;
        r->items[0].tag = KT_NONE;
        r->items[1].tag = KT_NONE;
        KamiSocket* cs = sock_new((int64_t)cfd);
        r->items[0].tag = KT_SOCKET;
        r->items[0].p = cs;
        KamiList* addr = list_new(2);
        r->items[1].tag = KT_LIST;
        r->items[1].p = addr;
        addr->len = 2;
        char ip[64] = "?";
        inet_ntop(AF_INET, &peer.sin_addr, ip, sizeof ip);
        addr->items[0].tag = KT_STR;
        addr->items[0].p = str_new(ip, (int64_t)strlen(ip));
        addr->items[1].tag = KT_INT;
        addr->items[1].i = ntohs(peer.sin_port);
        return;
    }
    if (m == "connect" && nargs == 1) {
        std::string host;
        int port;
        host_port(argv[0], host, port);
        addrinfo hints{}, *res = nullptr;
        hints.ai_family = AF_INET;
        hints.ai_socktype = SOCK_STREAM;
        lk.unlock();
        int rc = getaddrinfo(host.c_str(), std::to_string(port).c_str(), &hints, &res);
        int crc = -1;
        if (rc == 0 && res) {
            crc = ::connect(fd, res->ai_addr, (socklen_t)res->ai_addrlen);
            freeaddrinfo(res);
        }
        lk.lock();
        if (crc != 0)
            panic("ConnectionError: cannot connect to " + host + ":" + std::to_string(port));
        set_none();
        return;
    }
    if ((m == "send" || m == "sendall") && nargs == 1) {
        if (argv[0]->tag != KT_STR) panic("socket.send() expects a str");
        KamiStr* d = (KamiStr*)argv[0]->p;
        std::string data(d->data, (size_t)d->len);
        lk.unlock();
        int64_t sent = 0;
        while (sent < (int64_t)data.size()) {
            auto n = ::send(fd, data.data() + sent, (size_t)(data.size() - sent), 0);
            if (n <= 0) break;
            sent += n;
        }
        lk.lock();
        if (sent < (int64_t)data.size()) panic("OSError: send failed");
        out->tag = KT_INT;
        out->i = sent;
        return;
    }
    if (m == "recv" && nargs == 1) {
        if (argv[0]->tag != KT_INT) panic("socket.recv() expects a size");
        std::vector<char> buf((size_t)argv[0]->i);
        lk.unlock();
        auto n = ::recv(fd, buf.data(), buf.size(), 0);
        lk.lock();
        if (n < 0) panic("OSError: recv failed");
        out->tag = KT_STR;
        out->p = str_new(buf.data(), (int64_t)n);
        return;
    }
    if (m == "settimeout" && nargs == 1) {
        // accepted; blocking semantics retained (documented limitation)
        set_none();
        return;
    }
    if (m == "close" && nargs == 0) {
        if (!s->closed) {
            CLOSESOCK(fd);
            s->closed = true;
        }
        set_none();
        return;
    }
    panic("'socket' object has no method '" + m + "' with " + std::to_string(nargs) + " args");
}

} // namespace kami
