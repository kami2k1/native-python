"""Operating-system services.

Every function here is a few lines of Python over the raw C-ABI bindings in
`_kami` (which are themselves one-line forwards to libc). Nothing about "os" is
implemented in C++.
"""
import _kami
import os.path

sep = "\\" if _kami.platform() == "win32" else "/"
linesep = "\r\n" if _kami.platform() == "win32" else "\n"
name = "nt" if _kami.platform() == "win32" else "posix"
curdir = "."
pardir = ".."

SEEK_SET = 0
SEEK_CUR = 1
SEEK_END = 2


def _fail(what, target):
    raise OSError(what + " '" + target + "': " + _kami.errmsg())


def getcwd():
    return _kami.getcwd()


def chdir(path):
    if _kami.chdir(path) != 0:
        _fail("cannot chdir to", path)


def listdir(path="."):
    entries = _kami.listdir(path)
    if entries is None:
        _fail("cannot list directory", path)
    entries.sort()
    return entries


def mkdir(path):
    if _kami.mkdir(path) != 0:
        _fail("cannot create directory", path)


def makedirs(path, exist_ok=False):
    if _kami.stat(path) == 1:
        if exist_ok:
            return
        _fail("directory exists:", path)
    parent = os.path.dirname(path)
    if parent != "" and parent != path and _kami.stat(parent) < 0:
        makedirs(parent, True)
    if _kami.mkdir(path) != 0:
        _fail("cannot create directory", path)


def rmdir(path):
    if _kami.rmdir(path) != 0:
        _fail("cannot remove directory", path)


def remove(path):
    if _kami.unlink(path) != 0:
        _fail("cannot remove file", path)


def unlink(path):
    remove(path)


def rename(src, dst):
    if _kami.rename(src, dst) != 0:
        _fail("cannot rename", src)


def replace(src, dst):
    if _kami.stat(dst) == 0:
        _kami.unlink(dst)
    rename(src, dst)


def system(command):
    return _kami.system(command)


def getenv(key, default=None):
    value = _kami.getenv(key)
    if value is None:
        return default
    return value


def getpid():
    return _kami.getpid()


# ---- raw file descriptors (os.read/os.write/os.close/os.lseek) -------------
# Nothing but the syscall bindings; `open()` (the builtin) returns a buffered
# file object instead.

def read(fd, n):
    return _kami.fd_read(fd, n)


def write(fd, data):
    n = _kami.fd_write(fd, data)
    if n < 0:
        raise OSError("write failed: " + _kami.errmsg())
    return n


def close(fd):
    _kami.fd_close(fd)


def lseek(fd, offset, whence=0):
    return _kami.fd_seek(fd, offset, whence)
