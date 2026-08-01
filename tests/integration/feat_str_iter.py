# String iteration through the list-materializing builtins (regression: these
# used to segfault because iter_prep returned a bare str cast as a list).
print(sorted("dbca"))
print(list("hello"))
print(sum([ord(c) for c in "abc"]))
print(sorted(set("mississippi")))
print(max("azby"), min("azby"))
print(pow(2, 10), pow(2, 10, 100), pow(3, 4, 5))
for i, ch in enumerate("xyz", 1):
    print(i, ch)
