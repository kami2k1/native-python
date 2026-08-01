"""HTTP for humans — pure Python, compiled to native code by kamipy.

This replaces the hand-written C++ HTTP client (runtime/src/http_client.cpp).
The only thing below this module is a socket: `socket.socket()` for the TCP
connection and `sock.wrap_tls(host)` for the platform TLS stack (OpenSSL on
POSIX, SChannel on Windows). URL parsing, request framing, header parsing,
chunked transfer decoding, redirects and the Response object all live here, in
Python, where they belong.

    r = requests.get("https://example.com/api", params={"q": "x"}, timeout=5)
    if r.ok:
        print(r.status_code, r.json())
"""
import json as _json
import os
import socket

__version__ = "1.0.0"

DEFAULT_USER_AGENT = "kamipy-requests/" + __version__
MAX_REDIRECTS = 10
_CHUNK = 16384

# Exception "classes" are not yet expressible here, so the library raises
# ValueError with a tagged message. `except Exception as e` sees the tag, and
# the names below let user code spell the intent it means.
RequestException = "RequestException"
ConnectionError = "ConnectionError"
Timeout = "Timeout"
HTTPError = "HTTPError"
TooManyRedirects = "TooManyRedirects"
SSLError = "SSLError"


def _raise(kind, message):
    raise ValueError(kind + ": " + message)


# ---------------------------------------------------------------- URLs
def urlparse(url):
    """Splits a URL into [scheme, host, port, path]. Minimal but sufficient."""
    rest = url
    scheme = "http"
    at = rest.find("://")
    if at >= 0:
        scheme = rest[:at].lower()
        rest = rest[at + 3:]
    if scheme != "http" and scheme != "https":
        _raise(RequestException, "unsupported URL scheme '" + scheme + "'")
    slash = rest.find("/")
    if slash < 0:
        authority = rest
        path = "/"
    else:
        authority = rest[:slash]
        path = rest[slash:]
    # strip any userinfo
    mark = authority.rfind("@")
    if mark >= 0:
        authority = authority[mark + 1:]
    port = 443 if scheme == "https" else 80
    colon = authority.rfind(":")
    if colon >= 0 and authority.find("]") < colon:
        digits = authority[colon + 1:]
        if digits.isdigit():
            port = int(digits)
            authority = authority[:colon]
    if authority == "":
        _raise(RequestException, "URL has no host: " + url)
    return [scheme, authority, port, path]


_SAFE = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789-_.~"
_HEXDIG = "0123456789ABCDEF"


def quote(text, safe="/"):
    out = ""
    for ch in str(text):
        if ch in _SAFE or ch in safe:
            out = out + ch
        else:
            code = ord(ch)
            out = out + "%" + _HEXDIG[(code >> 4) & 15] + _HEXDIG[code & 15]
    return out


def urlencode(params):
    parts = []
    if isinstance(params, dict):
        for key in params.keys():
            value = params[key]
            if isinstance(value, list):
                for item in value:
                    parts.append(quote(key, "") + "=" + quote(item, ""))
            else:
                parts.append(quote(key, "") + "=" + quote(value, ""))
    else:
        for pair in params:
            parts.append(quote(pair[0], "") + "=" + quote(pair[1], ""))
    return "&".join(parts)


# ---------------------------------------------------------------- Response
class Response:
    def __init__(self, url, status_code, reason, headers, body):
        self.url = url
        self.status_code = status_code
        self.reason = reason
        self.headers = headers
        self.content = body
        self.text = body
        self.encoding = "utf-8"
        self.history = []
        self.elapsed = 0.0
        self.ok = 200 <= status_code and status_code < 400
        self.is_redirect = status_code in _REDIRECTS

    def json(self):
        return _json.loads(self.text)

    def raise_for_status(self):
        if 400 <= self.status_code and self.status_code < 500:
            _raise(HTTPError, str(self.status_code) + " Client Error: " + self.reason +
                   " for url " + self.url)
        if self.status_code >= 500:
            _raise(HTTPError, str(self.status_code) + " Server Error: " + self.reason +
                   " for url " + self.url)
        return None

    def iter_lines(self):
        return self.text.splitlines()

    def __repr__(self):
        return "<Response [" + str(self.status_code) + "]>"


# ---------------------------------------------------------------- header bag
# Header lookups must be case-insensitive; a dict keyed on the lowered name with
# the original spelling kept alongside is enough for that.
class Headers:
    def __init__(self):
        self._map = {}
        self._order = []

    def add(self, name, value):
        key = name.lower()
        if key not in self._map:
            self._order.append(name)
            self._map[key] = value
        else:
            self._map[key] = self._map[key] + ", " + value

    def get(self, name, default=None):
        v = self._map.get(name.lower())
        if v is None:
            return default
        return v

    def keys(self):
        return self._order

    def items(self):
        out = []
        for name in self._order:
            out.append([name, self._map[name.lower()]])
        return out

    def __len__(self):
        return len(self._order)

    def __repr__(self):
        return str(self.items())


# ---------------------------------------------------------------- transport
def _read_all(sock):
    out = ""
    while True:
        chunk = sock.recv(_CHUNK)
        if chunk == "":
            break
        out = out + chunk
    return out


def _read_until_headers(sock, buffered):
    data = buffered
    while data.find("\r\n\r\n") < 0:
        chunk = sock.recv(_CHUNK)
        if chunk == "":
            break
        data = data + chunk
        if len(data) > 262144:
            _raise(RequestException, "response headers too large")
    return data


def _parse_status(line):
    bits = line.split(" ")
    if len(bits) < 2 or not bits[0].startswith("HTTP/"):
        _raise(RequestException, "malformed HTTP status line: " + line[:60])
    code = bits[1]
    if not code.isdigit():
        _raise(RequestException, "malformed HTTP status code: " + code)
    reason = " ".join(bits[2:]) if len(bits) > 2 else ""
    return [int(code), reason]


def _parse_headers(block):
    bag = Headers()
    for line in block.split("\r\n"):
        if line == "":
            continue
        colon = line.find(":")
        if colon < 0:
            continue
        bag.add(line[:colon].strip(), line[colon + 1:].strip())
    return bag


def _decode_chunked(sock, buffered):
    """Reads a chunked body, pulling more bytes from the socket as needed."""
    data = buffered
    out = ""
    pos = 0
    while True:
        eol = data.find("\r\n", pos)
        while eol < 0:
            more = sock.recv(_CHUNK)
            if more == "":
                _raise(RequestException, "truncated chunked response")
            data = data + more
            eol = data.find("\r\n", pos)
        size_text = data[pos:eol].split(";")[0].strip()
        if size_text == "":
            _raise(RequestException, "bad chunk size")
        size = int(size_text, 16)
        pos = eol + 2
        if size == 0:
            return out
        while len(data) < pos + size + 2:
            more = sock.recv(_CHUNK)
            if more == "":
                break
            data = data + more
        out = out + data[pos:pos + size]
        pos = pos + size + 2


def _connect(scheme, host, port, timeout):
    sock = socket.socket()
    if timeout is not None and timeout > 0:
        sock.settimeout(timeout)
    try:
        sock.connect((host, port))
    except Exception as e:
        sock.close()
        _raise(ConnectionError, "cannot connect to " + host + ":" + str(port) +
               " (" + str(e) + ")")
    if scheme == "https":
        try:
            sock.wrap_tls(host)
        except Exception as e:
            sock.close()
            _raise(SSLError, "TLS handshake with " + host + " failed (" + str(e) + ")")
    return sock


def _proxy_for(scheme):
    name = "https_proxy" if scheme == "https" else "http_proxy"
    value = os.getenv(name, "")
    if value == "":
        value = os.getenv(name.upper(), "")
    if value == "":
        return None
    return urlparse(value)


def _build_request(method, host, port, scheme, path, body, headers):
    lines = [method + " " + path + " HTTP/1.1"]
    seen = {}
    for pair in headers.items():
        seen[pair[0].lower()] = True
        lines.append(pair[0] + ": " + pair[1])
    if "host" not in seen:
        default_port = 443 if scheme == "https" else 80
        hostvalue = host if port == default_port else host + ":" + str(port)
        lines.append("Host: " + hostvalue)
    if "user-agent" not in seen:
        lines.append("User-Agent: " + DEFAULT_USER_AGENT)
    if "accept" not in seen:
        lines.append("Accept: */*")
    lines.append("Connection: close")
    if body != "" or method == "POST" or method == "PUT" or method == "PATCH":
        if "content-length" not in seen:
            lines.append("Content-Length: " + str(len(body)))
    return "\r\n".join(lines) + "\r\n\r\n" + body


def _one_request(method, url, body, headers, timeout):
    parts = urlparse(url)
    scheme = parts[0]
    host = parts[1]
    port = parts[2]
    path = parts[3]

    target = path
    proxy = _proxy_for(scheme)
    connect_host = host
    connect_port = port
    connect_scheme = scheme
    if proxy is not None and scheme == "http":
        # A plain-HTTP proxy wants the absolute URI and its own endpoint.
        target = scheme + "://" + host + (":" + str(port) if port != 80 else "") + path
        connect_host = proxy[1]
        connect_port = proxy[2]
        connect_scheme = "http"

    sock = _connect(connect_scheme, connect_host, connect_port, timeout)
    try:
        wire = _build_request(method, host, port, scheme, target, body, headers)
        sock.send(wire)
        raw = _read_until_headers(sock, "")
        split = raw.find("\r\n\r\n")
        if split < 0:
            _raise(RequestException, "no HTTP response from " + host)
        head = raw[:split]
        leftover = raw[split + 4:]
        eol = head.find("\r\n")
        if eol < 0:
            status_line = head
            header_block = ""
        else:
            status_line = head[:eol]
            header_block = head[eol + 2:]
        status = _parse_status(status_line)
        bag = _parse_headers(header_block)

        transfer = bag.get("Transfer-Encoding", "")
        if transfer.lower().find("chunked") >= 0:
            payload = _decode_chunked(sock, leftover)
        else:
            payload = leftover + _read_all(sock)
            length = bag.get("Content-Length")
            if length is not None and length.strip().isdigit():
                want = int(length.strip())
                if len(payload) > want:
                    payload = payload[:want]
        return Response(url, status[0], status[1], bag, payload)
    finally:
        sock.close()


def _absolute(base_url, location):
    if location.find("://") >= 0:
        return location
    parts = urlparse(base_url)
    root = parts[0] + "://" + parts[1]
    default_port = 443 if parts[0] == "https" else 80
    if parts[2] != default_port:
        root = root + ":" + str(parts[2])
    if location.startswith("/"):
        return root + location
    stem = parts[3]
    cut = stem.rfind("/")
    if cut < 0:
        return root + "/" + location
    return root + stem[:cut + 1] + location


_REDIRECTS = [301, 302, 303, 307, 308]


def request(method, url, params=None, data=None, json=None, headers=None,
            timeout=None, allow_redirects=True, verify=True, auth=None, files=None,
            stream=False, cookies=None):
    method = method.upper()
    if params is not None and len(params) > 0:
        query = urlencode(params)
        url = url + ("&" if url.find("?") >= 0 else "?") + query

    bag = Headers()
    body = ""
    if json is not None:
        body = _json.dumps(json)
        bag.add("Content-Type", "application/json")
    elif isinstance(data, dict):
        body = urlencode(data)
        bag.add("Content-Type", "application/x-www-form-urlencoded")
    elif data is not None:
        body = str(data)
    if headers is not None:
        for key in headers.keys():
            bag.add(str(key), str(headers[key]))
    if auth is not None:
        bag.add("Authorization", "Basic " + _b64(str(auth[0]) + ":" + str(auth[1])))

    history = []
    current = url
    current_method = method
    current_body = body
    hop = 0
    while True:
        response = _one_request(current_method, current, current_body, bag, timeout)
        if not allow_redirects or response.status_code not in _REDIRECTS:
            response.history = history
            return response
        location = response.headers.get("Location")
        if location is None:
            response.history = history
            return response
        hop = hop + 1
        if hop > MAX_REDIRECTS:
            _raise(TooManyRedirects, "exceeded " + str(MAX_REDIRECTS) + " redirects")
        history.append(response)
        current = _absolute(current, location)
        if response.status_code == 303 or (current_method == "POST" and
                                          response.status_code in [301, 302]):
            current_method = "GET"
            current_body = ""


_B64 = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/"


def _b64(text):
    out = ""
    i = 0
    n = len(text)
    while i < n:
        b0 = ord(text[i])
        b1 = ord(text[i + 1]) if i + 1 < n else 0
        b2 = ord(text[i + 2]) if i + 2 < n else 0
        out = out + _B64[b0 >> 2]
        out = out + _B64[((b0 & 3) << 4) | (b1 >> 4)]
        out = out + (_B64[((b1 & 15) << 2) | (b2 >> 6)] if i + 1 < n else "=")
        out = out + (_B64[b2 & 63] if i + 2 < n else "=")
        i = i + 3
    return out


def get(url, params=None, headers=None, timeout=None, allow_redirects=True, verify=True,
        auth=None, stream=False, cookies=None):
    return request("GET", url, params, None, None, headers, timeout, allow_redirects,
                   verify, auth)


def post(url, data=None, json=None, params=None, headers=None, timeout=None,
         allow_redirects=True, verify=True, auth=None, files=None):
    return request("POST", url, params, data, json, headers, timeout, allow_redirects,
                   verify, auth)


def put(url, data=None, json=None, params=None, headers=None, timeout=None,
        allow_redirects=True, verify=True, auth=None):
    return request("PUT", url, params, data, json, headers, timeout, allow_redirects,
                   verify, auth)


def patch(url, data=None, json=None, params=None, headers=None, timeout=None,
          allow_redirects=True, verify=True, auth=None):
    return request("PATCH", url, params, data, json, headers, timeout, allow_redirects,
                   verify, auth)


def delete(url, params=None, headers=None, timeout=None, allow_redirects=True,
           verify=True, auth=None):
    return request("DELETE", url, params, None, None, headers, timeout, allow_redirects,
                   verify, auth)


def head(url, params=None, headers=None, timeout=None, allow_redirects=False,
         verify=True, auth=None):
    return request("HEAD", url, params, None, None, headers, timeout, allow_redirects,
                   verify, auth)


def options(url, headers=None, timeout=None):
    return request("OPTIONS", url, None, None, None, headers, timeout, True, True, None)


class Session:
    """A minimal Session: shared default headers, no connection reuse yet."""

    def __init__(self):
        self.headers = {}
        self.params = {}
        self.auth = None

    def _merge(self, headers):
        merged = {}
        for key in self.headers.keys():
            merged[key] = self.headers[key]
        if headers is not None:
            for key in headers.keys():
                merged[key] = headers[key]
        return merged

    def request(self, method, url, params=None, data=None, json=None, headers=None,
                timeout=None, allow_redirects=True):
        return request(method, url, params, data, json, self._merge(headers), timeout,
                       allow_redirects, True, self.auth)

    def get(self, url, params=None, headers=None, timeout=None):
        return self.request("GET", url, params, None, None, headers, timeout)

    def post(self, url, data=None, json=None, headers=None, timeout=None):
        return self.request("POST", url, None, data, json, headers, timeout)

    def close(self):
        return None
