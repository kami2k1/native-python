# A stand-in for a pip-installed SDK that kamipy cannot compile (walrus):
# resolution must fall back to the embedded CPython bridge (KT_PYOBJ).
class Robot:
    def __init__(self, name, power=1, tag=None):
        self.name = name
        self.power = (_p := power)
        self.tag = tag

    def on_event(self, i, payload):
        raise NotImplementedError

    def run(self, n=3):
        for i in range(n):
            self.on_event(i, "evt-" + str(i))

def helper(x, factor=2):
    return [v * factor for v in x]
