"""JSON encoder/decoder, written in Python and compiled to native code."""

_ESCAPES = {"n": "\n", "t": "\t", "r": "\r", "b": "\b", "f": "\f", "/": "/",
            "\\": "\\", "\"": "\""}
_HEX = "0123456789abcdef"


def _hexval(c):
    i = 0
    low = c.lower()
    while i < 16:
        if _HEX[i] == low:
            return i
        i += 1
    return -1


def _utf8(cp):
    if cp < 128:
        return chr(cp)
    if cp < 2048:
        return chr(192 + cp // 64) + chr(128 + cp % 64)
    if cp < 65536:
        return chr(224 + cp // 4096) + chr(128 + (cp // 64) % 64) + chr(128 + cp % 64)
    return (chr(240 + cp // 262144) + chr(128 + (cp // 4096) % 64) +
            chr(128 + (cp // 64) % 64) + chr(128 + cp % 64))


class _Decoder:
    def __init__(self, text):
        self.s = text
        self.i = 0
        self.n = len(text)

    def fail(self, msg):
        raise ValueError("json: " + msg + " at position " + str(self.i))

    def ws(self):
        while self.i < self.n and (self.s[self.i] == " " or self.s[self.i] == "\t" or
                                   self.s[self.i] == "\n" or self.s[self.i] == "\r"):
            self.i += 1

    def peek(self):
        if self.i < self.n:
            return self.s[self.i]
        return ""

    def literal(self, word):
        end = self.i + len(word)
        if end <= self.n and self.s[self.i:end] == word:
            self.i = end
            return True
        return False

    def value(self):
        self.ws()
        c = self.peek()
        if c == "":
            self.fail("unexpected end of input")
        if c == "{":
            return self.object()
        if c == "[":
            return self.array()
        if c == "\"":
            return self.string()
        if self.literal("true"):
            return True
        if self.literal("false"):
            return False
        if self.literal("null"):
            return None
        return self.number()

    def object(self):
        self.i += 1
        out = {}
        self.ws()
        if self.peek() == "}":
            self.i += 1
            return out
        while True:
            self.ws()
            if self.peek() != "\"":
                self.fail("expected a string key")
            key = self.string()
            self.ws()
            if self.peek() != ":":
                self.fail("expected ':'")
            self.i += 1
            out[key] = self.value()
            self.ws()
            c = self.peek()
            if c == ",":
                self.i += 1
                continue
            if c == "}":
                self.i += 1
                return out
            self.fail("expected ',' or '}'")

    def array(self):
        self.i += 1
        out = []
        self.ws()
        if self.peek() == "]":
            self.i += 1
            return out
        while True:
            out.append(self.value())
            self.ws()
            c = self.peek()
            if c == ",":
                self.i += 1
                continue
            if c == "]":
                self.i += 1
                return out
            self.fail("expected ',' or ']'")

    def string(self):
        self.i += 1
        out = ""
        while True:
            if self.i >= self.n:
                self.fail("unterminated string")
            c = self.s[self.i]
            self.i += 1
            if c == "\"":
                return out
            if c != "\\":
                out = out + c
                continue
            if self.i >= self.n:
                self.fail("truncated escape")
            e = self.s[self.i]
            self.i += 1
            if e == "u":
                out = out + self.unicode_escape()
                continue
            plain = _ESCAPES.get(e)
            if plain is None:
                self.fail("invalid escape '\\" + e + "'")
            out = out + plain

    def unicode_escape(self):
        cp = self.hex4()
        # a surrogate pair encodes one astral code point
        if cp >= 55296 and cp <= 56319 and self.i + 6 <= self.n and \
                self.s[self.i:self.i + 2] == "\\u":
            self.i += 2
            low = self.hex4()
            if low >= 56320 and low <= 57343:
                cp = 65536 + (cp - 55296) * 1024 + (low - 56320)
            else:
                return _utf8(cp) + _utf8(low)
        return _utf8(cp)

    def hex4(self):
        if self.i + 4 > self.n:
            self.fail("truncated \\u escape")
        value = 0
        k = 0
        while k < 4:
            digit = _hexval(self.s[self.i + k])
            if digit < 0:
                self.fail("bad \\u escape")
            value = value * 16 + digit
            k += 1
        self.i += 4
        return value

    def number(self):
        start = self.i
        if self.peek() == "-":
            self.i += 1
        isfloat = False
        while self.i < self.n:
            c = self.s[self.i]
            if c >= "0" and c <= "9":
                self.i += 1
            elif c == "." or c == "e" or c == "E" or c == "+" or c == "-":
                isfloat = True
                self.i += 1
            else:
                break
        text = self.s[start:self.i]
        if text == "" or text == "-":
            self.fail("invalid number")
        if isfloat:
            return float(text)
        return int(text)


def loads(text):
    dec = _Decoder(text)
    value = dec.value()
    dec.ws()
    if dec.i != dec.n:
        dec.fail("trailing data")
    return value


def _escape(s):
    out = "\""
    for c in s:
        o = ord(c)
        if c == "\"":
            out = out + "\\\""
        elif c == "\\":
            out = out + "\\\\"
        elif c == "\n":
            out = out + "\\n"
        elif c == "\t":
            out = out + "\\t"
        elif c == "\r":
            out = out + "\\r"
        elif o < 32:
            out = out + "\\u00" + _HEX[o // 16] + _HEX[o % 16]
        else:
            out = out + c
    return out + "\""


def _dump(value, indent, level, sort_keys):
    kind = type(value)
    if kind == "NoneType":
        return "null"
    if kind == "bool":
        return "true" if value else "false"
    if kind == "int" or kind == "float":
        return str(value)
    if kind == "str":
        return _escape(value)
    nl = ""
    pad = ""
    inner = ""
    if indent > 0:
        nl = "\n"
        pad = " " * (indent * level)
        inner = " " * (indent * (level + 1))
    if kind == "list":
        if len(value) == 0:
            return "[]"
        parts = []
        for item in value:
            parts.append(inner + _dump(item, indent, level + 1, sort_keys))
        sep = "," + nl if indent > 0 else ", "
        return "[" + nl + sep.join(parts) + nl + pad + "]"
    if kind == "dict":
        if len(value) == 0:
            return "{}"
        keys = list(value.keys())
        if sort_keys:
            keys.sort()
        parts = []
        for key in keys:
            if type(key) != "str":
                raise TypeError("json: dict keys must be strings")
            parts.append(inner + _escape(key) + ": " +
                         _dump(value[key], indent, level + 1, sort_keys))
        sep = "," + nl if indent > 0 else ", "
        return "{" + nl + sep.join(parts) + nl + pad + "}"
    raise TypeError("json: cannot serialize '" + kind + "'")


def dumps(obj, indent=0, sort_keys=False):
    if indent is None:
        indent = 0
    return _dump(obj, indent, 0, sort_keys)


def dump(obj, fp, indent=0, sort_keys=False):
    fp.write(dumps(obj, indent, sort_keys))


def load(fp):
    return loads(fp.read())
