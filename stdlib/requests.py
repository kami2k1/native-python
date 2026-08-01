"""A small HTTP/1.1 client written on top of stdlib/socket.py.

No curl, no libcurl, no hand-written C++ HTTP stack: the protocol is spelled out
in Python here and compiled to native code. TLS is out of scope, so https:// URLs
raise instead of silently falling back to plaintext.
"""
import json
import socket

DEFAULT_USER_AGENT = "kamipython-requests/1.0"


class Response:
    def __init__(self, status_code, reason, headers, text, url):
        self.status_code = status_code
        self.reason = reason
        self.headers = headers
        self.text = text
        self.content = text
        self.url = url
        self.ok = status_code >= 200 and status_code < 400

    def json(self):
        return json.loads(self.text)

    def raise_for_status(self):
        if self.status_code >= 400:
            raise RequestException("HTTP " + str(self.status_code) + " for " + self.url)


def _split_url(url):
    scheme = "http"
    rest = url
    marker = url.find("://")
    if marker >= 0:
        scheme = url[0:marker].lower()
        rest = url[marker + 3:]
    if scheme == "https":
        raise RequestException(
            "https is not supported (no TLS backend): " + url)
    if scheme != "http":
        raise RequestException("unsupported URL scheme '" + scheme + "'")
    slash = rest.find("/")
    if slash < 0:
        authority = rest
        path = "/"
    else:
        authority = rest[0:slash]
        path = rest[slash:]
    port = 80
    colon = authority.find(":")
    host = authority
    if colon >= 0:
        host = authority[0:colon]
        port = int(authority[colon + 1:])
    return (host, port, path)


def _header_lines(headers):
    out = ""
    if headers is None:
        return out
    for key in headers.keys():
        out = out + key + ": " + str(headers[key]) + "\r\n"
    return out


def _json_body(payload):
    return json.dumps(payload)


def _parse_head(head):
    lines = head.split("\r\n")
    status_line = lines[0]
    status = 0
    reason = ""
    space = status_line.find(" ")
    if space >= 0:
        rest = status_line[space + 1:]
        second = rest.find(" ")
        if second < 0:
            status = int(rest)
        else:
            status = int(rest[0:second])
            reason = rest[second + 1:]
    headers = {}
    i = 1
    while i < len(lines):
        line = lines[i]
        i += 1
        if line == "":
            continue
        colon = line.find(":")
        if colon < 0:
            continue
        headers[line[0:colon].lower()] = line[colon + 1:].strip()
    return (status, reason, headers)


def _dechunk(body):
    out = ""
    i = 0
    n = len(body)
    while i < n:
        end = -1
        j = i
        while j + 1 < n:
            if body[j] == "\r" and body[j + 1] == "\n":
                end = j
                break
            j += 1
        if end < 0:
            return out
        header = body[i:end]
        semi = header.find(";")
        if semi >= 0:
            header = header[0:semi]
        size = int(header.strip(), 16) if header.strip() != "" else 0
        i = end + 2
        if size == 0:
            return out
        out = out + body[i:i + size]
        i = i + size + 2
    return out


def request(method, url, data=None, headers=None, timeout=None, body=None):
    host, port, path = _split_url(url)
    payload = body if body is not None else data
    if payload is None:
        payload = ""
    lines = method + " " + path + " HTTP/1.1\r\n"
    lines = lines + "Host: " + host + "\r\n"
    lines = lines + "User-Agent: " + DEFAULT_USER_AGENT + "\r\n"
    lines = lines + "Accept: */*\r\n"
    lines = lines + "Connection: close\r\n"
    lines = lines + _header_lines(headers)
    if payload != "":
        lines = lines + "Content-Length: " + str(len(payload)) + "\r\n"
    lines = lines + "\r\n" + payload

    conn = socket.socket()
    if timeout is not None:
        conn.settimeout(timeout)
    try:
        conn.connect((host, port))
        conn.sendall(lines)
        raw = conn.recv_all()
    except Exception as exc:
        conn.close()
        raise RequestException("request to " + url + " failed: " + str(exc))
    conn.close()

    split = raw.find("\r\n\r\n")
    if split < 0:
        raise RequestException("malformed response from " + url)
    status, reason, resp_headers = _parse_head(raw[0:split])
    content = raw[split + 4:]
    encoding = resp_headers.get("transfer-encoding")
    if encoding is not None and encoding.lower() == "chunked":
        content = _dechunk(content)
    return Response(status, reason, resp_headers, content, url)


def get(url, headers=None, timeout=None, params=None, verify=True):
    if params is not None:
        query = ""
        for key in params.keys():
            query = query + ("&" if query != "" else "") + key + "=" + str(params[key])
        if query != "":
            url = url + ("&" if url.find("?") >= 0 else "?") + query
    return request("GET", url, None, headers, timeout)


def post(url, data=None, json=None, headers=None, timeout=None, verify=True):
    body = data
    if json is not None:
        body = _json_body(json)
        if headers is None:
            headers = {}
        headers["Content-Type"] = "application/json"
    return request("POST", url, None, headers, timeout, body)


def put(url, data=None, headers=None, timeout=None):
    return request("PUT", url, data, headers, timeout)


def delete(url, headers=None, timeout=None):
    return request("DELETE", url, None, headers, timeout)


def head(url, headers=None, timeout=None):
    return request("HEAD", url, None, headers, timeout)
