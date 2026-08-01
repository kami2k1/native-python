# Syntax and semantics added in v0.6.0.
# 1. implicit string concatenation, including across f-strings
# 2. *args catch-all parameters
# 3. the % string-formatting operator
# 4. repr() / callable() / __str__ / __repr__
# 5. dict and set equality by value

# --- implicit concatenation ------------------------------------------------
h = "HDR"
n = 7
plain = "a" "b" "c"
mixed = (
    f"{h}\n"
    f"count: {n}\n"
    "tail line."
)
lead = "x" f"{n}" "y"
comment_split = ("one "  # a comment between the pieces
                 "two")
print(plain)
print(mixed)
print(lead, comment_split)
print("a" "b" == "ab", len("q" f"{n}"))

# --- *args ----------------------------------------------------------------
def only_rest(*rest):
    return [len(rest), list(rest)]


def head_rest(first, *rest):
    return [first, list(rest)]


print(only_rest(), only_rest(1), only_rest(1, "b", None))
print(head_rest(1), head_rest(1, 2, 3))


class Joiner:
    def __init__(self, sep):
        self.sep = sep

    def join(self, *parts):
        out = []
        for p in parts:
            out.append(str(p))
        return self.sep.join(out)


j = Joiner("-")
print(j.join(), j.join("a"), j.join("a", 1, True))


def fmt(template, *args):
    if len(args) == 0:
        return template
    return template % args


print(fmt("literal"), fmt("%s=%d", "x", 5))

# --- % operator -----------------------------------------------------------
print("%s/%s" % ("a", "b"))
print("%d %05.2f %x %X %o" % (42, 3.14159, 255, 255, 64))
print("%-6s|%6s|" % ("ab", "cd"))
print("%(k)s -> %(v)d" % {"k": "key", "v": 9})
print("literal %% sign" % [])
print("single %s" % "arg")

# --- repr / callable / dunders -------------------------------------------
print(repr("a\nb\t'q'"), repr(12), repr([1, "x", None]))
print(callable(fmt), callable(Joiner), callable(3))


class Point:
    def __init__(self, x, y):
        self.x = x
        self.y = y

    def __repr__(self):
        return "Point(" + str(self.x) + ", " + str(self.y) + ")"


class Named:
    def __init__(self, n):
        self.n = n

    def __str__(self):
        return "<" + self.n + ">"

    def __repr__(self):
        return "Named(" + repr(self.n) + ")"


p = Point(1, 2)
print(p)
print(str(p), repr(p))
nm = Named("kami")
print(nm, str(nm), repr(nm))
print("in fstring: " + f"{p}")

# --- container equality ---------------------------------------------------
print({"a": 1} == {"a": 1}, {"a": 1} == {"a": 2}, {"a": 1} == {"b": 1})
print({"d": {"k": [1, 2]}} == {"d": {"k": [1, 2]}})
print([{"a": 1}, {"b": 2}] == [{"a": 1}, {"b": 2}])
print({"a": 1, "b": 2} == {"b": 2, "a": 1})
print(set([1, 2]) == set([2, 1]), set([1]) == set([1, 2]))

# --- str.find/index windows, strip(chars), splitlines, partition ---------
s = "abcabc"
print(s.find("b"), s.find("b", 2), s.find("z"), s.rfind("b"))
print(s.index("c"), s.rindex("c"))
print("xxhixx".strip("x"), "xxhi".lstrip("x"), "hixx".rstrip("x"))
print("l1\nl2\r\nl3".splitlines())
print("k=v=w".partition("="), "k=v=w".rpartition("="))
print("a,b".partition(";"))
d = {}
print(d.setdefault("k", 1), d.setdefault("k", 2), d)
print(sorted(list({"b": 1, "a": 2})))
