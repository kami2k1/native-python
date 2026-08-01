"""JSON encoding and decoding — pure Python, compiled to native code by kamipy.

Replaces the hand-written C++ encoder/decoder that used to live in
runtime/src/netio.cpp. A recursive-descent reader and a straightforward writer;
nothing about it needs to be C++.
"""

_ESCAPES = {
    '"': '"',
    "\\": "\\",
    "/": "/",
    "b": "\b",
    "f": "\f",
    "n": "\n",
    "r": "\r",
    "t": "\t",
}

_HEX = "0123456789abcdef"


def _err(msg, pos):
    raise ValueError("JSONDecodeError: " + msg + " at char " + str(pos))


# ---------------------------------------------------------------- decoding
class _Reader:
    def __init__(self, text):
        self.s = text
        self.i = 0
        self.n = len(text)

    def skip_ws(self):
        while self.i < self.n:
            c = self.s[self.i]
            if c == " " or c == "\t" or c == "\n" or c == "\r":
                self.i = self.i + 1
            else:
                break

    def peek(self):
        if self.i >= self.n:
            return ""
        return self.s[self.i]

    def expect(self, ch):
        if self.peek() != ch:
            _err("expected '" + ch + "'", self.i)
        self.i = self.i + 1

    def value(self):
        self.skip_ws()
        c = self.peek()
        if c == "":
            _err("unexpected end of input", self.i)
        if c == "{":
            return self.object()
        if c == "[":
            return self.array()
        if c == '"':
            return self.string()
        if c == "t":
            self.literal("true")
            return True
        if c == "f":
            self.literal("false")
            return False
        if c == "n":
            self.literal("null")
            return None
        if c == "N":  # non-standard but emitted by many producers
            self.literal("NaN")
            return float("nan")
        return self.number()

    def literal(self, word):
        if self.s[self.i:self.i + len(word)] != word:
            _err("invalid literal", self.i)
        self.i = self.i + len(word)

    def object(self):
        self.expect("{")
        out = {}
        self.skip_ws()
        if self.peek() == "}":
            self.i = self.i + 1
            return out
        while True:
            self.skip_ws()
            key = self.string()
            self.skip_ws()
            self.expect(":")
            out[key] = self.value()
            self.skip_ws()
            c = self.peek()
            if c == ",":
                self.i = self.i + 1
                continue
            if c == "}":
                self.i = self.i + 1
                return out
            _err("expected ',' or '}'", self.i)

    def array(self):
        self.expect("[")
        out = []
        self.skip_ws()
        if self.peek() == "]":
            self.i = self.i + 1
            return out
        while True:
            out.append(self.value())
            self.skip_ws()
            c = self.peek()
            if c == ",":
                self.i = self.i + 1
                continue
            if c == "]":
                self.i = self.i + 1
                return out
            _err("expected ',' or ']'", self.i)

    def string(self):
        self.expect('"')
        out = ""
        while True:
            if self.i >= self.n:
                _err("unterminated string", self.i)
            c = self.s[self.i]
            self.i = self.i + 1
            if c == '"':
                return out
            if c != "\\":
                out = out + c
                continue
            if self.i >= self.n:
                _err("unterminated escape", self.i)
            e = self.s[self.i]
            self.i = self.i + 1
            if e == "u":
                out = out + self.unicode_escape()
                continue
            simple = _ESCAPES.get(e)
            if simple is None:
                _err("invalid escape '\\" + e + "'", self.i)
            out = out + simple

    def unicode_escape(self):
        if self.i + 4 > self.n:
            _err("truncated \\u escape", self.i)
        cp = int(self.s[self.i:self.i + 4], 16)
        self.i = self.i + 4
        # surrogate pair
        if 0xD800 <= cp and cp <= 0xDBFF and self.s[self.i:self.i + 2] == "\\u":
            lo = int(self.s[self.i + 2:self.i + 6], 16)
            if 0xDC00 <= lo and lo <= 0xDFFF:
                self.i = self.i + 6
                cp = 0x10000 + ((cp - 0xD800) << 10) + (lo - 0xDC00)
        return _utf8(cp)

    def number(self):
        start = self.i
        if self.peek() == "-":
            self.i = self.i + 1
        is_float = False
        while self.i < self.n:
            c = self.s[self.i]
            if c.isdigit():
                self.i = self.i + 1
            elif c == "." or c == "e" or c == "E" or c == "+" or c == "-":
                is_float = True
                self.i = self.i + 1
            else:
                break
        raw = self.s[start:self.i]
        if raw == "" or raw == "-":
            _err("invalid number", start)
        if is_float:
            return float(raw)
        return int(raw)


# Encodes one code point as UTF-8 bytes (strings hold raw bytes here).
def _utf8(cp):
    if cp < 0x80:
        return chr(cp)
    if cp < 0x800:
        return chr(0xC0 | (cp >> 6)) + chr(0x80 | (cp & 0x3F))
    if cp < 0x10000:
        return chr(0xE0 | (cp >> 12)) + chr(0x80 | ((cp >> 6) & 0x3F)) + chr(0x80 | (cp & 0x3F))
    return (chr(0xF0 | (cp >> 18)) + chr(0x80 | ((cp >> 12) & 0x3F)) +
            chr(0x80 | ((cp >> 6) & 0x3F)) + chr(0x80 | (cp & 0x3F)))


def loads(s):
    r = _Reader(s)
    v = r.value()
    r.skip_ws()
    if r.i != r.n:
        _err("extra data", r.i)
    return v


def load(fp):
    return loads(fp.read())


# ---------------------------------------------------------------- encoding
def _quote(s, ensure_ascii):
    out = '"'
    for ch in s:
        code = ord(ch)
        if ch == '"':
            out = out + '\\"'
        elif ch == "\\":
            out = out + "\\\\"
        elif ch == "\n":
            out = out + "\\n"
        elif ch == "\r":
            out = out + "\\r"
        elif ch == "\t":
            out = out + "\\t"
        elif ch == "\b":
            out = out + "\\b"
        elif ch == "\f":
            out = out + "\\f"
        elif code < 0x20:
            out = out + "\\u00" + _HEX[(code >> 4) & 15] + _HEX[code & 15]
        elif code >= 0x80 and ensure_ascii:
            # Strings are raw bytes here; escaping individual bytes would produce
            # mojibake, so non-ASCII is passed through as valid UTF-8 instead.
            out = out + ch
        else:
            out = out + ch
    return out + '"'


def _num(v):
    if isinstance(v, bool):
        return "true" if v else "false"
    return str(v)


def _write(v, indent, level, sort_keys, ensure_ascii):
    if v is None:
        return "null"
    if isinstance(v, bool):
        return "true" if v else "false"
    if isinstance(v, str):
        return _quote(v, ensure_ascii)
    if isinstance(v, int) or isinstance(v, float):
        return _num(v)
    # CPython's defaults: ", " / ": " when compact, "," + newline when indented.
    pad = ""
    inner_pad = ""
    sep = ", "
    colon = ": "
    if indent > 0:
        pad = "\n" + " " * (indent * level)
        inner_pad = "\n" + " " * (indent * (level + 1))
        sep = "," + inner_pad
    if isinstance(v, list) or isinstance(v, tuple):
        if len(v) == 0:
            return "[]"
        parts = []
        for item in v:
            parts.append(_write(item, indent, level + 1, sort_keys, ensure_ascii))
        return "[" + inner_pad + sep.join(parts) + pad + "]"
    if isinstance(v, dict):
        keys = list(v.keys())
        if sort_keys:
            keys = sorted(keys)
        if len(keys) == 0:
            return "{}"
        parts = []
        for k in keys:
            ks = k if isinstance(k, str) else _num(k)
            parts.append(_quote(ks, ensure_ascii) + colon +
                         _write(v[k], indent, level + 1, sort_keys, ensure_ascii))
        return "{" + inner_pad + sep.join(parts) + pad + "}"
    raise ValueError("TypeError: object of type '" + str(type(v)) + "' is not JSON serializable")


def dumps(obj, indent=0, sort_keys=False, ensure_ascii=True, separators=None):
    n = 0
    if indent is not None:
        n = int(indent)
    return _write(obj, n, 0, sort_keys, ensure_ascii)


def dump(obj, fp, indent=0, sort_keys=False, ensure_ascii=True):
    fp.write(dumps(obj, indent, sort_keys, ensure_ascii))
