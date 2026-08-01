"""TCP sockets: a Python object model over the socket(2) family of syscalls."""
import _kami

AF_INET = 2
AF_INET6 = 10
SOCK_STREAM = 1
SOCK_DGRAM = 2
SOL_SOCKET = 1
SO_REUSEADDR = 2


# Exception names (socket.error / socket.timeout) are raised by name: the
# language matches `except` clauses by intent, not by class identity.
class socket:
    def __init__(self, family=AF_INET, type=SOCK_STREAM, proto=0, fileno=-1):
        if fileno >= 0:
            self.fd = fileno
        else:
            self.fd = _kami.sock_open()
        if self.fd < 0:
            raise OSError("socket(): " + _kami.errmsg())
        self.family = family
        self.closed = False

    def _check(self):
        if self.closed:
            raise OSError("operation on a closed socket")

    def fileno(self):
        return self.fd

    def bind(self, address):
        self._check()
        host = address[0]
        port = address[1]
        if _kami.sock_bind(self.fd, host, port) != 0:
            raise OSError("bind(" + str(host) + ":" + str(port) + "): " + _kami.errmsg())

    def listen(self, backlog=8):
        self._check()
        if _kami.sock_listen(self.fd, backlog) != 0:
            raise OSError("listen(): " + _kami.errmsg())

    def accept(self):
        self._check()
        got = _kami.sock_accept(self.fd)
        if got is None:
            raise OSError("accept(): " + _kami.errmsg())
        conn = socket(self.family, SOCK_STREAM, 0, got[0])
        return (conn, (got[1], got[2]))

    def connect(self, address):
        self._check()
        host = address[0]
        port = address[1]
        if _kami.sock_connect(self.fd, host, port) != 0:
            raise ConnectionError("cannot connect to " + str(host) + ":" + str(port) +
                                  ": " + _kami.errmsg())

    def connect_ex(self, address):
        self._check()
        return _kami.sock_connect(self.fd, address[0], address[1])

    def send(self, data):
        self._check()
        sent = _kami.sock_send(self.fd, data)
        if sent < 0:
            raise OSError("send(): " + _kami.errmsg())
        return sent

    def sendall(self, data):
        self.send(data)

    def recv(self, bufsize):
        self._check()
        data = _kami.sock_recv(self.fd, bufsize)
        if data is None:
            raise OSError("recv(): " + _kami.errmsg())
        return data

    def recv_all(self):
        """Read until the peer closes the connection (a KamiPython extension;
        CPython would use sock.makefile().read())."""
        out = ""
        while True:
            chunk = self.recv(8192)
            if chunk == "":
                return out
            out = out + chunk

    def settimeout(self, seconds):
        self._check()
        if seconds is None:
            seconds = 0
        _kami.sock_timeout(self.fd, seconds)

    def setsockopt(self, level, option, value):
        return 0  # SO_REUSEADDR is already set by the binding layer

    def shutdown(self, how=2):
        self.close()

    def close(self):
        if not self.closed:
            _kami.sock_close(self.fd)
            self.closed = True


def create_connection(address, timeout=None):
    s = socket()
    if timeout is not None:
        s.settimeout(timeout)
    s.connect(address)
    return s
