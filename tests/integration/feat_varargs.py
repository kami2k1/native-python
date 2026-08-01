# *args / **kwargs / keyword-only parameters, compiled AOT. The keyword dict
# travels in its own channel (not as a trailing argument), so it can never be
# confused with one more positional argument.
#
# Valid CPython — except that this language has no distinct tuple type, so *args
# prints as a list. The test therefore indexes instead of printing tuples.


def total(*nums):
    s = 0
    for n in nums:
        s += n
    return s


def tagged(tag, *rest):
    return tag + ":" + str(len(rest))


def opts(**kw):
    keys = list(kw.keys())
    keys.sort()
    out = []
    for k in keys:
        out.append(k + "=" + str(kw[k]))
    return ",".join(out)


def both(first, *rest, **kw):
    return str(first) + "|" + str(len(rest)) + "|" + opts(**kw)


def joined(*parts, sep="-", end=""):
    return sep.join([str(p) for p in parts]) + end


def add(a, b):
    return a + b


print(total(), total(1), total(1, 2, 3))
print(tagged("a"), tagged("a", 1, 2))
print(opts(), opts(b=2, a=1))
print(both(1), both(1, 2, 3, x=9, y=8))

# keyword-only parameters
print(joined(1, 2, 3), joined("a", "b", sep="+"), joined("x", sep=".", end="!"))

# call-site spreading
args = [3, 4]
print(add(*args))
kw = {"b": 10}
print(add(1, **kw))
print(total(*[1, 2], *[3, 4]))


# the decorator idiom: forwarding every argument shape
def doubler(fn):
    def inner(*a, **k):
        return fn(*a, **k) * 2
    return inner


@doubler
def scaled(a, b=3):
    return a * b


print(scaled(5), scaled(5, 4), scaled(5, b=10))

double_add = doubler(add)
print(double_add(2, 3))


# methods take them too
class Router:
    def __init__(self, name, *middleware, **settings):
        self.name = name
        self.mw = middleware
        self.settings = settings

    def describe(self, *extra, prefix=""):
        return prefix + self.name + "/" + str(len(self.mw)) + "/" + \
            str(len(self.settings)) + "/" + str(len(extra))

    def forward(self, *a, **k):
        return self.describe(*a, **k)


r = Router("api", "auth", "log", debug=True, port=80)
print(r.describe(), r.describe(1, 2))
print(r.mw[0], r.settings["port"])
print(r.describe(prefix=">"), r.forward(1, prefix="<"))
print(Router("bare").describe())


# error paths stay catchable
def strict(a, b):
    return a + b


try:
    strict(*[1, 2, 3])
except Exception:
    print("too many positional args caught")


def kwonly(*, req):
    return req


try:
    empty = {}
    kwonly(**empty)
except Exception:
    print("missing keyword-only arg caught")


# starred assignment targets
a, *rest = [1, 2, 3, 4]
print(a, rest)
first, *mid, last = [1, 2, 3, 4, 5]
print(first, mid, last)
solo, *none = [9]
print(solo, none)
head, *tail = "abcd"
print(head, tail)


# a subclass without __init__ inherits the one it needs
class Animal:
    def __init__(self, name):
        self.name = name

    def speak(self):
        return self.name + " makes a sound"


class Dog(Animal):
    def speak(self):
        return self.name + " barks"


class Puppy(Dog):
    pass


print(Dog("rex").speak())
print(Puppy("bit").speak(), isinstance(Puppy("b"), Animal))
