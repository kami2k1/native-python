s = "Hello" + ", " + "World"
print(s)
print(len(s))
print(s[0], s[-1])
print(s.upper())
print("ab" * 3)
print("ell" in s)
print("xyz" in s)
print(str(42) + "!")
print(int("123") + 1)
print(float("2.5") * 2)
print(ord("A"), chr(66))
t = ""
for c in "abc":
    t = c + t
print(t)
