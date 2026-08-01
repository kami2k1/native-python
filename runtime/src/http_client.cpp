// Native HTTP/HTTPS client for the `requests` module — no external processes.
// The Python `requests` library is "translated" down to the runtime's own
// network layer, exactly like the rest of the stdlib:
//   * Windows: WinHTTP (system API; TLS via SChannel, proxy + redirects built in)
//   * POSIX:   raw TCP sockets (+ OpenSSL for https when available),
//              with http(s)_proxy support, redirects and chunked decoding.
#include "rt_internal.h"

#include <cctype>
#include <cstdlib>
#include <cstring>
#include <string>

#ifdef _WIN32

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <winhttp.h>
#pragma comment(lib, "winhttp.lib")

namespace kami {

static std::wstring widen(const std::string& s) {
    if (s.empty()) return L"";
    int n = MultiByteToWideChar(CP_UTF8, 0, s.data(), (int)s.size(), nullptr, 0);
    std::wstring w(n, L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s.data(), (int)s.size(), &w[0], n);
    return w;
}

bool http_request(const std::string& method, const std::string& url,
                  const std::string& body, const std::string& content_type,
                  double timeout_sec, long& status_out, std::string& body_out,
                  std::string& err) {
    std::wstring wurl = widen(url);

    URL_COMPONENTS uc;
    memset(&uc, 0, sizeof uc);
    uc.dwStructSize = sizeof uc;
    wchar_t host[256], path[2048];
    uc.lpszHostName = host;
    uc.dwHostNameLength = 256;
    uc.lpszUrlPath = path;
    uc.dwUrlPathLength = 2048;
    if (!WinHttpCrackUrl(wurl.c_str(), (DWORD)wurl.size(), 0, &uc)) {
        err = "invalid URL";
        return false;
    }
    bool https = uc.nScheme == INTERNET_SCHEME_HTTPS;
    if (!https && uc.nScheme != INTERNET_SCHEME_HTTP) {
        err = "unsupported URL scheme (only http/https)";
        return false;
    }

    HINTERNET ses = WinHttpOpen(L"kamipy-requests/1.0",
                                WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY,
                                WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
    if (!ses) {
        // older Windows: fall back to the default (registry) proxy config
        ses = WinHttpOpen(L"kamipy-requests/1.0", WINHTTP_ACCESS_TYPE_DEFAULT_PROXY,
                          WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
    }
    if (!ses) {
        err = "WinHttpOpen failed";
        return false;
    }
    if (timeout_sec > 0) {
        int ms = (int)(timeout_sec * 1000.0);
        WinHttpSetTimeouts(ses, ms, ms, ms, ms);
    }

    HINTERNET con = WinHttpConnect(ses, host, uc.nPort, 0);
    if (!con) {
        WinHttpCloseHandle(ses);
        err = "cannot connect to host";
        return false;
    }
    HINTERNET req = WinHttpOpenRequest(con, widen(method).c_str(), path, nullptr,
                                       WINHTTP_NO_REFERER, WINHTTP_DEFAULT_ACCEPT_TYPES,
                                       https ? WINHTTP_FLAG_SECURE : 0);
    if (!req) {
        WinHttpCloseHandle(con);
        WinHttpCloseHandle(ses);
        err = "cannot open request";
        return false;
    }

    std::wstring headers;
    if (!content_type.empty())
        headers = L"Content-Type: " + widen(content_type) + L"\r\n";

    BOOL ok = WinHttpSendRequest(
        req, headers.empty() ? WINHTTP_NO_ADDITIONAL_HEADERS : headers.c_str(),
        headers.empty() ? 0 : (DWORD)-1L,
        body.empty() ? WINHTTP_NO_REQUEST_DATA : (LPVOID)body.data(),
        (DWORD)body.size(), (DWORD)body.size(), 0);
    if (ok) ok = WinHttpReceiveResponse(req, nullptr);
    if (!ok) {
        DWORD e = GetLastError();
        WinHttpCloseHandle(req);
        WinHttpCloseHandle(con);
        WinHttpCloseHandle(ses);
        switch (e) {
        case ERROR_WINHTTP_TIMEOUT: err = "timed out"; break;
        case ERROR_WINHTTP_NAME_NOT_RESOLVED: err = "cannot resolve host name"; break;
        case ERROR_WINHTTP_CANNOT_CONNECT: err = "connection refused"; break;
        case ERROR_WINHTTP_SECURE_FAILURE: err = "TLS/certificate error"; break;
        default: err = "WinHTTP error " + std::to_string((long)e); break;
        }
        return false;
    }

    DWORD status = 0, sz = sizeof status;
    WinHttpQueryHeaders(req, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER,
                        WINHTTP_HEADER_NAME_BY_INDEX, &status, &sz,
                        WINHTTP_NO_HEADER_INDEX);
    status_out = (long)status;

    body_out.clear();
    for (;;) {
        DWORD avail = 0;
        if (!WinHttpQueryDataAvailable(req, &avail) || avail == 0) break;
        size_t off = body_out.size();
        body_out.resize(off + avail);
        DWORD got = 0;
        if (!WinHttpReadData(req, &body_out[off], avail, &got)) {
            body_out.resize(off);
            break;
        }
        body_out.resize(off + got);
        if (got == 0) break;
    }

    WinHttpCloseHandle(req);
    WinHttpCloseHandle(con);
    WinHttpCloseHandle(ses);
    return true;
}

} // namespace kami

#else // ---------------------------------------------------------------- POSIX

#include <arpa/inet.h>
#include <fcntl.h>
#include <netdb.h>
#include <netinet/in.h>
#include <sys/select.h>
#include <sys/socket.h>
#include <unistd.h>

#ifdef KAMI_HAVE_OPENSSL
#include <openssl/err.h>
#include <openssl/ssl.h>
#endif

namespace kami {

namespace {

struct Url {
    bool https = false;
    std::string host;
    int port = 80;
    std::string path = "/";
};

bool parse_url(const std::string& url, Url& u, std::string& err) {
    std::string rest;
    if (url.rfind("http://", 0) == 0) {
        u.https = false;
        u.port = 80;
        rest = url.substr(7);
    } else if (url.rfind("https://", 0) == 0) {
        u.https = true;
        u.port = 443;
        rest = url.substr(8);
    } else {
        err = "unsupported URL scheme (only http/https)";
        return false;
    }
    size_t slash = rest.find('/');
    std::string hostport = slash == std::string::npos ? rest : rest.substr(0, slash);
    u.path = slash == std::string::npos ? "/" : rest.substr(slash);
    size_t colon = hostport.rfind(':');
    if (colon != std::string::npos) {
        u.host = hostport.substr(0, colon);
        u.port = atoi(hostport.c_str() + colon + 1);
    } else {
        u.host = hostport;
    }
    if (u.host.empty()) {
        err = "invalid URL: missing host";
        return false;
    }
    return true;
}

// Blocking connect with timeout (non-blocking connect + select).
int tcp_connect(const std::string& host, int port, double timeout_sec, std::string& err) {
    addrinfo hints;
    memset(&hints, 0, sizeof hints);
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;
    addrinfo* res = nullptr;
    if (getaddrinfo(host.c_str(), std::to_string(port).c_str(), &hints, &res) != 0 || !res) {
        err = "cannot resolve host name '" + host + "'";
        return -1;
    }
    int fd = -1;
    for (addrinfo* ai = res; ai; ai = ai->ai_next) {
        fd = socket(ai->ai_family, ai->ai_socktype, ai->ai_protocol);
        if (fd < 0) continue;
        int flags = fcntl(fd, F_GETFL, 0);
        fcntl(fd, F_SETFL, flags | O_NONBLOCK);
        int rc = connect(fd, ai->ai_addr, ai->ai_addrlen);
        if (rc != 0 && errno == EINPROGRESS) {
            fd_set wf;
            FD_ZERO(&wf);
            FD_SET(fd, &wf);
            timeval tv;
            double t = timeout_sec > 0 ? timeout_sec : 60.0;
            tv.tv_sec = (time_t)t;
            tv.tv_usec = (suseconds_t)((t - (double)tv.tv_sec) * 1e6);
            rc = select(fd + 1, nullptr, &wf, nullptr, &tv);
            if (rc > 0) {
                int soerr = 0;
                socklen_t len = sizeof soerr;
                getsockopt(fd, SOL_SOCKET, SO_ERROR, &soerr, &len);
                rc = soerr == 0 ? 0 : -1;
            } else {
                rc = -1;
            }
        }
        if (rc == 0) {
            fcntl(fd, F_SETFL, flags); // back to blocking
            break;
        }
        ::close(fd);
        fd = -1;
    }
    freeaddrinfo(res);
    if (fd < 0) {
        err = "cannot connect to " + host + ":" + std::to_string(port);
        return -1;
    }
    if (timeout_sec > 0) {
        timeval tv;
        tv.tv_sec = (time_t)timeout_sec;
        tv.tv_usec = (suseconds_t)((timeout_sec - (double)tv.tv_sec) * 1e6);
        setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof tv);
        setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof tv);
    }
    return fd;
}

// Transport abstraction: plain fd or TLS.
struct Conn {
    int fd = -1;
#ifdef KAMI_HAVE_OPENSSL
    SSL_CTX* ctx = nullptr;
    SSL* ssl = nullptr;
#endif

    ~Conn() { close_all(); }

    void close_all() {
#ifdef KAMI_HAVE_OPENSSL
        if (ssl) {
            SSL_shutdown(ssl);
            SSL_free(ssl);
            ssl = nullptr;
        }
        if (ctx) {
            SSL_CTX_free(ctx);
            ctx = nullptr;
        }
#endif
        if (fd >= 0) {
            ::close(fd);
            fd = -1;
        }
    }

    bool start_tls(const std::string& sni_host, std::string& err) {
#ifdef KAMI_HAVE_OPENSSL
        static bool inited = false;
        if (!inited) {
            SSL_library_init();
            SSL_load_error_strings();
            inited = true;
        }
        ctx = SSL_CTX_new(TLS_client_method());
        if (!ctx) {
            err = "TLS: cannot create context";
            return false;
        }
        SSL_CTX_set_default_verify_paths(ctx);
        SSL_CTX_set_verify(ctx, SSL_VERIFY_PEER, nullptr);
        ssl = SSL_new(ctx);
        SSL_set_fd(ssl, fd);
        SSL_set_tlsext_host_name(ssl, sni_host.c_str());
        SSL_set1_host(ssl, sni_host.c_str());
        if (SSL_connect(ssl) != 1) {
            unsigned long e = ERR_get_error();
            char ebuf[256];
            ERR_error_string_n(e, ebuf, sizeof ebuf);
            err = std::string("TLS handshake failed: ") + ebuf;
            return false;
        }
        return true;
#else
        (void)sni_host;
        err = "https:// requires OpenSSL (runtime was built without it)";
        return false;
#endif
    }

    ssize_t send_all(const char* data, size_t n) {
        size_t off = 0;
        while (off < n) {
            ssize_t w;
#ifdef KAMI_HAVE_OPENSSL
            if (ssl) w = SSL_write(ssl, data + off, (int)(n - off));
            else
#endif
                w = ::send(fd, data + off, n - off, 0);
            if (w <= 0) return -1;
            off += (size_t)w;
        }
        return (ssize_t)off;
    }

    ssize_t recv_some(char* buf, size_t n) {
#ifdef KAMI_HAVE_OPENSSL
        if (ssl) return SSL_read(ssl, buf, (int)n);
#endif
        return ::recv(fd, buf, n, 0);
    }
};

// Read until EOF (we always send "Connection: close").
bool read_response(Conn& c, std::string& raw, std::string& err) {
    char buf[8192];
    for (;;) {
        ssize_t n = c.recv_some(buf, sizeof buf);
        if (n > 0) {
            raw.append(buf, (size_t)n);
            continue;
        }
        if (n == 0) break; // clean EOF
#ifdef KAMI_HAVE_OPENSSL
        if (c.ssl) {
            int se = SSL_get_error(c.ssl, (int)n);
            if (se == SSL_ERROR_ZERO_RETURN) break;
        }
#endif
        if (errno == EAGAIN || errno == EWOULDBLOCK) {
            err = "timed out while reading response";
            return false;
        }
        break; // treat other errors at EOF as end of body
    }
    if (raw.empty()) {
        err = "empty response from server";
        return false;
    }
    return true;
}

bool decode_chunked(const std::string& in, std::string& out) {
    size_t pos = 0;
    for (;;) {
        size_t eol = in.find("\r\n", pos);
        if (eol == std::string::npos) return false;
        long len = strtol(in.c_str() + pos, nullptr, 16);
        if (len < 0) return false;
        pos = eol + 2;
        if (len == 0) return true;
        if (pos + (size_t)len > in.size()) return false;
        out.append(in, pos, (size_t)len);
        pos += (size_t)len + 2; // skip data + CRLF
    }
}

std::string header_value(const std::string& headers, const std::string& name) {
    size_t pos = 0;
    while (pos < headers.size()) {
        size_t eol = headers.find("\r\n", pos);
        if (eol == std::string::npos) eol = headers.size();
        size_t colon = headers.find(':', pos);
        if (colon != std::string::npos && colon < eol &&
            colon - pos == name.size() &&
            strncasecmp(headers.c_str() + pos, name.c_str(), name.size()) == 0) {
            size_t v = colon + 1;
            while (v < eol && headers[v] == ' ') v++;
            return headers.substr(v, eol - v);
        }
        pos = eol + 2;
    }
    return "";
}

// http(s)_proxy support: returns proxy host/port for the given scheme, or false.
bool env_proxy(bool https, std::string& phost, int& pport) {
    const char* e = https ? getenv("https_proxy") : getenv("http_proxy");
    if (!e || !*e) e = https ? getenv("HTTPS_PROXY") : getenv("HTTP_PROXY");
    if (!e || !*e) return false;
    Url pu;
    std::string dummy;
    if (!parse_url(e, pu, dummy)) return false;
    phost = pu.host;
    pport = pu.port;
    return true;
}

bool one_request(const std::string& method, const Url& u, const std::string& body,
                 const std::string& content_type, double timeout_sec, long& status,
                 std::string& resp_body, std::string& location, std::string& err) {
    std::string phost;
    int pport = 0;
    bool use_proxy = env_proxy(u.https, phost, pport);

    Conn c;
    c.fd = tcp_connect(use_proxy ? phost : u.host, use_proxy ? pport : u.port,
                       timeout_sec, err);
    if (c.fd < 0) return false;

    if (use_proxy && u.https) {
        // CONNECT tunnel, then TLS through it.
        std::string t = "CONNECT " + u.host + ":" + std::to_string(u.port) +
                        " HTTP/1.1\r\nHost: " + u.host + ":" + std::to_string(u.port) +
                        "\r\n\r\n";
        if (c.send_all(t.data(), t.size()) < 0) {
            err = "proxy CONNECT failed";
            return false;
        }
        std::string hdr;
        char ch;
        while (hdr.find("\r\n\r\n") == std::string::npos) {
            ssize_t n = c.recv_some(&ch, 1);
            if (n <= 0) {
                err = "proxy CONNECT failed";
                return false;
            }
            hdr += ch;
            if (hdr.size() > 65536) {
                err = "proxy CONNECT: response too large";
                return false;
            }
        }
        if (hdr.find(" 200") == std::string::npos) {
            err = "proxy refused CONNECT";
            return false;
        }
    }

    if (u.https && !c.start_tls(u.host, err)) return false;

    std::string target = (use_proxy && !u.https)
                             ? "http://" + u.host +
                                   (u.port != 80 ? ":" + std::to_string(u.port) : "") + u.path
                             : u.path;
    std::string req = method + " " + target + " HTTP/1.1\r\n";
    req += "Host: " + u.host +
           ((u.https ? u.port != 443 : u.port != 80) ? ":" + std::to_string(u.port) : "") +
           "\r\n";
    req += "User-Agent: kamipy-requests/1.0\r\n";
    req += "Accept: */*\r\n";
    req += "Connection: close\r\n";
    if (!body.empty() || method == "POST") {
        if (!content_type.empty()) req += "Content-Type: " + content_type + "\r\n";
        req += "Content-Length: " + std::to_string(body.size()) + "\r\n";
    }
    req += "\r\n";
    req += body;

    if (c.send_all(req.data(), req.size()) < 0) {
        err = "failed to send request";
        return false;
    }

    std::string raw;
    if (!read_response(c, raw, err)) return false;

    size_t hdr_end = raw.find("\r\n\r\n");
    if (hdr_end == std::string::npos || raw.compare(0, 5, "HTTP/") != 0) {
        err = "malformed HTTP response";
        return false;
    }
    size_t sp = raw.find(' ');
    status = strtol(raw.c_str() + sp + 1, nullptr, 10);
    std::string headers = raw.substr(0, hdr_end);
    std::string payload = raw.substr(hdr_end + 4);

    std::string te = header_value(headers, "Transfer-Encoding");
    for (auto& ch2 : te) ch2 = (char)tolower((unsigned char)ch2);
    if (te.find("chunked") != std::string::npos) {
        std::string decoded;
        if (!decode_chunked(payload, decoded)) {
            err = "bad chunked encoding in response";
            return false;
        }
        resp_body = std::move(decoded);
    } else {
        std::string cl = header_value(headers, "Content-Length");
        if (!cl.empty()) {
            size_t want = (size_t)strtol(cl.c_str(), nullptr, 10);
            if (payload.size() > want) payload.resize(want);
        }
        resp_body = std::move(payload);
    }
    location = header_value(headers, "Location");
    return true;
}

} // namespace

bool http_request(const std::string& method, const std::string& url,
                  const std::string& body, const std::string& content_type,
                  double timeout_sec, long& status_out, std::string& body_out,
                  std::string& err) {
    std::string cur_url = url;
    std::string cur_method = method;
    std::string cur_body = body;
    std::string cur_ct = content_type;
    for (int hop = 0; hop < 6; hop++) {
        Url u;
        if (!parse_url(cur_url, u, err)) return false;
        long status = 0;
        std::string rbody, location;
        if (!one_request(cur_method, u, cur_body, cur_ct, timeout_sec, status, rbody,
                         location, err))
            return false;
        bool redirect = status == 301 || status == 302 || status == 303 ||
                        status == 307 || status == 308;
        if (redirect && !location.empty() && hop < 5) {
            if (location[0] == '/')
                cur_url = (u.https ? "https://" : "http://") + u.host +
                          (u.port != (u.https ? 443 : 80) ? ":" + std::to_string(u.port)
                                                          : "") +
                          location;
            else
                cur_url = location;
            if (status == 303 || ((status == 301 || status == 302) && cur_method == "POST")) {
                cur_method = "GET"; // like python-requests
                cur_body.clear();
                cur_ct.clear();
            }
            continue;
        }
        status_out = status;
        body_out = std::move(rbody);
        return true;
    }
    err = "too many redirects";
    return false;
}

} // namespace kami

#endif
