print(True and False, True or False)
print(not True)
print(1 == 1.0)
print("a" < "b")
x = 0
y = x or 5
z = x and 5
print(y, z)

def side(v):
    print("eval", v)
    return v

r = side(0) and side(1)
print(r)
r2 = side(1) or side(2)
print(r2)
print(None == None)
