"""Regular expressions: a backtracking engine written in Python.

The pattern is parsed into a small tree of lists and matched with an explicit
continuation chain (also lists), so no C++ regex engine is needed — this file is
compiled to native code together with the program that imports it.

Supported: literals, ".", character classes with ranges and negation, the
predefined classes \d \D \w \W \s \S, anchors ^ $ \A \Z \b \B, groups
"(...)" and "(?:...)", named groups (matched, not addressable by name),
alternation, the quantifiers * + ? {m} {m,} {m,n} with lazy variants,
backreferences \1..\9, and the flags IGNORECASE / MULTILINE / DOTALL.
"""

I = 2
IGNORECASE = 2
M = 8
MULTILINE = 8
S = 16
DOTALL = 16

_BIG = 1000000000

# ---------------------------------------------------------------- char helpers

def _is_word(c):
    o = ord(c)
    if o >= 48 and o <= 57:
        return True
    if o >= 65 and o <= 90:
        return True
    if o >= 97 and o <= 122:
        return True
    return c == "_"


def _is_space(c):
    return c == " " or c == "\t" or c == "\n" or c == "\r" or c == "\f" or c == "\v"


def _is_digit(c):
    o = ord(c)
    return o >= 48 and o <= 57


def _lower(c):
    o = ord(c)
    if o >= 65 and o <= 90:
        return chr(o + 32)
    return c


# ---------------------------------------------------------------- parser
# A parsed pattern is a tree of plain lists (no *args in the language subset):
#   alt   := [seq, seq, ...]
#   seq   := [term, term, ...]
#   term  := [atom, min, max, greedy]        max < 0 means "unbounded"
#   atom  := ["char", ch]  | ["any"] | ["set", negated, ranges, classes]
#          | ["group", alt, gidx] | ["bol"] | ["eol"] | ["wordb", negated]
#          | ["backref", n]

class _Parser:
    def __init__(self, pattern):
        self.p = pattern
        self.i = 0
        self.n = len(pattern)
        self.ngroups = 0

    def error(self, msg):
        raise ValueError("re.error: " + msg + " at position " + str(self.i))

    def eof(self):
        return self.i >= self.n

    def peek(self):
        if self.i < self.n:
            return self.p[self.i]
        return ""

    def next(self):
        c = self.p[self.i]
        self.i += 1
        return c

    def parse_alt(self):
        branches = [self.parse_seq()]
        while not self.eof() and self.peek() == "|":
            self.i += 1
            branches.append(self.parse_seq())
        return branches

    def parse_seq(self):
        seq = []
        while not self.eof():
            c = self.peek()
            if c == "|" or c == ")":
                break
            atom = self.parse_atom()
            seq.append(self.parse_quant(atom))
        return seq

    def parse_quant(self, atom):
        mn = 1
        mx = 1
        if not self.eof():
            c = self.peek()
            if c == "*":
                self.i += 1
                mn = 0
                mx = -1
            elif c == "+":
                self.i += 1
                mn = 1
                mx = -1
            elif c == "?":
                self.i += 1
                mn = 0
                mx = 1
            elif c == "{":
                save = self.i
                self.i += 1
                lo = ""
                while not self.eof() and _is_digit(self.peek()):
                    lo = lo + self.next()
                hi = lo
                if not self.eof() and self.peek() == ",":
                    self.i += 1
                    hi = ""
                    while not self.eof() and _is_digit(self.peek()):
                        hi = hi + self.next()
                if self.eof() or self.peek() != "}" or lo == "":
                    self.i = save  # not a quantifier: literal "{"
                    return [atom, 1, 1, True]
                self.i += 1
                mn = int(lo)
                if hi == "":
                    mx = -1
                else:
                    mx = int(hi)
        greedy = True
        if not self.eof() and self.peek() == "?" and (mn != mx):
            self.i += 1
            greedy = False
        return [atom, mn, mx, greedy]

    def parse_atom(self):
        c = self.next()
        if c == "(":
            gidx = -1
            if self.peek() == "?":
                self.i += 1
                k = self.peek()
                if k == ":":
                    self.i += 1
                elif k == "P":
                    self.i += 1
                    if self.peek() == "<":
                        while not self.eof() and self.peek() != ">":
                            self.i += 1
                        self.i += 1
                    self.ngroups += 1
                    gidx = self.ngroups
                else:
                    self.error("unsupported group syntax '(?" + k + "'")
            else:
                self.ngroups += 1
                gidx = self.ngroups
            alt = self.parse_alt()
            if self.eof() or self.next() != ")":
                self.error("missing ), unterminated subpattern")
            return ["group", alt, gidx]
        if c == "[":
            return self.parse_set()
        if c == ".":
            return ["any"]
        if c == "^":
            return ["bol"]
        if c == "$":
            return ["eol"]
        if c == "\\":
            return self.parse_escape()
        if c == ")":
            self.error("unbalanced parenthesis")
        return ["char", c]

    # \d \w \s and friends become one-element set atoms; \b/\B are assertions.
    def parse_escape(self):
        if self.eof():
            self.error("bad escape (end of pattern)")
        c = self.next()
        if c == "d":
            return ["set", False, [], ["d"]]
        if c == "D":
            return ["set", True, [], ["d"]]
        if c == "w":
            return ["set", False, [], ["w"]]
        if c == "W":
            return ["set", True, [], ["w"]]
        if c == "s":
            return ["set", False, [], ["s"]]
        if c == "S":
            return ["set", True, [], ["s"]]
        if c == "b":
            return ["wordb", False]
        if c == "B":
            return ["wordb", True]
        if c == "A":
            return ["bos"]
        if c == "Z":
            return ["eos"]
        if _is_digit(c) and c != "0":
            return ["backref", int(c)]
        return ["char", _unescape(c)]

    def parse_set(self):
        negated = False
        if self.peek() == "^":
            self.i += 1
            negated = True
        ranges = []
        classes = []
        first = True
        while True:
            if self.eof():
                self.error("unterminated character set")
            c = self.next()
            if c == "]" and not first:
                break
            first = False
            if c == "\\":
                e = self.next()
                if e == "d" or e == "w" or e == "s":
                    classes.append(e)
                    continue
                if e == "D" or e == "W" or e == "S":
                    classes.append(e)
                    continue
                c = _unescape(e)
            lo = ord(c)
            hi = lo
            if self.peek() == "-" and self.i + 1 < self.n and self.p[self.i + 1] != "]":
                self.i += 1
                d = self.next()
                if d == "\\":
                    d = _unescape(self.next())
                hi = ord(d)
            ranges.append([lo, hi])
        return ["set", negated, ranges, classes]


def _unescape(c):
    if c == "n":
        return "\n"
    if c == "t":
        return "\t"
    if c == "r":
        return "\r"
    if c == "f":
        return "\f"
    if c == "v":
        return "\v"
    if c == "0":
        return "\x00"
    return c


# ---------------------------------------------------------------- matcher
# Continuations are lists so the matcher can be written without closures:
#   ["seq", seq, i, parent]      → keep matching seq from index i
#   ["rep", seq, i, count, start, parent] → one more round of a repetition
#   ["grp", gidx, start, parent] → close capture group gidx
#   None                         → success

class _M:
    def __init__(self, text, ngroups, flags):
        self.t = text
        self.n = len(text)
        self.ng = ngroups
        self.gs = []
        self.ge = []
        i = 0
        while i <= ngroups:
            self.gs.append(-1)
            self.ge.append(-1)
            i += 1
        self.icase = (flags % 4) >= 2
        self.dotall = (flags // 16) % 2 == 1
        self.multi = (flags // 8) % 2 == 1

    # One matcher is reused for every start position of a scan: allocating a
    # fresh state per position dominated the run time of long searches.
    def reset(self):
        i = 0
        while i <= self.ng:
            self.gs[i] = -1
            self.ge[i] = -1
            i += 1

    # ---- single-character tests ----
    def one(self, atom, pos):
        if pos >= self.n:
            return False
        c = self.t[pos]
        k = atom[0]
        if k == "char":
            if self.icase:
                return _lower(c) == _lower(atom[1])
            return c == atom[1]
        if k == "any":
            if self.dotall:
                return True
            return c != "\n"
        # set
        if self.icase:
            if self.in_set(atom, c):
                return True
            lo = _lower(c)
            up = chr(ord(lo) - 32) if (ord(lo) >= 97 and ord(lo) <= 122) else lo
            other = up if c == lo else lo
            if atom[1]:
                return not self.in_set_raw(atom, other)
            return self.in_set_raw(atom, other)
        return self.in_set(atom, c)

    def in_set(self, atom, c):
        hit = self.in_set_raw(atom, c)
        if atom[1]:
            return not hit
        return hit

    def in_set_raw(self, atom, c):
        o = ord(c)
        for r in atom[2]:
            if o >= r[0] and o <= r[1]:
                return True
        for cl in atom[3]:
            if cl == "d":
                if _is_digit(c):
                    return True
            elif cl == "w":
                if _is_word(c):
                    return True
            elif cl == "s":
                if _is_space(c):
                    return True
            elif cl == "D":
                if not _is_digit(c):
                    return True
            elif cl == "W":
                if not _is_word(c):
                    return True
            elif cl == "S":
                if not _is_space(c):
                    return True
        return False

    def simple(self, atom):
        k = atom[0]
        return k == "char" or k == "any" or k == "set"

    # ---- continuation plumbing ----
    def apply(self, k, pos):
        if k is None:
            return pos
        kind = k[0]
        if kind == "seq":
            return self.run(k[1], k[2], pos, k[3])
        if kind == "grp":
            gidx = k[1]
            olds = self.gs[gidx]
            olde = self.ge[gidx]
            self.gs[gidx] = k[2]
            self.ge[gidx] = pos
            r = self.apply(k[3], pos)
            if r < 0:
                self.gs[gidx] = olds
                self.ge[gidx] = olde
            return r
        # rep: finished one iteration that started at k[4]
        seq = k[1]
        i = k[2]
        count = k[3] + 1
        if pos == k[4]:
            # the iteration consumed nothing: repeating again cannot progress
            return self.apply(["seq", seq, i + 1, k[5]], pos)
        return self.term(seq, i, pos, k[5], count)

    def run(self, seq, i, pos, cont):
        if i >= len(seq):
            return self.apply(cont, pos)
        return self.term(seq, i, pos, cont, 0)

    def term(self, seq, i, pos, cont, count):
        term = seq[i]
        atom = term[0]
        mn = term[1]
        mx = term[2]
        greedy = term[3]
        rest = ["seq", seq, i + 1, cont]
        # fast path: a single-character atom repeated — loop instead of recurse
        if count == 0 and mx != 1 and self.simple(atom):
            limit = mx if mx >= 0 else _BIG
            hi = pos
            while (hi - pos) < limit and self.one(atom, hi):
                hi += 1
            if (hi - pos) < mn:
                return -1
            if greedy:
                j = hi
                while j >= pos + mn:
                    r = self.apply(rest, j)
                    if r >= 0:
                        return r
                    j -= 1
                return -1
            j = pos + mn
            while j <= hi:
                r = self.apply(rest, j)
                if r >= 0:
                    return r
                j += 1
            return -1
        more = mx < 0 or count < mx
        if greedy:
            if more:
                r = self.atom(atom, pos, ["rep", seq, i, count, pos, cont])
                if r >= 0:
                    return r
            if count >= mn:
                return self.apply(rest, pos)
            return -1
        if count >= mn:
            r = self.apply(rest, pos)
            if r >= 0:
                return r
        if more:
            return self.atom(atom, pos, ["rep", seq, i, count, pos, cont])
        return -1

    def atom(self, atom, pos, k):
        kind = atom[0]
        if kind == "char" or kind == "any" or kind == "set":
            if self.one(atom, pos):
                return self.apply(k, pos + 1)
            return -1
        if kind == "group":
            k2 = k
            if atom[2] >= 0:
                k2 = ["grp", atom[2], pos, k]
            for branch in atom[1]:
                r = self.run(branch, 0, pos, k2)
                if r >= 0:
                    return r
            return -1
        if kind == "bol":
            if pos == 0:
                return self.apply(k, pos)
            if self.multi and self.t[pos - 1] == "\n":
                return self.apply(k, pos)
            return -1
        if kind == "eol":
            if pos == self.n:
                return self.apply(k, pos)
            if self.multi and self.t[pos] == "\n":
                return self.apply(k, pos)
            return -1
        if kind == "bos":
            if pos == 0:
                return self.apply(k, pos)
            return -1
        if kind == "eos":
            if pos == self.n:
                return self.apply(k, pos)
            return -1
        if kind == "wordb":
            before = pos > 0 and _is_word(self.t[pos - 1])
            after = pos < self.n and _is_word(self.t[pos])
            at = before != after
            if atom[1]:
                at = not at
            if at:
                return self.apply(k, pos)
            return -1
        # backref
        g = atom[1]
        if g >= len(self.gs) or self.gs[g] < 0:
            return -1
        sub = self.t[self.gs[g]:self.ge[g]]
        end = pos + len(sub)
        if end > self.n:
            return -1
        if self.t[pos:end] == sub:
            return self.apply(k, end)
        return -1


class Match:
    def __init__(self, text, gs, ge, pattern):
        self.string = text
        self._gs = gs
        self._ge = ge
        self.re = pattern

    def _span(self, i):
        if i < 0 or i >= len(self._gs):
            raise IndexError("no such group " + str(i))
        return i

    def group(self, i=0):
        g = self._span(i)
        if self._gs[g] < 0:
            return None
        return self.string[self._gs[g]:self._ge[g]]

    def groups(self):
        out = []
        i = 1
        while i < len(self._gs):
            out.append(self.group(i))
            i += 1
        return out

    def start(self, i=0):
        return self._gs[self._span(i)]

    def end(self, i=0):
        return self._ge[self._span(i)]

    def span(self, i=0):
        g = self._span(i)
        return (self._gs[g], self._ge[g])


class Pattern:
    def __init__(self, pattern, flags=0):
        p = _Parser(pattern)
        self.pattern = pattern
        self.flags = flags
        self._alt = p.parse_alt()
        if not p.eof():
            p.error("unbalanced parenthesis")
        self.groups = p.ngroups
        self._first = self._first_atom()

    # If every branch must start by consuming one character, a scan can skip
    # positions that cannot possibly match instead of running the whole matcher.
    def _first_atom(self):
        first = None
        for branch in self._alt:
            if len(branch) == 0:
                return None
            term = branch[0]
            atom = term[0]
            kind = atom[0]
            if term[1] < 1 or (kind != "char" and kind != "set"):
                return None
            if first is None:
                first = atom
            elif first is not atom:
                return None
        return first

    def _attempt(self, m, text, pos, anchor_end):
        for branch in self._alt:
            m.reset()
            m.gs[0] = pos
            end = m.run(branch, 0, pos, None)
            if end >= 0 and (not anchor_end or end == len(text)):
                m.ge[0] = end
                return Match(text, m.gs, m.ge, self)
        return None

    def match(self, text, pos=0):
        return self._attempt(_M(text, self.groups, self.flags), text, pos, False)

    def fullmatch(self, text, pos=0):
        return self._attempt(_M(text, self.groups, self.flags), text, pos, True)

    def search(self, text, pos=0):
        m = _M(text, self.groups, self.flags)
        first = self._first
        i = pos
        n = len(text)
        while i <= n:
            if first is None or m.one(first, i):
                found = self._attempt(m, text, i, False)
                if found is not None:
                    return found
            i += 1
        return None

    def finditer(self, text):
        out = []
        i = 0
        n = len(text)
        while i <= n:
            m = self.search(text, i)
            if m is None:
                break
            out.append(m)
            if m.end() == m.start():
                i = m.end() + 1
            else:
                i = m.end()
        return out

    def findall(self, text):
        out = []
        for m in self.finditer(text):
            if self.groups == 0:
                out.append(m.group(0))
            elif self.groups == 1:
                out.append(_or_empty(m.group(1)))
            else:
                row = []
                i = 1
                while i <= self.groups:
                    row.append(_or_empty(m.group(i)))
                    i += 1
                out.append(row)
        return out

    def sub(self, repl, text, count=0):
        out = ""
        last = 0
        done = 0
        for m in self.finditer(text):
            out = out + text[last:m.start()] + _expand(repl, m)
            last = m.end()
            done += 1
            if count > 0 and done >= count:
                break
        return out + text[last:]

    def split(self, text, maxsplit=0):
        out = []
        last = 0
        done = 0
        for m in self.finditer(text):
            if m.end() == m.start():
                continue
            out.append(text[last:m.start()])
            last = m.end()
            done += 1
            if maxsplit > 0 and done >= maxsplit:
                break
        out.append(text[last:])
        return out


def _or_empty(v):
    if v is None:
        return ""
    return v


def _expand(repl, m):
    out = ""
    i = 0
    n = len(repl)
    while i < n:
        c = repl[i]
        if c == "\\" and i + 1 < n:
            d = repl[i + 1]
            if _is_digit(d):
                out = out + _or_empty(m.group(int(d)))
                i += 2
                continue
            if d == "g" and i + 2 < n and repl[i + 2] == "<":
                j = i + 3
                num = ""
                while j < n and repl[j] != ">":
                    num = num + repl[j]
                    j += 1
                out = out + _or_empty(m.group(int(num)))
                i = j + 1
                continue
            out = out + _unescape(d)
            i += 2
            continue
        out = out + c
        i += 1
    return out


_cache = {}


def compile(pattern, flags=0):
    key = str(flags) + "\x00" + pattern
    hit = _cache.get(key)
    if hit is not None:
        return hit
    p = Pattern(pattern, flags)
    _cache[key] = p
    return p


def match(pattern, text, flags=0):
    return compile(pattern, flags).match(text)


def fullmatch(pattern, text, flags=0):
    return compile(pattern, flags).fullmatch(text)


def search(pattern, text, flags=0):
    return compile(pattern, flags).search(text)


def findall(pattern, text, flags=0):
    return compile(pattern, flags).findall(text)


def finditer(pattern, text, flags=0):
    return compile(pattern, flags).finditer(text)


def sub(pattern, repl, text, count=0, flags=0):
    return compile(pattern, flags).sub(repl, text, count)


def split(pattern, text, maxsplit=0, flags=0):
    return compile(pattern, flags).split(text, maxsplit)


def escape(text):
    out = ""
    for c in text:
        if _is_word(c):
            out = out + c
        else:
            out = out + "\\" + c
    return out
