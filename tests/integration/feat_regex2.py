# The `re` module is now pure Python (runtime/pylib/re.py) compiled by kamipy.
# Every line below is checked against CPython's own output.
import re

# --- quantifiers, greedy vs lazy ------------------------------------------
print(re.search(r"<(.+)>", "<a><b>").group(1))
print(re.search(r"<(.+?)>", "<a><b>").group(1))
print(re.findall(r"a{2,3}", "a aa aaa aaaa"))
print(re.findall(r"ab{0,1}c", "ac abc abbc"))
print(re.fullmatch(r"\d{3}", "123") is not None)
print(re.fullmatch(r"\d{3}", "1234") is None)
print(re.match(r"a*", "").group(0) == "")

# --- alternation and grouping ---------------------------------------------
print(re.findall(r"(?:foo|bar)baz", "foobaz barbaz quxbaz"))
print(re.search(r"(a|ab)(c|bc)", "abc").group(0))
print(re.match(r"(\d+)-(\d+)", "12-34").groups())

# --- character classes ----------------------------------------------------
print(re.findall(r"[^aeiou\s]+", "the quick brown fox"))
print(re.findall(r"[a-c-]", "a-b-c-d"))
print(re.findall(r"[\d.]+", "v1.2.3 and 45"))
print(re.sub(r"[][]", "", "[hi]"))

# --- anchors and boundaries ----------------------------------------------
print(re.findall(r"^\w+", "one two\nthree four", re.M))
print(re.findall(r"\w+$", "one two\nthree four", re.M))
print(re.sub(r"\bcat\b", "dog", "cat concat cat."))
print(re.findall(r"\Bab", "ab cab"))

# --- flags ---------------------------------------------------------------
print(re.findall(r"[a-z]+", "Hello World", re.I))
print(re.search(r"a.b", "a\nb") is None)
print(re.search(r"a.b", "a\nb", re.S).group(0) == "a\nb")
print(re.match(r"(?i)hello", "HELLO").group(0))

# --- named groups, backreferences, groupdict -----------------------------
m = re.match(r"(?P<key>\w+)=(?P<val>\d+)", "size=42")
print(m.group("key"), m.group("val"), m.groupdict())
print(m.span(), m.span(1), m.span(2), m.lastindex)
print(re.search(r"(\w)\1", "aabb").group(0))
print(re.search(r"(\w)\1", "abab") is None)

# --- optional groups that did not participate ---------------------------
m2 = re.match(r"(a)(b)?(c)", "ac")
print(m2.groups(), m2.group(2) is None, m2.groups("!"))

# --- sub / subn / split --------------------------------------------------
print(re.sub(r"(\w+)@(\w+)", r"\2 at \1", "me@host"))
print(re.sub(r"\d", "#", "a1b2c3", 2))
print(re.subn(r"o", "0", "foo boo"))
print(re.split(r"\s*,\s*", "a , b,c"))
print(re.split(r"(\d)", "a1b2c"))
print(re.split(r",", "a,b,c", 1))
print(re.sub(r"x", lambda mm: "[" + mm.group(0).upper() + "]", "axbx"))

# --- compile / finditer / escape ----------------------------------------
rx = re.compile(r"(\w+):(\d+)")
print([[m3.group(1), int(m3.group(2))] for m3 in rx.finditer("a:1 bb:22")])
print(rx.pattern, rx.groups)
print(re.escape("a.b*c"))
print(re.findall(re.escape("1+1"), "1+1=2"))

# --- pathological patterns must terminate -------------------------------
print(re.match(r"(a*)*b", "aaab").group(0))
print(re.match(r"(|a)*b", "aab").group(0))
print(re.search(r"(\s*)$", "trail   ").group(1) == "   ")

# --- long input: the VM must not recurse --------------------------------
big = "x" * 20000 + "needle"
print(re.search(r"needle", big).start())
print(len(re.findall(r"x+", big)[0]))
