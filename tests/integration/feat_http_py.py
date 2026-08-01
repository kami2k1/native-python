# socket.py and requests.py are Python modules over the socket(2) bindings.
# A compiled HTTP server and a compiled HTTP client talk to each other in the
# same binary — no curl, no libcurl, no C++ protocol code.
import json
import requests
import socket
import threading
import time

PORT = 8467


def serve_once(port):
    srv = socket.socket()
    srv.bind(("127.0.0.1", port))
    srv.listen(1)
    for _ in range(2):
        pair = srv.accept()
        conn = pair[0]
        request = conn.recv(4096)
        head = request.split("\r\n")[0]
        if head.find("POST") == 0:
            marker = request.find("\r\n\r\n")
            body = request[marker + 4:]
            payload = "{\"echo\": " + body + "}"
        else:
            payload = "{\"path\": \"" + head.split(" ")[1] + "\"}"
        conn.send("HTTP/1.1 200 OK\r\nContent-Type: application/json\r\n" +
                  "Content-Length: " + str(len(payload)) + "\r\n\r\n" + payload)
        conn.close()
    srv.close()


t = threading.spawn(serve_once, PORT)
time.sleep(0.4)

base = "http://127.0.0.1:" + str(PORT)
r = requests.get(base + "/status")
print(r.status_code, r.ok, r.reason)
print(r.headers["content-type"])
print(r.json()["path"])

r2 = requests.post(base + "/echo", json={"n": 7, "tag": "ok"})
print(r2.status_code, r2.json()["echo"]["n"], r2.json()["echo"]["tag"])

threading.join(t)

# error paths
try:
    requests.get("https://example.invalid/")
except Exception as e:
    print("https guarded")
try:
    requests.get("ftp://example.invalid/")
except Exception as e:
    print("scheme guarded")
try:
    requests.get("http://127.0.0.1:9/nothing", timeout=1)
except Exception as e:
    print("connection error raised")

# raw sockets still work on their own
a = socket.socket()
print(a.fileno() >= 0)
a.close()
print("http ok")
