# Imported modules get a real namespace: a module-level name in re.py / json.py
# cannot collide with a program that happens to use the same names. Every
# function and class below shadows something the stdlib defines internally.
import json
import re
from json import dumps
from re import findall as grep


def match(x):
    return "user match: " + x


def search(x):
    return "user search: " + x


def compile(x):
    return "user compile: " + x


def loads(x):
    return "user loads: " + x


class Match:
    def __init__(self, v):
        self.v = v

    def group(self, i=0):
        return "user group " + str(i) + ": " + self.v


class Pattern:
    def __init__(self):
        self.kind = "user pattern"


# the user's own names win everywhere
print(match("a"), search("b"))
print(compile("c"), loads("d"))
print(Match("m").group(1), Pattern().kind)

# ...and the module's names still reach the module's own code
print(re.match(r"\d+", "42").group())
print(re.search(r"[a-z]+", "12abc").group())
print(re.compile(r"a+").findall("aa b aaa"))
print(json.loads('{"k": [1, 2]}')["k"][1])
print(json.dumps({"k": 1}))

# from-imports are namespaced too, including "as" renames
print(dumps([1, 2]))
print(grep(r"\d", "a1b2"))

# module-level state of a translated module is private to it
_cache = "user cache"
print(_cache, len(re.findall(r"x", "xxx")))
