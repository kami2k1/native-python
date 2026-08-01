"""Path manipulation — pure string handling plus stat() from the C-ABI layer."""
import _kami

if _kami.platform() == "win32":
    sep = "\\"
    altsep = "/"
    extsep = "."
    pathsep = ";"
else:
    sep = "/"
    altsep = ""
    extsep = "."
    pathsep = ":"

curdir = "."
pardir = ".."


def _is_sep(c):
    if c == sep:
        return True
    return altsep != "" and c == altsep


# Index of the last separator in p, or -1. (str.rfind is not available yet.)
def _last_sep(p):
    i = len(p) - 1
    while i >= 0:
        if _is_sep(p[i]):
            return i
        i -= 1
    return -1


def isabs(p):
    if p == "":
        return False
    if _is_sep(p[0]):
        return True
    # Windows drive letter: "C:\..."
    return len(p) >= 3 and p[1] == ":" and _is_sep(p[2])


def join(a, b="", c="", d="", e="", f="", g=""):
    out = a
    for part in [b, c, d, e, f, g]:
        if part == "":
            continue
        if isabs(part):
            out = part
        elif out == "" or _is_sep(out[len(out) - 1]):
            out = out + part
        else:
            out = out + sep + part
    return out


def split(p):
    i = _last_sep(p)
    if i < 0:
        return ("", p)
    head = p[0:i]
    if head == "":
        head = p[0:1]  # keep the root separator
    return (head, p[i + 1:])


def basename(p):
    return split(p)[1]


def dirname(p):
    return split(p)[0]


def splitext(p):
    base = basename(p)
    i = len(base) - 1
    while i > 0:
        if base[i] == extsep:
            cut = len(p) - (len(base) - i)
            return (p[0:cut], p[cut:])
        i -= 1
    return (p, "")


def normpath(p):
    if p == "":
        return curdir
    rooted = _is_sep(p[0])
    parts = []
    current = ""
    for ch in p:
        if _is_sep(ch):
            parts.append(current)
            current = ""
        else:
            current = current + ch
    parts.append(current)
    out = []
    for part in parts:
        if part == "" or part == curdir:
            continue
        if part == pardir:
            if len(out) > 0 and out[len(out) - 1] != pardir:
                out.pop()
                continue
            if rooted:
                continue
        out.append(part)
    joined = sep.join(out)
    if rooted:
        joined = sep + joined
    if joined == "":
        return curdir
    return joined


def abspath(p):
    if isabs(p):
        return normpath(p)
    return normpath(_kami.getcwd() + sep + p)


def exists(p):
    return _kami.stat(p) >= 0


def isfile(p):
    return _kami.stat(p) == 0


def isdir(p):
    return _kami.stat(p) == 1


def getsize(p):
    size = _kami.filesize(p)
    if size < 0:
        raise OSError("cannot stat '" + p + "': " + _kami.errmsg())
    return size


def expanduser(p):
    if p == "" or p[0] != "~":
        return p
    home = _kami.getenv("HOME")
    if home is None:
        home = _kami.getenv("USERPROFILE")
    if home is None:
        return p
    return home + p[1:]
