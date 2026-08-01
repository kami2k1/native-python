def fib(n):
    if n < 2:
        return n
    return fib(n - 1) + fib(n - 2)

def integer_loop(n):
    total = 0
    for i in range(n):
        total += i * 2 - 1
    return total

def fdiv_table():
    out = []
    for a in [7, -7]:
        for b in [2, -2, 3, -3]:
            out.append(a // b)
            out.append(a % b)
    return out

print(fib(25))
print(integer_loop(1000000))
print(fdiv_table())
print(1 / 4, 7.5 // 2.0, 2.5 % 1.0)
