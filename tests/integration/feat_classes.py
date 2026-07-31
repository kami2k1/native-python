# Classes: __init__, methods, fields, inheritance, class attrs, kwargs, defaults.
class Shape:
    kind = "shape"

    def __init__(self, name):
        self.name = name

    def describe(self):
        return self.name + " is a " + Shape.kind

class Circle(Shape):
    def __init__(self, r):
        Shape.__init__(self, "circle")
        self.r = r

    def area(self):
        return 3.14159 * self.r * self.r

    def scaled(self, factor=2):
        return Circle(self.r * factor)

s = Shape("blob")
print(s.describe())
c = Circle(10)
print(c.describe())
print(c.area() > 314.0, c.area() < 314.2)
big = c.scaled()
print(big.r)
huge = c.scaled(10)
print(huge.r)
print(isinstance(c, Circle), isinstance(c, Shape), isinstance(s, Circle))

class Stack:
    def __init__(self):
        self.items = []

    def push(self, v):
        self.items.append(v)

    def pop(self):
        return self.items.pop()

    def size(self):
        return len(self.items)

st = Stack()
for i in range(5):
    st.push(i * i)
print(st.size(), st.pop(), st.pop(), st.size())

def greet(name, greeting="hi", punct="!"):
    return greeting + " " + name + punct

print(greet("kami"))
print(greet("kami", greeting="hello", punct="?"))
print("a", "b", "c", sep="+", end=";\n")

counter = 0
def bump(by=1):
    global counter
    counter += by

bump(); bump(10)
print(counter)
