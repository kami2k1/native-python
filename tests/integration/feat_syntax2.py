# Ellipsis in annotations/stubs, bytes literal accepted, __file__, paren imports
from math import (sin, cos, pi)


def f(x: tuple[int, ...]) -> int:
    return len(x)


class Stub:
    def method(self) -> int: ...


data = b"bytes"
print(f((1, 2, 3, 4)))
print(round(sin(0.0), 1), round(cos(0.0), 1), round(pi, 2))
print("bytes len:", len(data))
print("file is str:", isinstance(__file__, str))
