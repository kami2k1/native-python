"""ctypes / cffi: Python-looking calls compiled into direct C calls."""
import ctypes

libm = ctypes.CDLL("libm.so.6")
libm.sqrt.restype = ctypes.c_double
libm.sqrt.argtypes = [ctypes.c_double]
print("sqrt", libm.sqrt(2.25))

libm.atan2.restype = ctypes.c_double
libm.atan2.argtypes = [ctypes.c_double, ctypes.c_double]
print("atan2 is zero", libm.atan2(0.0, 1.0))

# CDLL(None) is "this process": libc, already linked in.
libc = ctypes.CDLL(None)
libc.abs.restype = ctypes.c_int
libc.abs.argtypes = [ctypes.c_int]
print("abs", libc.abs(-41))

libc.strlen.restype = ctypes.c_size_t
libc.strlen.argtypes = [ctypes.c_char_p]
print("strlen", libc.strlen("kamipython"))

libc.getenv.restype = ctypes.c_char_p
libc.getenv.argtypes = [ctypes.c_char_p]
print("missing env", libc.getenv("KAMIPY_NO_SUCH_VARIABLE_XYZ"))

import cffi
ffi = cffi.FFI()
ffi.cdef("double cbrt(double x); int toupper(int c);")
lib = ffi.dlopen("libm.so.6")
print("cbrt", lib.cbrt(27.0))
