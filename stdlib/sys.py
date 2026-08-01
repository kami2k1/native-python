"""Interpreter-level services, as Python over the C-ABI layer.

There is no interpreter here, so the values describe the compiled program: argv
comes from main(), and stdout/stderr are file objects over the standard
descriptors that share stdio's buffer with print().
"""
import _kami

argv = _kami.argv()
platform = _kami.platform()
maxsize = 9223372036854775807
byteorder = "little"
version = "3.11 (KamiPython, compiled)"
version_info = (3, 11, 0)
executable = argv[0] if len(argv) > 0 else ""
path = []
modules = {}


class _Stream:
    """A write-only text stream on a file descriptor."""

    def __init__(self, fd, name):
        self.fd = fd
        self.name = name
        self.encoding = "utf-8"

    def write(self, text):
        return _kami.fd_write(self.fd, str(text))

    def writelines(self, lines):
        for line in lines:
            self.write(line)

    def flush(self):
        pass

    def isatty(self):
        return False

    def fileno(self):
        return self.fd


stdout = _Stream(1, "<stdout>")
stderr = _Stream(2, "<stderr>")


def exit(code=0):
    _kami.exit(code)


def getdefaultencoding():
    return "utf-8"


def getrecursionlimit():
    return 1000


def setrecursionlimit(limit):
    pass  # the native stack is what it is
