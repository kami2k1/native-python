# json / os / os.path / string are Python modules under stdlib/, translated into
# this program at compile time. Valid CPython too — same output.
import json
import os
import os.path
import string

# ---- json round trip
doc = {"name": "kami", "nums": [1, 2, 3], "nested": {"ok": True, "x": None}, "pi": 3.5}
text = json.dumps(doc)
print(text)
back = json.loads(text)
print(back["name"], back["nums"][2], back["nested"]["ok"], back["pi"])
print(json.dumps([1, "two", True, None, 2.5]))
print(json.dumps({}), json.dumps([]))
print(json.dumps({"b": 1, "a": 2}, sort_keys=True))
print(json.dumps({"a": [1, {"b": 2}]}, indent=2))

# escapes and unicode
print(json.dumps({"q": "he said \"hi\"\n\ttab"}))
print(json.loads('"\\u0041\\u00e9"'))
print(json.loads('{"a": [1, 2, [3, {"b": -4.5e2}]]}')["a"][2][1]["b"])
print(json.loads("  [1,2]  "))
try:
    json.loads("[1, 2")
except ValueError as e:
    print("decode error caught")

# ---- os.path is pure string handling
print(os.path.join("a", "b", "c"))
print(os.path.join("/tmp", "x.txt"))
print(os.path.basename("/x/y/z.txt"), os.path.dirname("/x/y/z.txt"))
print(os.path.splitext("/x/y/z.tar.gz")[1], os.path.splitext("noext")[1] == "")
print(os.path.normpath("a/./b/../c//d"))
print(os.path.isabs("/x"), os.path.isabs("x"))
print(os.path.exists("/tmp"), os.path.isdir("/tmp"), os.path.isfile("/tmp"))

# ---- os touches the file system through the syscall bindings
work = "/tmp/kami_stdlib_test/inner"
os.makedirs(work, True)
print(os.path.isdir(work))
target = os.path.join(work, "note.txt")
with open(target, "w") as f:
    f.write(json.dumps({"saved": 42}))
print(os.path.getsize(target) > 0)
with open(target) as f:
    print(json.loads(f.read())["saved"])
print(os.listdir(work))
os.rename(target, os.path.join(work, "renamed.txt"))
print(os.listdir(work))
os.remove(os.path.join(work, "renamed.txt"))
os.rmdir(work)
os.rmdir("/tmp/kami_stdlib_test")
print(os.path.exists("/tmp/kami_stdlib_test"))
print(len(os.getcwd()) > 0, os.getenv("KAMI_NOT_SET_HOPEFULLY", "fallback"))
try:
    os.remove("/tmp/kami_definitely_missing_file")
except OSError as e:
    print("oserror caught")

# ---- string constants
print(string.digits, len(string.ascii_letters))
print(string.capwords("hello wide world"))
