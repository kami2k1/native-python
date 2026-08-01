"""Threads and locks: the CPython `threading` API over native OS threads.

`Thread` and `Lock` are Python classes here; the only native pieces are the
primitives in `_kami` (std::thread / std::recursive_mutex, which are themselves
thin wrappers over pthreads and CreateThread).

Note on the GIL: compiled code still takes one runtime lock around every runtime
call, released around blocking operations (sleep, join, accept, recv, Lock.acquire).
Threads therefore interleave and block independently, but CPU-bound Python code
does not run on two cores at once. See docs/*/ARCHITECTURE.md.
"""
import _kami


def _runner(target, args, kwargs):
    """A zero-argument closure, so the spawn primitive only needs one value."""
    def go():
        target(*args, **kwargs)
    return go


class Thread:
    def __init__(self, target=None, args=None, kwargs=None, name=None, daemon=None):
        if target is None:
            raise TypeError("Thread(target=...) is required")
        self._target = target
        self._args = args if args is not None else []
        self._kwargs = kwargs if kwargs is not None else {}
        self.name = name if name is not None else "Thread"
        # daemon threads are accepted but not honoured: the runtime joins every
        # live thread at shutdown so output is never truncated.
        self.daemon = daemon if daemon is not None else False
        self._handle = None

    def start(self):
        if self._handle is not None:
            raise RuntimeError("threads can only be started once")
        self._handle = _kami.thread_spawn(_runner(self._target, self._args, self._kwargs))

    def join(self, timeout=None):
        if self._handle is None:
            raise RuntimeError("cannot join a thread before it is started")
        _kami.thread_join(self._handle)

    def is_alive(self):
        if self._handle is None:
            return False
        return _kami.thread_alive(self._handle)

    def run(self):
        self._target(*self._args, **self._kwargs)


class Lock:
    """A mutex. Recursive at the OS level, so it doubles as RLock."""

    def __init__(self):
        self._h = _kami.mutex_new(0)
        self._held = False

    def acquire(self, blocking=True, timeout=-1):
        if not blocking:
            got = _kami.mutex_trylock(self._h) == 1
            if got:
                self._held = True
            return got
        _kami.mutex_lock(self._h)
        self._held = True
        return True

    def release(self):
        if not self._held:
            raise RuntimeError("release() on an unlocked lock")
        self._held = False
        _kami.mutex_unlock(self._h)

    def locked(self):
        return self._held

    def __enter__(self):
        self.acquire()
        return self

    def __exit__(self):
        self.release()


class RLock(Lock):
    pass


def current_thread():
    return _kami.getpid()


# --- KamiPython extension ----------------------------------------------------
# spawn()/join() are the low-level pair the compiled runtime has always exposed;
# they stay because programs and tests in this repository use them.

def spawn(target, a=None, b=None, c=None, d=None):
    args = []
    for value in [a, b, c, d]:
        if value is None:
            break
        args.append(value)
    return _kami.thread_spawn(_runner(target, args, {}))


def join(handle):
    _kami.thread_join(handle)
