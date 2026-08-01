# threading.Lock / RLock are native OS mutexes, and `with` releases them even
# when the statement sits inside a loop or a function.
import threading
import time

# --- context managers inside loops (regression: __exit__ used to be skipped) --
class Tracker:
    def __init__(self, tag):
        self.tag = tag

    def __enter__(self):
        print("enter", self.tag)
        return self

    def __exit__(self, exc_type, exc, tb):
        print("exit", self.tag)


def loop_with(n):
    i = 0
    while i < n:
        with Tracker(i):
            pass
        i = i + 1


loop_with(3)
for k in range(2):
    with Tracker("for" + str(k)):
        pass


def raising():
    try:
        with Tracker("boom"):
            raise ValueError("inner")
    except Exception:
        print("caught after exit")


raising()

# `with open(...)` inside a loop must actually close each file
i = 0
while i < 3:
    with open("lockfile_tmp.txt", "w") as f:
        f.write("iteration " + str(i))
    i = i + 1
with open("lockfile_tmp.txt", "r") as f:
    print("file says:", f.read())
import os
os.remove("lockfile_tmp.txt")

# --- mutual exclusion across real OS threads --------------------------------
lock = threading.Lock()
counter = [0]


def bump(times):
    i = 0
    while i < times:
        with lock:
            counter[0] = counter[0] + 1
        i = i + 1


threads = []
k = 0
while k < 4:
    threads.append(threading.spawn(bump, 2000))
    k = k + 1
for t in threads:
    threading.join(t)
print("counter:", counter[0])

# without the lock the same program loses updates; with it, never.
print("exact:", counter[0] == 8000)

# --- the Lock API -----------------------------------------------------------
l2 = threading.Lock()
print("locked before:", l2.locked())
print("acquire:", l2.acquire())
print("locked after:", l2.locked())
l2.release()
print("locked released:", l2.locked())

r = threading.RLock()
r.acquire()
r.acquire()
r.release()
r.release()
print("rlock reentrant ok")
print("done")
