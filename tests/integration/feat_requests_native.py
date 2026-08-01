# Native `requests` client (sockets + TLS in the runtime, no curl/subprocess)
# talking to an HTTP server that is itself written in KamiPython — the whole
# request/response cycle happens inside native code.
import socket
import threading
import time
import json


def http_server(port):
    s = socket.socket()
    s.bind(("127.0.0.1", port))
    s.listen(1)
    ca = s.accept()
    conn = ca[0]
    req = conn.recv(4096)
    body = json.dumps({"ok": True, "n": 42})
    resp = "HTTP/1.1 200 OK\r\nContent-Type: application/json\r\nContent-Length: " + str(len(body)) + "\r\nConnection: close\r\n\r\n" + body
    conn.send(resp)
    conn.close()
    s.close()


import requests

PORT = 8353
t = threading.spawn(http_server, PORT)
time.sleep(0.4)
r = requests.get("http://127.0.0.1:" + str(PORT) + "/api", timeout=5)
print("status:", r.status_code)
print("ok:", r.ok)
d = r.json()
print("n:", d.get("n"))
threading.join(t)

try:
    requests.get("http://127.0.0.1:1/nope", timeout=2)
    print("unreachable: no exception?!")
except Exception:
    print("unreachable: exception raised")
print("requests native ok")
