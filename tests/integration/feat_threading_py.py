# threading is stdlib/threading.py: Thread and Lock are Python classes over the
# _kami thread/mutex primitives (std::thread, std::recursive_mutex). Valid
# CPython, same output.
import threading
import time

counter = 0
lock = threading.Lock()

def worker(n, step):
    global counter
    for i in range(n):
        with lock:
            counter = counter + step

t1 = threading.Thread(target=worker, args=(500, 1))
t2 = threading.Thread(target=worker, args=(500, 2))
print(t1.is_alive())
t1.start()
t2.start()
t1.join()
t2.join()
print(counter, t1.is_alive())

def named(tag, times=1):
    for i in range(times):
        pass
    return tag

t3 = threading.Thread(target=named, args=("x",), kwargs={"times": 3}, name="w3")
t3.start()
t3.join()
print(t3.name, t3.daemon)

lk = threading.Lock()
print(lk.acquire(False), lk.locked())
lk.release()
print(lk.locked())

r = threading.RLock()
r.acquire()
r.release()
print("rlock ok")
