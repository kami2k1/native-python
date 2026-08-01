// Sockets and TLS: C-ABI bindings to the operating system, nothing more.
//
// This file deliberately contains no protocol logic. Everything above the
// syscall level — HTTP, JSON, logging, regular expressions — is written in
// Python and lives in runtime/pylib/, where the compiler translates it like any
// other program. What must stay in C is the part that cannot be expressed as
// Python at all: the actual system calls (socket/bind/listen/accept/connect/
// send/recv/close) and the platform TLS provider (OpenSSL on POSIX, SChannel on
// Windows), which `socket.wrap_tls()` exposes as a stream.
#include "../include/kami_builtins.h"
#include "rt_internal.h"

#include <cerrno>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#define SECURITY_WIN32
#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>
#include <security.h>
#include <schannel.h>
#pragma comment(lib, "ws2_32.lib")
#pragma comment(lib, "secur32.lib")
#pragma comment(lib, "crypt32.lib")
typedef SOCKET native_sock;
#define CLOSESOCK closesocket
#else
#include <arpa/inet.h>
#include <netdb.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <unistd.h>
typedef int native_sock;
#define CLOSESOCK ::close
#endif

#ifdef KAMI_HAVE_OPENSSL
#include <openssl/err.h>
#include <openssl/ssl.h>
#endif

namespace kami {

// ---------------------------------------------------------------- TLS stream
// A tiny transport interface over the platform's TLS library. The Python
// `requests` module speaks HTTP through it and never sees these details.
struct TlsConn {
    native_sock fd = (native_sock)-1;
#ifdef KAMI_HAVE_OPENSSL
    SSL_CTX* ctx = nullptr;
    SSL* ssl = nullptr;
#endif
#ifdef _WIN32
    CredHandle cred{};
    CtxtHandle ctxt{};
    bool have_cred = false;
    bool have_ctxt = false;
    SecPkgContext_StreamSizes sizes{};
    std::string incoming;  // raw bytes read but not yet decrypted
    std::string plaintext; // decrypted bytes not yet handed to the caller
#endif
};

#if defined(_WIN32)

// ---- Windows: SChannel (the OS TLS stack, same one WinHTTP and Edge use) ----
static bool sch_recv_raw(TlsConn* c, std::string& err) {
    char buf[16384];
    int n = ::recv(c->fd, buf, (int)sizeof buf, 0);
    if (n <= 0) {
        err = "TLS: connection closed during handshake";
        return false;
    }
    c->incoming.append(buf, (size_t)n);
    return true;
}

static bool sch_send_raw(TlsConn* c, const char* data, size_t n, std::string& err) {
    size_t off = 0;
    while (off < n) {
        int w = ::send(c->fd, data + off, (int)(n - off), 0);
        if (w <= 0) {
            err = "TLS: failed to write handshake data";
            return false;
        }
        off += (size_t)w;
    }
    return true;
}

static void tls_free(TlsConn* c) {
    if (!c) return;
    if (c->have_ctxt) DeleteSecurityContext(&c->ctxt);
    if (c->have_cred) FreeCredentialsHandle(&c->cred);
    delete c;
}

static TlsConn* tls_client_start(native_sock fd, const std::string& host, std::string& err) {
    TlsConn* c = new TlsConn();
    c->fd = fd;

    SCHANNEL_CRED cred{};
    cred.dwVersion = SCHANNEL_CRED_VERSION;
    cred.grbitEnabledProtocols = 0; // let the OS pick (TLS 1.2 / 1.3)
    cred.dwFlags = SCH_CRED_AUTO_CRED_VALIDATION | SCH_CRED_NO_DEFAULT_CREDS |
                   SCH_USE_STRONG_CRYPTO;
    TimeStamp expiry{};
    SECURITY_STATUS ss = AcquireCredentialsHandleA(nullptr, (char*)UNISP_NAME_A,
                                                   SECPKG_CRED_OUTBOUND, nullptr, &cred,
                                                   nullptr, nullptr, &c->cred, &expiry);
    if (ss != SEC_E_OK) {
        err = "TLS: AcquireCredentialsHandle failed";
        tls_free(c);
        return nullptr;
    }
    c->have_cred = true;

    const DWORD req = ISC_REQ_SEQUENCE_DETECT | ISC_REQ_REPLAY_DETECT |
                      ISC_REQ_CONFIDENTIALITY | ISC_REQ_ALLOCATE_MEMORY |
                      ISC_REQ_STREAM;
    std::string sni = host;
    bool first = true;
    for (;;) {
        SecBuffer inbuf[2]{};
        SecBufferDesc indesc{SECBUFFER_VERSION, 2, inbuf};
        inbuf[0].BufferType = SECBUFFER_TOKEN;
        inbuf[0].pvBuffer = c->incoming.empty() ? nullptr : &c->incoming[0];
        inbuf[0].cbBuffer = (unsigned long)c->incoming.size();
        inbuf[1].BufferType = SECBUFFER_EMPTY;

        SecBuffer outbuf[1]{};
        SecBufferDesc outdesc{SECBUFFER_VERSION, 1, outbuf};
        outbuf[0].BufferType = SECBUFFER_TOKEN;

        DWORD attrs = 0;
        ss = InitializeSecurityContextA(&c->cred, first ? nullptr : &c->ctxt,
                                        (char*)sni.c_str(), req, 0, 0,
                                        first ? nullptr : &indesc, 0,
                                        first ? &c->ctxt : nullptr, &outdesc, &attrs,
                                        &expiry);
        first = false;
        c->have_ctxt = true;

        if (outbuf[0].cbBuffer > 0 && outbuf[0].pvBuffer) {
            bool ok = sch_send_raw(c, (const char*)outbuf[0].pvBuffer,
                                   outbuf[0].cbBuffer, err);
            FreeContextBuffer(outbuf[0].pvBuffer);
            if (!ok) {
                tls_free(c);
                return nullptr;
            }
        }

        if (ss == SEC_E_INCOMPLETE_MESSAGE) {
            if (!sch_recv_raw(c, err)) {
                tls_free(c);
                return nullptr;
            }
            continue;
        }
        if (ss == SEC_I_CONTINUE_NEEDED) {
            // Keep whatever SChannel did not consume, then read more.
            if (inbuf[1].BufferType == SECBUFFER_EXTRA && inbuf[1].cbBuffer > 0)
                c->incoming.erase(0, c->incoming.size() - inbuf[1].cbBuffer);
            else
                c->incoming.clear();
            if (!sch_recv_raw(c, err)) {
                tls_free(c);
                return nullptr;
            }
            continue;
        }
        if (ss == SEC_E_OK) {
            if (inbuf[1].BufferType == SECBUFFER_EXTRA && inbuf[1].cbBuffer > 0)
                c->incoming.erase(0, c->incoming.size() - inbuf[1].cbBuffer);
            else
                c->incoming.clear();
            break;
        }
        char msg[96];
        snprintf(msg, sizeof msg, "TLS: handshake failed (SChannel 0x%08lx)",
                 (unsigned long)ss);
        err = msg;
        tls_free(c);
        return nullptr;
    }

    if (QueryContextAttributes(&c->ctxt, SECPKG_ATTR_STREAM_SIZES, &c->sizes) != SEC_E_OK) {
        err = "TLS: cannot query stream sizes";
        tls_free(c);
        return nullptr;
    }
    return c;
}

static int tls_send(TlsConn* c, const char* data, int n, std::string& err) {
    int sent = 0;
    while (sent < n) {
        int chunk = n - sent;
        if ((unsigned long)chunk > c->sizes.cbMaximumMessage)
            chunk = (int)c->sizes.cbMaximumMessage;
        std::vector<char> buf(c->sizes.cbHeader + (size_t)chunk + c->sizes.cbTrailer);
        memcpy(buf.data() + c->sizes.cbHeader, data + sent, (size_t)chunk);
        SecBuffer bufs[4]{};
        SecBufferDesc desc{SECBUFFER_VERSION, 4, bufs};
        bufs[0].BufferType = SECBUFFER_STREAM_HEADER;
        bufs[0].pvBuffer = buf.data();
        bufs[0].cbBuffer = c->sizes.cbHeader;
        bufs[1].BufferType = SECBUFFER_DATA;
        bufs[1].pvBuffer = buf.data() + c->sizes.cbHeader;
        bufs[1].cbBuffer = (unsigned long)chunk;
        bufs[2].BufferType = SECBUFFER_STREAM_TRAILER;
        bufs[2].pvBuffer = buf.data() + c->sizes.cbHeader + chunk;
        bufs[2].cbBuffer = c->sizes.cbTrailer;
        bufs[3].BufferType = SECBUFFER_EMPTY;
        if (EncryptMessage(&c->ctxt, 0, &desc, 0) != SEC_E_OK) {
            err = "TLS: EncryptMessage failed";
            return -1;
        }
        size_t total = bufs[0].cbBuffer + bufs[1].cbBuffer + bufs[2].cbBuffer;
        if (!sch_send_raw(c, buf.data(), total, err)) return -1;
        sent += chunk;
    }
    return sent;
}

static int tls_recv(TlsConn* c, char* out, int n, std::string& err) {
    for (;;) {
        if (!c->plaintext.empty()) {
            int take = (int)c->plaintext.size() < n ? (int)c->plaintext.size() : n;
            memcpy(out, c->plaintext.data(), (size_t)take);
            c->plaintext.erase(0, (size_t)take);
            return take;
        }
        if (!c->incoming.empty()) {
            SecBuffer bufs[4]{};
            SecBufferDesc desc{SECBUFFER_VERSION, 4, bufs};
            bufs[0].BufferType = SECBUFFER_DATA;
            bufs[0].pvBuffer = &c->incoming[0];
            bufs[0].cbBuffer = (unsigned long)c->incoming.size();
            bufs[1].BufferType = SECBUFFER_EMPTY;
            bufs[2].BufferType = SECBUFFER_EMPTY;
            bufs[3].BufferType = SECBUFFER_EMPTY;
            SECURITY_STATUS ss = DecryptMessage(&c->ctxt, &desc, 0, nullptr);
            if (ss == SEC_E_OK || ss == SEC_I_RENEGOTIATE) {
                std::string extra;
                for (int i = 0; i < 4; i++) {
                    if (bufs[i].BufferType == SECBUFFER_DATA && bufs[i].cbBuffer)
                        c->plaintext.append((const char*)bufs[i].pvBuffer, bufs[i].cbBuffer);
                    else if (bufs[i].BufferType == SECBUFFER_EXTRA && bufs[i].cbBuffer)
                        extra.assign((const char*)bufs[i].pvBuffer, bufs[i].cbBuffer);
                }
                c->incoming = extra;
                continue;
            }
            if (ss == SEC_I_CONTEXT_EXPIRED) return 0; // peer sent close_notify
            if (ss != SEC_E_INCOMPLETE_MESSAGE) {
                err = "TLS: DecryptMessage failed";
                return -1;
            }
        }
        char raw[16384];
        int got = ::recv(c->fd, raw, (int)sizeof raw, 0);
        if (got == 0) return 0;
        if (got < 0) {
            err = "TLS: read error";
            return -1;
        }
        c->incoming.append(raw, (size_t)got);
    }
}

static bool tls_available() { return true; }

#elif defined(KAMI_HAVE_OPENSSL)

// ---- POSIX: OpenSSL ----------------------------------------------------------
static void tls_free(TlsConn* c) {
    if (!c) return;
    if (c->ssl) {
        SSL_shutdown(c->ssl);
        SSL_free(c->ssl);
    }
    if (c->ctx) SSL_CTX_free(c->ctx);
    delete c;
}

static TlsConn* tls_client_start(native_sock fd, const std::string& host, std::string& err) {
    static bool inited = false;
    if (!inited) {
        SSL_library_init();
        SSL_load_error_strings();
        inited = true;
    }
    TlsConn* c = new TlsConn();
    c->fd = fd;
    c->ctx = SSL_CTX_new(TLS_client_method());
    if (!c->ctx) {
        err = "TLS: cannot create SSL context";
        tls_free(c);
        return nullptr;
    }
    SSL_CTX_set_default_verify_paths(c->ctx);
    SSL_CTX_set_verify(c->ctx, SSL_VERIFY_PEER, nullptr);
    c->ssl = SSL_new(c->ctx);
    if (!c->ssl) {
        err = "TLS: cannot create SSL object";
        tls_free(c);
        return nullptr;
    }
    SSL_set_fd(c->ssl, (int)fd);
    SSL_set_tlsext_host_name(c->ssl, host.c_str());
    SSL_set1_host(c->ssl, host.c_str());
    if (SSL_connect(c->ssl) != 1) {
        unsigned long e = ERR_get_error();
        char buf[160];
        ERR_error_string_n(e, buf, sizeof buf);
        err = std::string("TLS: handshake failed (") + buf + ")";
        tls_free(c);
        return nullptr;
    }
    return c;
}

static int tls_send(TlsConn* c, const char* data, int n, std::string& err) {
    int off = 0;
    while (off < n) {
        int w = SSL_write(c->ssl, data + off, n - off);
        if (w <= 0) {
            err = "TLS: write failed";
            return -1;
        }
        off += w;
    }
    return off;
}

static int tls_recv(TlsConn* c, char* out, int n, std::string& err) {
    int r = SSL_read(c->ssl, out, n);
    if (r > 0) return r;
    int se = SSL_get_error(c->ssl, r);
    if (se == SSL_ERROR_ZERO_RETURN || se == SSL_ERROR_SYSCALL) return 0;
    err = "TLS: read failed";
    return -1;
}

static bool tls_available() { return true; }

#else

// ---- no TLS provider was found at build time --------------------------------
static void tls_free(TlsConn*) {}
static TlsConn* tls_client_start(native_sock, const std::string&, std::string& err) {
    err = "TLS: this build has no TLS provider (install OpenSSL and rebuild)";
    return nullptr;
}
static int tls_send(TlsConn*, const char*, int, std::string& err) {
    err = "TLS: unavailable";
    return -1;
}
static int tls_recv(TlsConn*, char*, int, std::string& err) {
    err = "TLS: unavailable";
    return -1;
}
static bool tls_available() { return false; }

#endif

// ---------------------------------------------------------------- socket objects
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
    s->tls = nullptr;
    return s;
}

void socket_release(KamiSocket* s) { // called by the GC sweep
    if (s->tls) {
        tls_free((TlsConn*)s->tls);
        s->tls = nullptr;
    }
}

static void apply_timeout(native_sock fd, double seconds) {
#ifdef _WIN32
    DWORD ms = seconds <= 0 ? 0 : (DWORD)(seconds * 1000.0);
    setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, (const char*)&ms, sizeof ms);
    setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, (const char*)&ms, sizeof ms);
#else
    struct timeval tv;
    tv.tv_sec = (long)(seconds > 0 ? seconds : 0);
    tv.tv_usec = (long)(seconds > 0 ? (seconds - (double)tv.tv_sec) * 1e6 : 0);
    setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof tv);
    setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof tv);
#endif
}

bool socket_create(KamiValue* out) {
    sock_startup();
    native_sock fd = ::socket(AF_INET, SOCK_STREAM, 0);
    if ((int64_t)fd < 0) return false;
    int one = 1;
    setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, (const char*)&one, sizeof one);
    KamiSocket* s = sock_new((int64_t)fd);
    out->tag = KT_SOCKET;
    out->p = s;
    return true;
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
        double tmo = s->timeout;
        lk.unlock();
        int rc = getaddrinfo(host.c_str(), std::to_string(port).c_str(), &hints, &res);
        int crc = -1;
        if (rc == 0 && res) {
            if (tmo > 0) apply_timeout(fd, tmo);
            crc = ::connect(fd, res->ai_addr, (socklen_t)res->ai_addrlen);
            freeaddrinfo(res);
        }
        lk.lock();
        if (crc != 0)
            panic("ConnectionError: cannot connect to " + host + ":" + std::to_string(port));
        set_none();
        return;
    }
    // wrap_tls(hostname): hands the connected socket to the platform TLS stack;
    // every later send/recv on this socket is encrypted. This is the `ssl`
    // module's job in CPython, and it is the only reason TLS appears in C here.
    if (m == "wrap_tls" && nargs == 1) {
        if (argv[0]->tag != KT_STR) panic("socket.wrap_tls() expects a hostname str");
        if (s->tls) panic("socket is already wrapped in TLS");
        KamiStr* h = (KamiStr*)argv[0]->p;
        std::string host(h->data, (size_t)h->len);
        std::string err;
        lk.unlock();
        TlsConn* c = tls_client_start(fd, host, err);
        lk.lock();
        if (!c) panic("SSLError: " + err);
        s->tls = c;
        *out = *obj; // chainable, like ssl.wrap_socket()
        return;
    }
    if (m == "tls_available" && nargs == 0) {
        out->tag = KT_BOOL;
        out->i = tls_available();
        return;
    }
    if ((m == "send" || m == "sendall") && nargs == 1) {
        if (argv[0]->tag != KT_STR) panic("socket.send() expects a str");
        KamiStr* d = (KamiStr*)argv[0]->p;
        std::string data(d->data, (size_t)d->len);
        TlsConn* tls = (TlsConn*)s->tls;
        std::string err;
        lk.unlock();
        int64_t sent = 0;
        if (tls) {
            int w = tls_send(tls, data.data(), (int)data.size(), err);
            sent = w < 0 ? 0 : w;
        } else {
            while (sent < (int64_t)data.size()) {
                auto n = ::send(fd, data.data() + sent, (size_t)(data.size() - sent), 0);
                if (n <= 0) break;
                sent += n;
            }
        }
        lk.lock();
        if (sent < (int64_t)data.size())
            panic("OSError: send failed" + (err.empty() ? std::string() : " (" + err + ")"));
        out->tag = KT_INT;
        out->i = sent;
        return;
    }
    if (m == "recv" && nargs == 1) {
        if (argv[0]->tag != KT_INT) panic("socket.recv() expects a size");
        std::vector<char> buf((size_t)(argv[0]->i > 0 ? argv[0]->i : 1));
        TlsConn* tls = (TlsConn*)s->tls;
        std::string err;
        lk.unlock();
        int64_t n;
        if (tls) n = tls_recv(tls, buf.data(), (int)buf.size(), err);
        else n = ::recv(fd, buf.data(), buf.size(), 0);
        lk.lock();
        if (n < 0) panic("OSError: recv failed" +
                         (err.empty() ? std::string() : " (" + err + ")"));
        out->tag = KT_STR;
        out->p = str_new(buf.data(), n);
        return;
    }
    if (m == "settimeout" && nargs == 1) {
        double sec = -1.0;
        if (argv[0]->tag == KT_FLOAT) sec = argv[0]->f;
        else if (argv[0]->tag == KT_INT) sec = (double)argv[0]->i;
        s->timeout = sec;
        apply_timeout(fd, sec);
        set_none();
        return;
    }
    if (m == "gettimeout" && nargs == 0) {
        if (s->timeout <= 0) set_none();
        else {
            out->tag = KT_FLOAT;
            out->f = s->timeout;
        }
        return;
    }
    if (m == "fileno" && nargs == 0) {
        out->tag = KT_INT;
        out->i = s->fd;
        return;
    }
    if (m == "close" && nargs == 0) {
        if (!s->closed) {
            socket_release(s);
            CLOSESOCK(fd);
            s->closed = true;
        }
        set_none();
        return;
    }
    panic("'socket' object has no method '" + m + "' with " + std::to_string(nargs) + " args");
}

} // namespace kami
