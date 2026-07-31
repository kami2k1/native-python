# Real project: concurrent workers updating disjoint slots of a shared list
# (each account is owned by exactly one thread => deterministic).
import threading

accounts = [1000, 1000, 1000, 1000, 1000, 1000]

def worker(idx, rounds):
    i = 0
    while i < rounds:
        accounts[idx] = accounts[idx] + 1
        i += 1

ts = []
for i in range(6):
    ts.append(threading.spawn(worker, i, 5000))
for i in range(6):
    threading.join(ts[i])

total = 0
for i in range(6):
    total += accounts[i]
print(total)
print(accounts)
