# Complex program: sorting, dict analytics, higher-order functions,
# numeric statistics and multi-threaded computation — all in one binary.
import math
import threading

def sort_list(xs):
    i = 1
    while i < len(xs):
        key = xs[i]
        j = i - 1
        while j >= 0 and xs[j] > key:
            xs[j + 1] = xs[j]
            j -= 1
        xs[j + 1] = key
        i += 1
    return xs

def word_freq(words):
    freq = {}
    for w in words:
        freq[w] = freq.get(w, 0) + 1
    return freq

def mapv(f, xs):
    out = []
    for x in xs:
        out.append(f(x))
    return out

def sq(x):
    return x * x

def mean(xs):
    s = 0.0
    for x in xs:
        s += x
    return s / len(xs)

def stddev(xs):
    m = mean(xs)
    var = 0.0
    for x in xs:
        var += (x - m) * (x - m)
    return math.sqrt(var / len(xs))

data = [4, 1, 3, 5, 2]
print(sort_list(data))
words = ["a", "b", "a", "c", "b", "a"]
f = word_freq(words)
print(f["a"], f["b"], f["c"])
print(mapv(sq, [1, 2, 3, 4]))
print(mean([1.0, 2.0, 3.0, 4.0]))
print(stddev([2.0, 4.0, 4.0, 4.0, 5.0, 5.0, 7.0, 9.0]))

# threaded pi estimation (Leibniz series, 4 threads x 25000 terms)
parts = [0.0, 0.0, 0.0, 0.0]

def leibniz(idx, start, count):
    s = 0.0
    k = start
    n = 0
    while n < count:
        term = 1.0 / (2.0 * k + 1.0)
        if k % 2 == 0:
            s += term
        else:
            s -= term
        k += 1
        n += 1
    parts[idx] = s

hs = []
for i in range(4):
    hs.append(threading.spawn(leibniz, i, i * 25000, 25000))
for i in range(4):
    threading.join(hs[i])

pi_est = 0.0
for i in range(4):
    pi_est += parts[i]
pi_est *= 4.0
print(math.fabs(pi_est - math.pi) < 0.001)
print("complex app ok")
