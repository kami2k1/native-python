# ctypes compiled to direct C calls: `lib.f(x)` becomes `call @f(...)` in the
# emitted LLVM IR, with argtypes/restype driving the marshalling. The C source
# next to this file is compiled and linked into the same executable.
#
# Not run under CPython as-is: real ctypes needs b"..." for char* arguments,
# and CDLL(None) is POSIX-only.
import ctypes

lib = ctypes.CDLL(None)  # symbols already linked into this program

lib.kami_demo_add.argtypes = [ctypes.c_int, ctypes.c_int]
lib.kami_demo_add.restype = ctypes.c_int
lib.kami_demo_hypot2.argtypes = [ctypes.c_double, ctypes.c_double]
lib.kami_demo_hypot2.restype = ctypes.c_double
lib.kami_demo_len.argtypes = [ctypes.c_char_p]
lib.kami_demo_len.restype = ctypes.c_long
lib.kami_demo_name.argtypes = []
lib.kami_demo_name.restype = ctypes.c_char_p
lib.kami_demo_say.argtypes = [ctypes.c_char_p]
lib.kami_demo_say.restype = None

print(lib.kami_demo_add(2, 3), lib.kami_demo_add(-4, 4))
print(lib.kami_demo_hypot2(3.0, 4.0))
print(lib.kami_demo_len("hello"), lib.kami_demo_len(""))
print(lib.kami_demo_name())
lib.kami_demo_say("kami")

# the C standard library is reachable the same way
libc = ctypes.CDLL(None)
libc.strlen.argtypes = [ctypes.c_char_p]
libc.strlen.restype = ctypes.c_size_t
libc.atoi.argtypes = [ctypes.c_char_p]
libc.atoi.restype = ctypes.c_int
print(libc.strlen("abcd"), libc.atoi("  -42"))

# values flow through Python as usual
total = 0
for i in range(5):
    total = lib.kami_demo_add(total, i)
print(total)
print(int(lib.kami_demo_hypot2(1.5, 2.0) * 2))
