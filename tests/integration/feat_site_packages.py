# Third-party packages: compile pure-Python site-packages modules when
# possible, bridge everything else through the embedded CPython — including
# subclassing a bridged class with compiled method overrides (bot SDKs).
from purelib import GREETING, double
from fakesdk import Robot, helper

print(GREETING, double(21))          # compiled from site-packages source
print(helper([1, 2, 3]))             # bridged function, positional
print(helper([1, 2, 3], factor=10))  # bridged function, kwargs

class Bot(Robot):
    def __init__(self, name, tag):
        super().__init__(name, power=7, tag=tag)
        self.seen = 0

    def on_event(self, i, payload):
        self.seen += 1
        print(self.tag, "event", i, payload, "power", self.power)

b = Bot("kami", "BOT-1")
b.run(2)
print("seen:", b.seen)
print("site-packages ok")
