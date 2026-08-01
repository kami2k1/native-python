# sys is stdlib/sys.py now: argv, platform, and stdout/stderr streams over the
# C-ABI layer. sys.stdout.write() shares stdio's buffer with print(), so output
# order is the same as CPython's.
import sys

print(len(sys.argv) >= 1, isinstance(sys.argv, list))
print(sys.platform in ["linux", "win32", "darwin"], sys.maxsize > 1000000)
sys.stdout.write("written straight to fd 1\n")
print("printed after that write")
print(sys.getdefaultencoding(), sys.byteorder)
print(sys.stdout.fileno(), sys.stderr.fileno(), sys.stdout.isatty() in [True, False])
n = sys.stdout.write("counted\n")
print(n)
