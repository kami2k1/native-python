# Real project: Sieve of Eratosthenes up to 100000 (large list, tight loops).
n = 100000
sieve = []
for i in range(n + 1):
    sieve.append(True)
sieve[0] = False
sieve[1] = False
i = 2
while i * i <= n:
    if sieve[i]:
        j = i * i
        while j <= n:
            sieve[j] = False
            j += i
    i += 1

count = 0
s1000 = 0
largest = 0
for i in range(n + 1):
    if sieve[i]:
        count += 1
        largest = i
        if i < 1000:
            s1000 += i

print(count)
print(s1000)
print(largest)
