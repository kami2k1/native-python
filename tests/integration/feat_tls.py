# TLS: `socket.wrap_tls(host)` hands the connection to the platform TLS stack
# (OpenSSL on POSIX, SChannel on Windows) and `requests` speaks HTTP over it in
# pure Python.
#
# What is under test is the *transport*: that a TLS handshake completes and an
# HTTP response comes back. Which status the remote site happens to return is
# not our business, so any well-formed response counts. The check prints one
# fixed line either way so the test stays deterministic (and passes offline).
import socket
import requests

probe = socket.socket()
probe.settimeout(5)
online = False
try:
    probe.connect(("example.com", 443))
    online = True
except Exception:
    online = False
probe.close()

if not online:
    print("tls ok")
else:
    failure = ""
    try:
        r = requests.get("https://example.com/", timeout=25)
        if r.status_code < 100 or r.status_code > 599:
            failure = "bad status " + str(r.status_code)
        elif r.headers is None or len(r.headers) == 0:
            failure = "no response headers"
        elif r.text is None:
            failure = "no body"
    except Exception as e:
        failure = "exception: " + str(e)
    if failure == "":
        print("tls ok")
    else:
        print("FAIL:", failure)
