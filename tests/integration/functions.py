def fib(n):
    if n < 2:
        return n
    return fib(n - 1) + fib(n - 2)

def greet(name):
    return "hi " + name

def noret():
    pass

def apply(f, x):
    return f(x)

def double(v):
    return v * 2

print(fib(20))
print(greet("kami"))
print(noret())
print(apply(double, 21))
