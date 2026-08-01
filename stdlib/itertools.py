def count(start=0, step=1):
    # bounded materialization is impossible; callers use it with islice/zip.
    # We provide a large finite range as a practical approximation.
    result = []
    n = start
    i = 0
    while i < 1000000:
        result.append(n)
        n = n + step
        i = i + 1
    return result

def repeat(obj, times):
    result = []
    i = 0
    while i < times:
        result.append(obj)
        i = i + 1
    return result

def chain(a, b):
    result = []
    for x in a:
        result.append(x)
    for x in b:
        result.append(x)
    return result

def accumulate(iterable):
    result = []
    total = None
    first = True
    for x in iterable:
        if first:
            total = x
            first = False
        else:
            total = total + x
        result.append(total)
    return result

def combinations(iterable, r):
    pool = list(iterable)
    n = len(pool)
    result = []
    if r > n:
        return result
    indices = list(range(r))
    result.append([pool[i] for i in indices])
    while True:
        found = -1
        i = r - 1
        while i >= 0:
            if indices[i] != i + n - r:
                found = i
                break
            i = i - 1
        if found == -1:
            return result
        indices[found] = indices[found] + 1
        j = found + 1
        while j < r:
            indices[j] = indices[j - 1] + 1
            j = j + 1
        result.append([pool[i] for i in indices])

def permutations(iterable, r):
    # Iterative algorithm (indices + cycles), matching CPython's itertools.
    pool = list(iterable)
    n = len(pool)
    result = []
    if r > n:
        return result
    indices = list(range(n))
    cycles = list(range(n, n - r, -1))
    result.append([pool[indices[i]] for i in range(r)])
    while n > 0:
        moved = False
        i = r - 1
        while i >= 0:
            cycles[i] = cycles[i] - 1
            if cycles[i] == 0:
                first = indices[i]
                j = i
                while j < n - 1:
                    indices[j] = indices[j + 1]
                    j = j + 1
                indices[n - 1] = first
                cycles[i] = n - i
            else:
                k = n - cycles[i]
                tmp = indices[i]
                indices[i] = indices[k]
                indices[k] = tmp
                result.append([pool[indices[x]] for x in range(r)])
                moved = True
                break
            i = i - 1
        if not moved:
            return result
    return result

def combinations_with_replacement(iterable, r):
    pool = list(iterable)
    n = len(pool)
    result = []
    if n == 0 and r > 0:
        return result
    indices = [0 for i in range(r)]
    result.append([pool[i] for i in indices])
    while True:
        found = -1
        i = r - 1
        while i >= 0:
            if indices[i] != n - 1:
                found = i
                break
            i = i - 1
        if found == -1:
            return result
        val = indices[found] + 1
        j = found
        while j < r:
            indices[j] = val
            j = j + 1
        result.append([pool[i] for i in indices])

def product(a, b=None, repeat=1):
    if b is None:
        pools = []
        i = 0
        while i < repeat:
            pools.append(list(a))
            i = i + 1
    else:
        pools = [list(a), list(b)]
    result = [[]]
    for pool in pools:
        newresult = []
        for prefix in result:
            for item in pool:
                newresult.append(prefix + [item])
        result = newresult
    return result

def pairwise(iterable):
    items = list(iterable)
    result = []
    i = 0
    while i + 1 < len(items):
        result.append([items[i], items[i + 1]])
        i = i + 1
    return result

def zip_longest(a, b, fillvalue=None):
    la = list(a)
    lb = list(b)
    n = len(la)
    if len(lb) > n:
        n = len(lb)
    result = []
    i = 0
    while i < n:
        x = la[i] if i < len(la) else fillvalue
        y = lb[i] if i < len(lb) else fillvalue
        result.append([x, y])
        i = i + 1
    return result

def starmap(function, iterable):
    result = []
    for args in iterable:
        result.append(function(args[0], args[1]))
    return result

def islice(iterable, stop):
    result = []
    i = 0
    for x in iterable:
        if i >= stop:
            break
        result.append(x)
        i = i + 1
    return result
