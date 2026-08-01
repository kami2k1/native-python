# The regex engine is stdlib/re.py — Python source compiled to native code.
# Everything here is also valid CPython and produces the same output.
import re

# lazy vs greedy
print(re.findall(r"<(.+?)>", "<a><bb>"))
print(re.search(r"<(.+)>", "<a><bb>").group(1))

# flags
print(re.search(r"hello", "say HELLO", re.I).group())
print(re.findall(r"^\w+", "one\ntwo", re.M))
print(re.search(r"a.b", "a\nb", re.S).group() == "a\nb")

# backreferences, groups and alternation
print(re.sub(r"(a)(b)", r"\2\1", "abab"))
print(re.search(r"(\w)\1", "hello").group())
print(re.match(r"(?:ab)+", "ababab").group())
pairs = re.findall(r"(a)(b)?", "ab a")
print(len(pairs), pairs[0][0], pairs[0][1], pairs[1][0], pairs[1][1] == "")

# quantifiers
print(re.findall(r"a{2,3}", "a aa aaa aaaa"))
print(re.sub(r"x*", "-", "abc"))
print(re.match(r"a.*z", "abcz").group())

# classes and anchors
print(re.findall(r"[^a-z ]+", "ab1 cd23"))
print(re.search(r"\d+$", "x 42").group())
print(re.escape("a.b*c"))

# compiled patterns are cached and reusable
p = re.compile(r"(\d+)-(\d+)")
m = p.search("range 10-20 ok")
g = m.groups()
print(g[0], g[1], m.start(), m.end(), m.start(2))
print([x.group(0) for x in p.finditer("1-2 and 30-40")])
print(p.sub(r"\2-\1", "1-2 and 30-40"))
print(re.split(r"\s*,\s*", "a , b,c"))
print(re.split(r",", "a,b,c", 1))
print(re.fullmatch(r"\d+", "123") is not None, re.fullmatch(r"\d+", "12a") is None)

# a tiny tokenizer, the way the module is actually used
TOKEN = re.compile(r"(\d+|[a-z]+|[-+*/()])")
print(TOKEN.findall("12+ab*(3-4)"))
