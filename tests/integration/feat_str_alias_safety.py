def build():
    s = ""
    for i in range(5):
        s += "a"
    return s

def mutate_loop():
    lst = [build(), build()]
    for t in lst:
        t += "X"
    print(lst)

def mutate_from_elem():
    lst = [build()]
    s = lst[0]
    s += "?"
    print(lst[0])

def helper(x):
    return x

def mutate_via_call():
    a = build()
    b = helper(a)
    b += "Z"
    print(a)

mutate_loop()
mutate_from_elem()
mutate_via_call()
