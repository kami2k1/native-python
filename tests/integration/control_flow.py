x = 15
if x > 10:
    print("big")
elif x > 5:
    print("medium")
else:
    print("small")

n = 0
while n < 5:
    n += 1
    if n == 3:
        continue
    if n == 5:
        break
    print(n)
print("done", n)
