import json
import os

d = {"name": "kami", "nums": [1, 2, 3], "nested": {"ok": True, "x": None}, "pi": 3.5}
text = json.dumps(d)
back = json.loads(text)
print(back["name"], back["nums"][1], back["nested"]["ok"], back["pi"])
print(json.loads("[1, 2, [3, 4], {\"a\": 5}]")[2][1])
print(json.dumps([1, "two", True, None]))

print(os.path.join("a", "b", "c"))
print(os.path.basename("/x/y/z.txt"), os.path.dirname("/x/y/z.txt"))
print(os.path.exists("/tmp"), os.path.isdir("/tmp"), os.path.isfile("/tmp"))

with open("/tmp/kami_json.txt", "w") as f:
    f.write(json.dumps({"saved": 42}))
with open("/tmp/kami_json.txt") as f:
    loaded = json.loads(f.read())
print(loaded["saved"])
os.remove("/tmp/kami_json.txt")
print(os.path.exists("/tmp/kami_json.txt"))
