# Standard library `_operator` fallback for KamiPython AOT compiler.

def itemgetter(*items):
    if len(items) == 1:
        item = items[0]
        def g(obj):
            return obj[item]
        return g
    def g(obj):
        return tuple(obj[i] for i in items)
    return g

def attrgetter(*attrs):
    def g(obj):
        for attr in attrs:
            obj = getattr(obj, attr)
        return obj
    return g

def eq(a, b): return a == b
def ne(a, b): return a != b
def lt(a, b): return a < b
def le(a, b): return a <= b
def gt(a, b): return a > b
def ge(a, b): return a >= b
def add(a, b): return a + b
def sub(a, b): return a - b
def mul(a, b): return a * b
def truediv(a, b): return a / b
def floordiv(a, b): return a // b
def mod(a, b): return a % b
