# `requests` is now a pure-Python module (runtime/pylib/requests.py) speaking
# HTTP/1.1 over a socket. The server below is also KamiPython.
import socket
import threading
import time
import json
import requests

# --- URL / query-string helpers are plain Python now --------------------
print(requests.urlparse("https://host.example:8443/a/b?q=1"))
print(requests.urlparse("http://host/"))
print(requests.urlparse("https://user:pw@host/x"))
print(requests.quote("a b/c?d"))
print(requests.urlencode({"a": "1 2", "b": "x&y"}))


def one_shot(port, body, extra):
    s = socket.socket()
    s.bind(("127.0.0.1", port))
    s.listen(1)
    ca = s.accept()
    conn = ca[0]
    conn.recv(8192)
    resp = ("HTTP/1.1 200 OK\r\nContent-Type: application/json\r\n" + extra +
            "Content-Length: " + str(len(body)) + "\r\nConnection: close\r\n\r\n" + body)
    conn.send(resp)
    conn.close()
    s.close()


def chunked_server(port):
    s = socket.socket()
    s.bind(("127.0.0.1", port))
    s.listen(1)
    ca = s.accept()
    conn = ca[0]
    conn.recv(8192)
    head = "HTTP/1.1 200 OK\r\nTransfer-Encoding: chunked\r\nConnection: close\r\n\r\n"
    conn.send(head + "5\r\nhello\r\n6\r\n world\r\n0\r\n\r\n")
    conn.close()
    s.close()


PORT = 8461
t = threading.spawn(one_shot, PORT, json.dumps({"ok": True, "n": 42}), "X-Trace: abc\r\n")
time.sleep(0.4)
r = requests.get("http://127.0.0.1:" + str(PORT) + "/api", timeout=5)
threading.join(t)
print("status", r.status_code, r.reason, r.ok)
print("json", r.json())
print("header exact", r.headers.get("Content-Type"))
print("header folded", r.headers.get("x-trace"))
print("missing header", r.headers.get("nope", "-"))
print("repr", str(r))
print("raise_for_status returns", r.raise_for_status())

t2 = threading.spawn(chunked_server, PORT + 1)
time.sleep(0.4)
r2 = requests.get("http://127.0.0.1:" + str(PORT + 1) + "/chunked", timeout=5)
threading.join(t2)
print("chunked body:", r2.text)

try:
    requests.get("http://127.0.0.1:1/nope", timeout=2)
    print("no exception?!")
except Exception as e:
    print("connect error raised")

try:
    requests.get("ftp://host/x")
except Exception as e:
    print("bad scheme raised")

print("requests py ok")
