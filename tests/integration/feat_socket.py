import socket
import threading
import time

def server(port):
    s = socket.socket()
    s.bind(("127.0.0.1", port))
    s.listen(1)
    ca = s.accept()
    conn = ca[0]
    data = conn.recv(1024)
    conn.send("echo:" + data)
    conn.close()
    s.close()

PORT = 8351
t = threading.spawn(server, PORT)
time.sleep(0.4)
c = socket.socket()
c.connect(("127.0.0.1", PORT))
c.send("ping")
print(c.recv(1024))
c.close()
threading.join(t)
print("socket ok")
