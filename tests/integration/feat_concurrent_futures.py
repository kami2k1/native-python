# concurrent.futures compiled straight from the machine's CPython source
# (concurrent/futures/__init__.py, _base.py, thread.py + queue/heapq):
# ThreadPoolExecutor, submit, as_completed, wait, Executor.map, and
# exception propagation through futures.
import concurrent.futures
from concurrent.futures import ThreadPoolExecutor, as_completed, wait

def work(n):
    return n * n

def boom(x):
    raise ValueError("bad " + str(x))

def main():
    with ThreadPoolExecutor(max_workers=4) as ex:
        futs = [ex.submit(work, i) for i in range(8)]
        got = sorted([f.result() for f in as_completed(futs)])
        print(got)

        done, not_done = wait([ex.submit(work, i) for i in range(5)], timeout=30)
        print(len(done), len(not_done))

        print(list(ex.map(work, [1, 2, 3])))

        f = ex.submit(boom, 7)
        try:
            f.result()
            print("unreachable")
        except ValueError:
            print("propagated")

main()
print("futures ok")
