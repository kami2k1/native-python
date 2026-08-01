"""Modules compiled from the CPython installation found on this machine.

Neither module is shipped with kamipy or implemented in C++: the compiler
discovers the system Python's stdlib and compiles the real source.
"""
import stat
import colorsys

print("S_IFREG", stat.S_IFREG)
print("isdir", stat.S_ISDIR(16877))
print("filemode", stat.filemode(33188))

r, g, b = colorsys.hsv_to_rgb(0.0, 1.0, 1.0)
print("hsv->rgb", r, g, b)
print("rgb->yiq", colorsys.rgb_to_yiq(1.0, 0.0, 0.0)[0])
h, l, s = colorsys.rgb_to_hls(0.0, 0.0, 1.0)
# round() keeps the check independent of float repr differences.
print("rgb->hls", round(h, 6), l, s)
