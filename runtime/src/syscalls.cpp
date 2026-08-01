// C-ABI bindings to the operating system — module `_kami`.
//
// This file is deliberately dumb. Every entry point is a thin forward to a
// standard libc / OS function (open, read, write, close, socket, connect, send,
// recv, stat, ...) that converts KamiValues to C scalars and back. No module
// semantics live here: os, os.path, socket, requests, json, re and logging are
// *Python* source under stdlib/, compiled to native code like any user program,
// and they reach the kernel through these primitives.
//
// Rule of thumb for adding something here: if it cannot be written in Python
// because it needs a struct the language has no way to express (sockaddr,
// struct stat, DIR*), it belongs here. Otherwise it belongs in stdlib/.
#include "../include/kami_builtins.h"
#include "rt_internal.h"

#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <sys/stat.h>
#include <sys/types.h>

#ifdef _WIN32
#include <direct.h>
#include <fcntl.h>
#include <io.h>
#include <process.h>
#include <winsock2.h>
#include <ws2tcpip.h>
#pragma comment(lib, "ws2_32.lib")
using native_sock = SOCKET;
#define CLOSESOCK closesocket
#define KAMI_OPEN ::_open
#define KAMI_READ ::_read
#define KAMI_WRITE ::_write
#define KAMI_CLOSE ::_close
#define KAMI_SEEK ::_lseeki64
#else
#include <arpa/inet.h>
#include <dirent.h>
#include <fcntl.h>
#include <netdb.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <sys/socket.h>
#include <unistd.h>
using native_sock = int;
#define CLOSESOCK ::close
#define KAMI_OPEN ::open
#define KAMI_READ ::read
#define KAMI_WRITE ::write
#define KAMI_CLOSE ::close
#define KAMI_SEEK ::lseek
#endif

namespace kami {

namespace {

std::string arg_str(KamiValue** argv, int i, const char* fn) {
    if (argv[i]->tag != KT_STR) panic(std::string("_kami.") + fn + "() expected a str");
    KamiStr* s = (KamiStr*)argv[i]->p;
    return std::string(s->data, (size_t)s->len);
}

int64_t arg_int(KamiValue** argv, int i, const char* fn) {
    KamiValue* v = argv[i];
    if (v->tag == KT_INT || v->tag == KT_BOOL) return v->i;
    if (v->tag == KT_FLOAT) return (int64_t)v->f;
    panic(std::string("_kami.") + fn + "() expected an int");
}

double arg_num(KamiValue** argv, int i, const char* fn) {
    KamiValue* v = argv[i];
    if (v->tag == KT_INT || v->tag == KT_BOOL) return (double)v->i;
    if (v->tag == KT_FLOAT) return v->f;
    panic(std::string("_kami.") + fn + "() expected a number");
}

void set_int(KamiValue* out, int64_t v) {
    out->tag = KT_INT;
    out->i = v;
}

void set_none(KamiValue* out) {
    out->tag = KT_NONE;
    out->i = 0;
}

void set_str(KamiValue* out, const char* data, size_t len) {
    put_str(out, data, (int64_t)len);
}

// "r", "w", "a", "r+", "w+", "a+" (b/t markers ignored) → open(2) flags.
int open_flags(const std::string& mode) {
    bool plus = mode.find('+') != std::string::npos;
    char base = mode.empty() ? 'r' : mode[0];
    int flags = 0;
#ifdef _WIN32
    flags |= _O_BINARY;
#endif
    switch (base) {
    case 'r': flags |= plus ? O_RDWR : O_RDONLY; break;
    case 'w': flags |= (plus ? O_RDWR : O_WRONLY) | O_CREAT | O_TRUNC; break;
    case 'a': flags |= (plus ? O_RDWR : O_WRONLY) | O_CREAT | O_APPEND; break;
    case 'x': flags |= (plus ? O_RDWR : O_WRONLY) | O_CREAT | O_EXCL; break;
    default: panic("_kami.fd_open(): invalid mode '" + mode + "'");
    }
    return flags;
}

// host:port → a connected/bindable IPv4 address via getaddrinfo(3).
bool resolve(const std::string& host, int port, sockaddr_in& out) {
    memset(&out, 0, sizeof out);
    out.sin_family = AF_INET;
    out.sin_port = htons((uint16_t)port);
    if (host.empty() || host == "0.0.0.0") {
        out.sin_addr.s_addr = INADDR_ANY;
        return true;
    }
    if (inet_pton(AF_INET, host.c_str(), &out.sin_addr) == 1) return true;
    addrinfo hints{};
    hints.ai_family = AF_INET;
    hints.ai_socktype = SOCK_STREAM;
    addrinfo* res = nullptr;
    if (getaddrinfo(host.c_str(), nullptr, &hints, &res) != 0 || !res) return false;
    out.sin_addr = ((sockaddr_in*)res->ai_addr)->sin_addr;
    freeaddrinfo(res);
    return true;
}

void winsock_init() {
#ifdef _WIN32
    static bool done = false;
    if (!done) {
        WSADATA w;
        WSAStartup(MAKEWORD(2, 2), &w);
        done = true;
    }
#endif
}

} // namespace

// Returns false when `id` is not one of ours, so the main dispatcher can report
// an unknown builtin.
bool dispatch_syscall(std::unique_lock<std::recursive_mutex>& lk, int64_t id, KamiValue* out,
                      KamiValue** argv, int64_t nargs) {
    (void)nargs;
    switch (id) {
    // ------------------------------------------------------------ file descriptors
    case KB_SYS_FD_OPEN: {
        std::string path = arg_str(argv, 0, "fd_open");
        std::string mode = arg_str(argv, 1, "fd_open");
        int flags = open_flags(mode);
#ifdef _WIN32
        set_int(out, KAMI_OPEN(path.c_str(), flags, _S_IREAD | _S_IWRITE));
#else
        set_int(out, KAMI_OPEN(path.c_str(), flags, 0666));
#endif
        return true;
    }
    case KB_SYS_FD_READ: {
        int fd = (int)arg_int(argv, 0, "fd_read");
        int64_t want = arg_int(argv, 1, "fd_read");
        std::string data;
        char buf[8192];
        if (want < 0) { // read to EOF
            for (;;) {
                auto n = KAMI_READ(fd, buf, sizeof buf);
                if (n <= 0) break;
                data.append(buf, (size_t)n);
            }
        } else {
            data.resize((size_t)want);
            auto n = want == 0 ? 0 : KAMI_READ(fd, &data[0], (unsigned)want);
            data.resize(n > 0 ? (size_t)n : 0);
        }
        set_str(out, data.data(), data.size());
        return true;
    }
    case KB_SYS_FD_WRITE: {
        int fd = (int)arg_int(argv, 0, "fd_write");
        std::string data = arg_str(argv, 1, "fd_write");
        int64_t done = 0;
        while (done < (int64_t)data.size()) {
            auto n = KAMI_WRITE(fd, data.data() + done, (unsigned)(data.size() - done));
            if (n <= 0) {
                set_int(out, -1);
                return true;
            }
            done += n;
        }
        set_int(out, done);
        return true;
    }
    case KB_SYS_FD_CLOSE: set_int(out, KAMI_CLOSE((int)arg_int(argv, 0, "fd_close"))); return true;
    case KB_SYS_FD_SEEK:
        set_int(out, (int64_t)KAMI_SEEK((int)arg_int(argv, 0, "fd_seek"),
                                       arg_int(argv, 1, "fd_seek"),
                                       (int)arg_int(argv, 2, "fd_seek")));
        return true;
    // ------------------------------------------------------------ file system
    case KB_SYS_STAT: { // -1 missing, 0 regular file, 1 directory, 2 other
        std::string path = arg_str(argv, 0, "stat");
        struct stat st;
        if (::stat(path.c_str(), &st) != 0) {
            set_int(out, -1);
            return true;
        }
        if ((st.st_mode & S_IFMT) == S_IFDIR) set_int(out, 1);
        else if ((st.st_mode & S_IFMT) == S_IFREG) set_int(out, 0);
        else set_int(out, 2);
        return true;
    }
    case KB_SYS_FILESIZE: {
        std::string path = arg_str(argv, 0, "filesize");
        struct stat st;
        set_int(out, ::stat(path.c_str(), &st) == 0 ? (int64_t)st.st_size : -1);
        return true;
    }
    case KB_SYS_LISTDIR: {
        std::string path = arg_str(argv, 0, "listdir");
        KamiList* r = list_new(8);
        out->tag = KT_LIST;
        out->p = r; // rooted before any further allocation
#ifdef _WIN32
        std::string pat = path + "\\*";
        struct _finddata_t fd;
        intptr_t h = _findfirst(pat.c_str(), &fd);
        if (h == -1) {
            set_none(out);
            return true;
        }
        do {
            if (strcmp(fd.name, ".") == 0 || strcmp(fd.name, "..") == 0) continue;
            KamiValue v{KT_STR, {0}};
            v.p = str_new(fd.name, (int64_t)strlen(fd.name));
            list_push((KamiList*)out->p, &v);
        } while (_findnext(h, &fd) == 0);
        _findclose(h);
#else
        DIR* d = opendir(path.c_str());
        if (!d) {
            set_none(out); // Python-level code turns this into an OSError
            return true;
        }
        for (dirent* e = readdir(d); e; e = readdir(d)) {
            if (strcmp(e->d_name, ".") == 0 || strcmp(e->d_name, "..") == 0) continue;
            KamiValue v{KT_STR, {0}};
            v.p = str_new(e->d_name, (int64_t)strlen(e->d_name));
            list_push((KamiList*)out->p, &v);
        }
        closedir(d);
#endif
        return true;
    }
    case KB_SYS_MKDIR: {
        std::string path = arg_str(argv, 0, "mkdir");
#ifdef _WIN32
        set_int(out, ::_mkdir(path.c_str()));
#else
        set_int(out, ::mkdir(path.c_str(), 0777));
#endif
        return true;
    }
    case KB_SYS_RMDIR: {
        std::string path = arg_str(argv, 0, "rmdir");
#ifdef _WIN32
        set_int(out, ::_rmdir(path.c_str()));
#else
        set_int(out, ::rmdir(path.c_str()));
#endif
        return true;
    }
    case KB_SYS_UNLINK: set_int(out, ::remove(arg_str(argv, 0, "unlink").c_str())); return true;
    case KB_SYS_RENAME:
        set_int(out, ::rename(arg_str(argv, 0, "rename").c_str(),
                              arg_str(argv, 1, "rename").c_str()));
        return true;
    case KB_SYS_GETCWD: {
        char buf[4096];
#ifdef _WIN32
        const char* p = ::_getcwd(buf, sizeof buf);
#else
        const char* p = ::getcwd(buf, sizeof buf);
#endif
        if (!p) set_str(out, "", 0);
        else set_str(out, p, strlen(p));
        return true;
    }
    case KB_SYS_CHDIR: {
        std::string path = arg_str(argv, 0, "chdir");
#ifdef _WIN32
        set_int(out, ::_chdir(path.c_str()));
#else
        set_int(out, ::chdir(path.c_str()));
#endif
        return true;
    }
    case KB_SYS_GETENV: {
        const char* v = getenv(arg_str(argv, 0, "getenv").c_str());
        if (v) set_str(out, v, strlen(v));
        else set_none(out);
        return true;
    }
    case KB_SYS_SYSTEM: {
        std::string cmd = arg_str(argv, 0, "system");
        lk.unlock(); // blocking: let other threads run
        int rc = ::system(cmd.c_str());
        lk.lock();
        set_int(out, rc);
        return true;
    }
    case KB_SYS_GETPID:
#ifdef _WIN32
        set_int(out, (int64_t)::_getpid());
#else
        set_int(out, (int64_t)::getpid());
#endif
        return true;
    case KB_SYS_ERRMSG: {
        const char* m = strerror(errno);
        set_str(out, m, strlen(m));
        return true;
    }
    case KB_SYS_PLATFORM:
#ifdef _WIN32
        set_str(out, "win32", 5);
#elif defined(__APPLE__)
        set_str(out, "darwin", 6);
#else
        set_str(out, "linux", 5);
#endif
        return true;
    case KB_SYS_LOCALTIME: { // → [year, month, day, hour, min, sec, wday, yday, isdst]
        time_t t = (time_t)arg_num(argv, 0, "localtime");
        struct tm tmv;
#ifdef _WIN32
        localtime_s(&tmv, &t);
#else
        localtime_r(&t, &tmv);
#endif
        const int64_t f[9] = {tmv.tm_year + 1900,     tmv.tm_mon + 1, tmv.tm_mday,
                              tmv.tm_hour,            tmv.tm_min,     tmv.tm_sec,
                              (tmv.tm_wday + 6) % 7,  tmv.tm_yday + 1, tmv.tm_isdst > 0};
        KamiList* r = list_new(9);
        out->tag = KT_LIST;
        out->p = r;
        for (int64_t x : f) {
            r->items[r->len].tag = KT_INT;
            r->items[r->len].i = x;
            r->len++;
        }
        return true;
    }
    // ------------------------------------------------------------ sockets
    case KB_SOCK_OPEN: {
        winsock_init();
        native_sock fd = ::socket(AF_INET, SOCK_STREAM, 0);
        if ((int64_t)fd < 0) {
            set_int(out, -1);
            return true;
        }
        int one = 1;
        setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, (const char*)&one, sizeof one);
        set_int(out, (int64_t)fd);
        return true;
    }
    case KB_SOCK_CONNECT:
    case KB_SOCK_BIND: {
        const char* fn = id == KB_SOCK_CONNECT ? "sock_connect" : "sock_bind";
        native_sock fd = (native_sock)arg_int(argv, 0, fn);
        std::string host = arg_str(argv, 1, fn);
        int port = (int)arg_int(argv, 2, fn);
        sockaddr_in addr;
        bool resolved;
        int rc;
        lk.unlock(); // DNS + connect can block
        resolved = resolve(host, port, addr);
        if (!resolved) rc = -1;
        else if (id == KB_SOCK_CONNECT) rc = ::connect(fd, (sockaddr*)&addr, sizeof addr);
        else rc = ::bind(fd, (sockaddr*)&addr, sizeof addr);
        lk.lock();
        set_int(out, rc == 0 ? 0 : -1);
        return true;
    }
    case KB_SOCK_LISTEN:
        set_int(out, ::listen((native_sock)arg_int(argv, 0, "sock_listen"),
                             (int)arg_int(argv, 1, "sock_listen")) == 0
                         ? 0
                         : -1);
        return true;
    case KB_SOCK_ACCEPT: { // → [fd, peer_host, peer_port] or None
        native_sock fd = (native_sock)arg_int(argv, 0, "sock_accept");
        sockaddr_in peer{};
        socklen_t plen = sizeof peer;
        lk.unlock(); // blocking
        native_sock cfd = ::accept(fd, (sockaddr*)&peer, &plen);
        lk.lock();
        if ((int64_t)cfd < 0) {
            set_none(out);
            return true;
        }
        KamiList* r = list_new(3);
        out->tag = KT_LIST;
        out->p = r;
        r->len = 3;
        r->items[0].tag = KT_INT;
        r->items[0].i = (int64_t)cfd;
        r->items[1].tag = KT_NONE;
        r->items[2].tag = KT_INT;
        r->items[2].i = ntohs(peer.sin_port);
        char ip[64] = "?";
        inet_ntop(AF_INET, &peer.sin_addr, ip, sizeof ip);
        KamiList* rl = (KamiList*)out->p;
        put_str(&rl->items[1], ip, (int64_t)strlen(ip));
        return true;
    }
    case KB_SOCK_SEND: {
        native_sock fd = (native_sock)arg_int(argv, 0, "sock_send");
        std::string data = arg_str(argv, 1, "sock_send");
        lk.unlock(); // blocking
        int64_t sent = 0;
        while (sent < (int64_t)data.size()) {
            auto n = ::send(fd, data.data() + sent, (int)(data.size() - sent), 0);
            if (n <= 0) break;
            sent += n;
        }
        lk.lock();
        set_int(out, sent == (int64_t)data.size() ? sent : -1);
        return true;
    }
    case KB_SOCK_RECV: {
        native_sock fd = (native_sock)arg_int(argv, 0, "sock_recv");
        int64_t want = arg_int(argv, 1, "sock_recv");
        if (want <= 0) {
            set_str(out, "", 0);
            return true;
        }
        std::string buf;
        buf.resize((size_t)want);
        lk.unlock(); // blocking
        auto n = ::recv(fd, &buf[0], (int)want, 0);
        lk.lock();
        if (n < 0) set_none(out); // Python-level code raises OSError
        else set_str(out, buf.data(), (size_t)n);
        return true;
    }
    case KB_SOCK_CLOSE: set_int(out, CLOSESOCK((native_sock)arg_int(argv, 0, "sock_close")));
        return true;
    case KB_SOCK_TIMEOUT: { // seconds; <= 0 restores blocking mode
        native_sock fd = (native_sock)arg_int(argv, 0, "sock_timeout");
        double secs = arg_num(argv, 1, "sock_timeout");
        if (secs < 0) secs = 0;
#ifdef _WIN32
        DWORD ms = (DWORD)(secs * 1000.0);
        int rc = setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, (const char*)&ms, sizeof ms);
        rc |= setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, (const char*)&ms, sizeof ms);
#else
        timeval tv{};
        tv.tv_sec = (long)secs;
        tv.tv_usec = (long)((secs - (double)tv.tv_sec) * 1e6);
        int rc = setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof tv);
        rc |= setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof tv);
#endif
        set_int(out, rc == 0 ? 0 : -1);
        return true;
    }
    default: return false;
    }
}

} // namespace kami
