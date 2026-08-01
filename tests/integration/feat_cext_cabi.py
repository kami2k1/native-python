"""C extension modules bound to C-ABI primitives, and ctypes/cffi native calls."""
import _math
import _struct
import _socket
import _os

# _math members go straight to libm: `call double @sqrt(double)`.
print("sqrt", _math.sqrt(16.0))
print("pow", _math.pow(2.0, 10.0))
print("hypot", _math.hypot(3.0, 4.0))
print("fmod", _math.fmod(7.0, 3.0))
# ...while the members libm has no equivalent for reuse runtime primitives.
print("factorial", _math.factorial(6))
print("isqrt", _math.isqrt(17))

# _struct: real byte-level packing implemented in runtime/src/syscalls.cpp.
print("calcsize", _struct.calcsize("<hi"))
print("roundtrip", _struct.unpack("<hi", _struct.pack("<hi", 7, 300)))
print("bigendian", _struct.unpack(">i", _struct.pack(">i", 66051)))
print("double", _struct.unpack("<d", _struct.pack("<d", 0.5)))
print("signed", _struct.unpack("<b", _struct.pack("<b", -3)))

# _socket byte-order helpers are uint16_t/uint32_t in C: 32-bit registers.
print("htons", _socket.htons(4660))
print("ntohl", _socket.ntohl(_socket.htonl(305419896)))

# _os (CPython calls it posix/nt) descriptor I/O: open/write/close/read/unlink are real syscalls.
fd = _os.open("cext_tmp.txt", 65, 420)
_os.write(fd, "raw syscall\n")
_os.close(fd)
fd = _os.open("cext_tmp.txt", 0)
print("readback", _os.read(fd, 32).strip())
_os.close(fd)
_os.unlink("cext_tmp.txt")
print("O_CREAT", _os.O_CREAT)

# from-imports of C extension members work the same way.
from _math import sqrt, factorial
print("from-import", sqrt(2.25), factorial(5))
