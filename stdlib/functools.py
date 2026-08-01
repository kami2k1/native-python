def reduce(function, iterable, initializer=None):
    items = list(iterable)
    if initializer is None:
        if len(items) == 0:
            raise TypeError("reduce() of empty iterable with no initial value")
        acc = items[0]
        start = 1
    else:
        acc = initializer
        start = 0
    i = start
    while i < len(items):
        acc = function(acc, items[i])
        i = i + 1
    return acc
