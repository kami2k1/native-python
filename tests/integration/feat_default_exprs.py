# Non-literal default parameter values: evaluated in the callee's prologue
# (definition scope) when the caller omits the argument.
class ThreadType:
    def __init__(self, name):
        self.name = name


USER = ThreadType("USER")
GROUP = ThreadType("GROUP")
BASE = 10


def send(msg, thread_type=USER, retries=BASE + 5):
    return msg + " -> " + thread_type.name + " x" + str(retries)


def greet(name="kami", punct="!"):
    return "hi " + name + punct


print(send("a"))
print(send("b", GROUP))
print(send("c", thread_type=GROUP, retries=2))
print(greet())
print(greet(punct="?"))
