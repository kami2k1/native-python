# `json` is now a pure-Python module (runtime/pylib/json.py) compiled by kamipy.
import json

print(json.dumps({"a": 1, "b": [1, 2.5, True, None], "c": "x\"y\n"}))
print(json.dumps([1, [2, [3]]]))
print(json.dumps({"z": 1, "a": 2}, sort_keys=True))
print(json.dumps({"a": [1, {"b": 2}]}, indent=2))
print(json.dumps("plain"), json.dumps(7), json.dumps(True), json.dumps(None))

d = json.loads('{"n": 42, "f": 1.5, "s": "hi\\n", "t": true, "z": null, "l": [1,2]}')
print(d["n"], d["f"], d["s"] == "hi\n", d["t"], d["z"] is None, d["l"])
print(json.loads('"\\u0041\\u00e9"'))
print(json.loads("  [1, [2, [3]]]  "))
print(json.loads("-12"), json.loads("1e3"), json.loads("[]"), json.loads("{}"))

# round trip
src = {"users": [{"id": 1, "tags": ["a", "b"]}, {"id": 2, "tags": []}], "ok": True}
print(json.loads(json.dumps(src)) == src)

for bad in ["{bad}", "[1,", '{"a" 1}', "tru"]:
    try:
        json.loads(bad)
        print("NO ERROR for", bad)
    except Exception:
        print("rejected", bad)
