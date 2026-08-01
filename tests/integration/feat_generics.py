class Box[T]:
    def __init__(self, v):
        self.v = v
    def get(self):
        return self.v

def identity[T](x):
    return x

b = Box("hi")
print(b.get())
print(identity(42))
print(identity([1, 2, 3]))
