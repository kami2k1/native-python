# Threading test: 4 native threads each compute a partial sum in parallel,
# write into a shared list, then the main thread joins and aggregates.
import threading

results = [0, 0, 0, 0]

def worker(idx, n):
    total = 0
    for i in range(n):
        total += i
    results[idx] = total

threads = []
for t in range(4):
    threads.append(threading.spawn(worker, t, 10000))

for t in range(4):
    threading.join(threads[t])

grand = 0
for t in range(4):
    grand += results[t]

print(grand)
print("threads ok")
