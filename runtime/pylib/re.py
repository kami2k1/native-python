"""Regular expressions — a real Python module, compiled to native code by kamipy.

This replaces the hand-written C++ regex engine that used to live in
runtime/src/kami_regex.cpp. Nothing here is special: the compiler translates
this file exactly the way it translates the user's program.

The engine is the classic two stage design.

  1. `_Parser` turns the pattern into a small tree of nodes.
  2. `_Emitter` flattens that tree into a program for a backtracking virtual
     machine (CHAR / CLASS / SPLIT / JMP / SAVE / ...).
  3. `_run` executes the program with an *explicit* trail stack, so matching
     never recurses and cannot overflow the native stack on long inputs.

Supported syntax: literals, `.`, `^`, `$`, `[...]` (ranges, negation, class
escapes), `(...)`, `(?:...)`, `(?P<name>...)`, `(?i)`-style inline flags, `|`,
`*`, `+`, `?`, `{m}`, `{m,}`, `{m,n}`, all with a non-greedy `?` suffix,
the escapes `\\d \\D \\w \\W \\s \\S \\b \\B \\A \\Z` and back-references `\\1`.
"""

# ---------------------------------------------------------------- flags
NOFLAG = 0
IGNORECASE = 2
I = 2
MULTILINE = 8
M = 8
DOTALL = 16
S = 16
VERBOSE = 64
X = 64

# ---------------------------------------------------------------- opcodes
_CHAR = 0
_CLASS = 1
_ANY = 2
_SPLIT = 3
_JMP = 4
_SAVE = 5
_BOL = 6
_EOL = 7
_WORDB = 8
_BACKREF = 9
_MARK = 10
_PROGRESS = 11
_STREND = 12
_MATCH = 13

_WORD_EXTRA = "_"


def _fail(msg):
    raise ValueError("re.error: " + msg)


def _is_word(ch):
    return ch.isalnum() or ch == _WORD_EXTRA


def _lower(ch):
    return ch.lower()


# ---------------------------------------------------------------- parser
# Node shapes (plain lists so no extra classes are needed):
#   ["char", ch]                  a single literal character
#   ["any"]                       .
#   ["class", negated, items]     items: ["ch",c] / ["rng",lo,hi] / ["cat",name]
#   ["cat", [node, ...]]          concatenation
#   ["alt", [node, ...]]          alternation
#   ["rep", node, min, max, greedy]   max < 0 means unbounded
#   ["group", index, node]        index < 0 for (?:...)
#   ["bol"] ["eol"] ["strstart"] ["strend"]
#   ["wordb", negated]
#   ["backref", n]


class _Parser:
    def __init__(self, pattern, flags):
        self.p = pattern
        self.i = 0
        self.n = len(pattern)
        self.flags = flags
        self.ngroups = 0
        self.names = {}

    def at_end(self):
        return self.i >= self.n

    def peek(self):
        if self.i >= self.n:
            return ""
        return self.p[self.i]

    def next(self):
        ch = self.p[self.i]
        self.i = self.i + 1
        return ch

    def eat(self, ch):
        if self.peek() == ch:
            self.i = self.i + 1
            return True
        return False

    # alternation := concat ("|" concat)*
    def parse_alt(self):
        branches = [self.parse_cat()]
        while self.eat("|"):
            branches.append(self.parse_cat())
        if len(branches) == 1:
            return branches[0]
        return ["alt", branches]

    def parse_cat(self):
        items = []
        while not self.at_end():
            ch = self.peek()
            if ch == "|" or ch == ")":
                break
            items.append(self.parse_quantified())
        return ["cat", items]

    def parse_quantified(self):
        atom = self.parse_atom()
        while True:
            ch = self.peek()
            if ch == "*":
                self.i = self.i + 1
                atom = self.wrap_rep(atom, 0, -1)
            elif ch == "+":
                self.i = self.i + 1
                atom = self.wrap_rep(atom, 1, -1)
            elif ch == "?":
                self.i = self.i + 1
                atom = self.wrap_rep(atom, 0, 1)
            elif ch == "{":
                save = self.i
                bounds = self.try_braces()
                if bounds is None:
                    self.i = save
                    break
                atom = self.wrap_rep(atom, bounds[0], bounds[1])
            else:
                break
        return atom

    # "{m}", "{m,}", "{m,n}" — anything else is a literal brace.
    def try_braces(self):
        self.i = self.i + 1  # skip '{'
        lo = ""
        while not self.at_end() and self.peek().isdigit():
            lo = lo + self.next()
        if lo == "":
            return None
        hi = lo
        if self.eat(","):
            hi = ""
            while not self.at_end() and self.peek().isdigit():
                hi = hi + self.next()
        if not self.eat("}"):
            return None
        low = int(lo)
        if hi == "":
            return [low, -1]
        high = int(hi)
        if high < low:
            _fail("min repeat greater than max repeat")
        return [low, high]

    def wrap_rep(self, atom, lo, hi):
        greedy = True
        if self.peek() == "?":
            self.i = self.i + 1
            greedy = False
        elif self.peek() == "+":
            self.i = self.i + 1  # possessive: treated as greedy
        kind = atom[0]
        if kind == "bol" or kind == "eol" or kind == "wordb":
            _fail("nothing to repeat")
        return ["rep", atom, lo, hi, greedy]

    def parse_atom(self):
        if self.at_end():
            _fail("unexpected end of pattern")
        ch = self.next()
        if ch == "(":
            return self.parse_group()
        if ch == "[":
            return self.parse_class()
        if ch == ".":
            return ["any"]
        if ch == "^":
            return ["bol"]
        if ch == "$":
            return ["eol"]
        if ch == "*" or ch == "+" or ch == "?":
            _fail("nothing to repeat")
        if ch == ")":
            _fail("unbalanced parenthesis")
        if ch == "\\":
            return self.parse_escape()
        return ["char", ch]

    def parse_group(self):
        index = -1
        if self.eat("?"):
            nxt = self.peek()
            if nxt == ":":
                self.i = self.i + 1
            elif nxt == "P":
                self.i = self.i + 1
                if not self.eat("<"):
                    _fail("unknown extension ?P")
                name = ""
                while not self.at_end() and self.peek() != ">":
                    name = name + self.next()
                if not self.eat(">"):
                    _fail("missing > in group name")
                self.ngroups = self.ngroups + 1
                index = self.ngroups
                self.names[name] = index
            elif nxt == "#":
                # comment group: swallow it, contributes nothing
                while not self.at_end() and self.peek() != ")":
                    self.i = self.i + 1
                if not self.eat(")"):
                    _fail("missing ) in comment group")
                return ["cat", []]
            elif nxt == "=" or nxt == "!" or nxt == "<":
                _fail("look-around assertions are not supported")
            else:
                # inline flags: (?i), (?im), (?is:...)
                added = 0
                while not self.at_end():
                    f = self.peek()
                    if f == "i":
                        added = added | IGNORECASE
                    elif f == "m":
                        added = added | MULTILINE
                    elif f == "s":
                        added = added | DOTALL
                    elif f == "x":
                        added = added | VERBOSE
                    else:
                        break
                    self.i = self.i + 1
                self.flags = self.flags | added
                if self.eat(")"):
                    return ["cat", []]
                if not self.eat(":"):
                    _fail("missing : in inline flag group")
        else:
            self.ngroups = self.ngroups + 1
            index = self.ngroups
        inner = self.parse_alt()
        if not self.eat(")"):
            _fail("missing ), unterminated subpattern")
        return ["group", index, inner]

    def parse_class(self):
        negated = self.eat("^")
        items = []
        first = True
        while True:
            if self.at_end():
                _fail("unterminated character set")
            ch = self.next()
            if ch == "]" and not first:
                break
            first = False
            if ch == "\\":
                esc = self.class_escape()
                if esc[0] == "cat":
                    items.append(esc)
                    continue
                ch = esc[1]
            if self.peek() == "-" and self.i + 1 < self.n and self.p[self.i + 1] != "]":
                self.i = self.i + 1
                hi = self.next()
                if hi == "\\":
                    esc = self.class_escape()
                    if esc[0] == "cat":
                        _fail("bad character range")
                    hi = esc[1]
                items.append(["rng", ch, hi])
            else:
                items.append(["ch", ch])
        return ["class", negated, items]

    # An escape inside [...]: either a category (\d) or a literal character.
    def class_escape(self):
        if self.at_end():
            _fail("bad escape (end of pattern)")
        ch = self.next()
        if ch in "dDwWsS":
            return ["cat", ch]
        return ["ch", _unescape_char(ch)]

    def parse_escape(self):
        if self.at_end():
            _fail("bad escape (end of pattern)")
        ch = self.next()
        if ch in "dDwWsS":
            return ["class", False, [["cat", ch]]]
        if ch == "b":
            return ["wordb", False]
        if ch == "B":
            return ["wordb", True]
        if ch == "A":
            return ["strstart"]
        if ch == "Z" or ch == "z":
            return ["strend"]
        if ch.isdigit() and ch != "0":
            num = ch
            while not self.at_end() and self.peek().isdigit():
                num = num + self.next()
            return ["backref", int(num)]
        return ["char", _unescape_char(ch)]


def _unescape_char(ch):
    if ch == "n":
        return "\n"
    if ch == "t":
        return "\t"
    if ch == "r":
        return "\r"
    if ch == "f":
        return "\f"
    if ch == "v":
        return "\v"
    if ch == "0":
        return "\0"
    if ch == "a":
        return "\a"
    return ch


# ---------------------------------------------------------------- emitter
class _Emitter:
    def __init__(self):
        self.prog = []
        self.nmarks = 0

    def add(self, op, a, b):
        self.prog.append([op, a, b])
        return len(self.prog) - 1

    def here(self):
        return len(self.prog)

    def emit(self, node):
        kind = node[0]
        if kind == "char":
            self.add(_CHAR, node[1], 0)
        elif kind == "any":
            self.add(_ANY, 0, 0)
        elif kind == "class":
            self.add(_CLASS, node[2], node[1])
        elif kind == "cat":
            for kid in node[1]:
                self.emit(kid)
        elif kind == "bol":
            self.add(_BOL, 0, 0)
        elif kind == "eol":
            self.add(_EOL, 0, 0)
        elif kind == "strstart":
            self.add(_BOL, 0, 1)
        elif kind == "strend":
            self.add(_STREND, 0, 0)
        elif kind == "wordb":
            self.add(_WORDB, 1 if node[1] else 0, 0)
        elif kind == "backref":
            self.add(_BACKREF, node[1], 0)
        elif kind == "group":
            idx = node[1]
            if idx < 0:
                self.emit(node[2])
            else:
                self.add(_SAVE, idx * 2, 0)
                self.emit(node[2])
                self.add(_SAVE, idx * 2 + 1, 0)
        elif kind == "alt":
            self.emit_alt(node[1])
        elif kind == "rep":
            self.emit_rep(node[1], node[2], node[3], node[4])
        else:
            _fail("internal: unknown node " + str(kind))

    def emit_alt(self, branches):
        jumps = []
        k = 0
        last = len(branches) - 1
        while k < last:
            sp = self.add(_SPLIT, 0, 0)
            self.prog[sp][1] = self.here()
            self.emit(branches[k])
            jumps.append(self.add(_JMP, 0, 0))
            self.prog[sp][2] = self.here()
            k = k + 1
        self.emit(branches[last])
        end = self.here()
        for j in jumps:
            self.prog[j][1] = end

    def emit_rep(self, node, lo, hi, greedy):
        k = 0
        while k < lo:
            self.emit(node)
            k = k + 1
        if hi < 0:
            # x* / x+ tail: split → body → jump back. The MARK/PROGRESS pair
            # kills the "empty body loops forever" case, e.g. (a*)* .
            slot = self.nmarks
            self.nmarks = self.nmarks + 1
            top = self.add(_SPLIT, 0, 0)
            body = self.here()
            self.add(_MARK, slot, 0)
            self.emit(node)
            self.add(_PROGRESS, slot, 0)
            self.add(_JMP, top, 0)
            out = self.here()
            if greedy:
                self.prog[top][1] = body
                self.prog[top][2] = out
            else:
                self.prog[top][1] = out
                self.prog[top][2] = body
            return
        # bounded: (hi - lo) optional copies, each guarded by its own split
        splits = []
        k = 0
        while k < hi - lo:
            sp = self.add(_SPLIT, 0, 0)
            body = self.here()
            splits.append([sp, body])
            self.emit(node)
            k = k + 1
        end = self.here()
        for pair in splits:
            sp = pair[0]
            body = pair[1]
            if greedy:
                self.prog[sp][1] = body
                self.prog[sp][2] = end
            else:
                self.prog[sp][1] = end
                self.prog[sp][2] = body


def _compile_prog(node, anchored_end):
    e = _Emitter()
    e.add(_SAVE, 0, 0)
    e.emit(node)
    if anchored_end:
        e.add(_STREND, 0, 0)
    e.add(_SAVE, 1, 0)
    e.add(_MATCH, 0, 0)
    return e


# ---------------------------------------------------------------- matcher
def _class_match(ch, items, negated, fold):
    hit = False
    for it in items:
        kind = it[0]
        if kind == "ch":
            if ch == it[1] or (fold and _lower(ch) == _lower(it[1])):
                hit = True
        elif kind == "rng":
            lo = it[1]
            hi = it[2]
            if lo <= ch and ch <= hi:
                hit = True
            elif fold:
                c2 = ch.lower()
                c3 = ch.upper()
                if (lo <= c2 and c2 <= hi) or (lo <= c3 and c3 <= hi):
                    hit = True
        else:
            if _cat_match(ch, it[1]):
                hit = True
        if hit:
            break
    if negated:
        return not hit
    return hit


def _cat_match(ch, name):
    if name == "d":
        return ch.isdigit()
    if name == "D":
        return not ch.isdigit()
    if name == "w":
        return _is_word(ch)
    if name == "W":
        return not _is_word(ch)
    if name == "s":
        return ch == " " or ch == "\t" or ch == "\n" or ch == "\r" or ch == "\f" or ch == "\v"
    return not (ch == " " or ch == "\t" or ch == "\n" or ch == "\r" or ch == "\f" or ch == "\v")


def _run(prog, s, start, ncaps, nmarks, flags):
    """Runs the VM anchored at `start`. Returns the capture vector or None."""
    fold = (flags & IGNORECASE) != 0
    dotall = (flags & DOTALL) != 0
    multiline = (flags & MULTILINE) != 0
    n = len(s)
    caps = [-1] * ncaps
    marks = [-1] * nmarks
    trail = []
    pc = 0
    sp = start
    while True:
        ins = prog[pc]
        op = ins[0]
        bad = False
        if op == _CHAR:
            if sp < n and (s[sp] == ins[1] or (fold and _lower(s[sp]) == _lower(ins[1]))):
                sp = sp + 1
                pc = pc + 1
            else:
                bad = True
        elif op == _CLASS:
            if sp < n and _class_match(s[sp], ins[1], ins[2], fold):
                sp = sp + 1
                pc = pc + 1
            else:
                bad = True
        elif op == _ANY:
            if sp < n and (dotall or s[sp] != "\n"):
                sp = sp + 1
                pc = pc + 1
            else:
                bad = True
        elif op == _SPLIT:
            trail.append([ins[2], sp, caps[:], marks[:]])
            pc = ins[1]
        elif op == _JMP:
            pc = ins[1]
        elif op == _SAVE:
            caps[ins[1]] = sp
            pc = pc + 1
        elif op == _MARK:
            marks[ins[1]] = sp
            pc = pc + 1
        elif op == _PROGRESS:
            if sp == marks[ins[1]]:
                bad = True
            else:
                pc = pc + 1
        elif op == _BOL:
            if sp == 0 or (multiline and ins[2] == 0 and s[sp - 1] == "\n"):
                pc = pc + 1
            else:
                bad = True
        elif op == _EOL:
            if sp == n or (multiline and s[sp] == "\n"):
                pc = pc + 1
            else:
                bad = True
        elif op == _STREND:
            if sp == n:
                pc = pc + 1
            else:
                bad = True
        elif op == _WORDB:
            before = sp > 0 and _is_word(s[sp - 1])
            after = sp < n and _is_word(s[sp])
            on_edge = before != after
            if ins[1] == 1:
                on_edge = not on_edge
            if on_edge:
                pc = pc + 1
            else:
                bad = True
        elif op == _BACKREF:
            gi = ins[1] * 2
            a = caps[gi]
            b = caps[gi + 1]
            if a < 0 or b < 0:
                bad = True
            else:
                want = s[a:b]
                got = s[sp:sp + len(want)]
                if got == want or (fold and got.lower() == want.lower()):
                    sp = sp + len(want)
                    pc = pc + 1
                else:
                    bad = True
        else:
            return caps
        if bad:
            if len(trail) == 0:
                return None
            frame = trail.pop()
            pc = frame[0]
            sp = frame[1]
            caps = frame[2]
            marks = frame[3]


# ---------------------------------------------------------------- Match
class Match:
    def __init__(self, pattern, string, caps, pos, endpos):
        self.re = pattern
        self.string = string
        self.pos = pos
        self.endpos = endpos
        self._caps = caps
        self.lastindex = None
        g = 1
        while g <= pattern.groups:
            if caps[g * 2] >= 0 and caps[g * 2 + 1] >= 0:
                self.lastindex = g
            g = g + 1

    def _index(self, key):
        if isinstance(key, str):
            idx = self.re.groupindex.get(key)
            if idx is None:
                raise ValueError("IndexError: no such group '" + key + "'")
            return idx
        if key < 0 or key > self.re.groups:
            raise ValueError("IndexError: no such group " + str(key))
        return key

    def _slice(self, key):
        i = self._index(key)
        a = self._caps[i * 2]
        b = self._caps[i * 2 + 1]
        if a < 0 or b < 0:
            return None
        return self.string[a:b]

    def group(self, *which):
        if len(which) == 0:
            return self._slice(0)
        if len(which) == 1:
            return self._slice(which[0])
        out = []
        for w in which:
            out.append(self._slice(w))
        return out

    def groups(self, default=None):
        out = []
        g = 1
        while g <= self.re.groups:
            v = self._slice(g)
            out.append(default if v is None else v)
            g = g + 1
        return out

    def groupdict(self, default=None):
        out = {}
        for name in self.re.groupindex.keys():
            v = self._slice(self.re.groupindex[name])
            out[name] = default if v is None else v
        return out

    def start(self, group=0):
        return self._caps[self._index(group) * 2]

    def end(self, group=0):
        return self._caps[self._index(group) * 2 + 1]

    def span(self, group=0):
        i = self._index(group)
        return [self._caps[i * 2], self._caps[i * 2 + 1]]

    def expand(self, template):
        return _expand(self, template)

    def __repr__(self):
        return "<re.Match span=(" + str(self.start()) + ", " + str(self.end()) + ")>"


# ---------------------------------------------------------------- Pattern
class Pattern:
    def __init__(self, pattern, flags=0):
        self.pattern = pattern
        src = pattern
        if (flags & VERBOSE) != 0:
            src = _strip_verbose(pattern)
        p = _Parser(src, flags)
        tree = p.parse_alt()
        if not p.at_end():
            _fail("unbalanced parenthesis in " + repr(pattern))
        self.flags = p.flags
        self.groups = p.ngroups
        self.groupindex = p.names
        e = _compile_prog(tree, False)
        self._prog = e.prog
        self._nmarks = e.nmarks
        ef = _compile_prog(tree, True)
        self._fullprog = ef.prog
        self._fullmarks = ef.nmarks
        self._ncaps = (p.ngroups + 1) * 2

    def _wrap(self, string, caps, endpos):
        if caps is None:
            return None
        return Match(self, string, caps, 0, endpos)

    def match(self, string, pos=0, endpos=-1):
        text = string if endpos < 0 else string[:endpos]
        caps = _run(self._prog, text, pos, self._ncaps, self._nmarks, self.flags)
        return self._wrap(text, caps, len(text))

    def fullmatch(self, string, pos=0, endpos=-1):
        text = string if endpos < 0 else string[:endpos]
        caps = _run(self._fullprog, text, pos, self._ncaps, self._fullmarks, self.flags)
        return self._wrap(text, caps, len(text))

    def search(self, string, pos=0, endpos=-1):
        text = string if endpos < 0 else string[:endpos]
        at = pos
        limit = len(text)
        while at <= limit:
            caps = _run(self._prog, text, at, self._ncaps, self._nmarks, self.flags)
            if caps is not None:
                return self._wrap(text, caps, limit)
            at = at + 1
        return None

    def finditer(self, string, pos=0):
        # No generators in the compiler yet, so this eagerly returns a list.
        out = []
        at = pos
        limit = len(string)
        while at <= limit:
            m = self.search(string, at)
            if m is None:
                break
            out.append(m)
            if m.end() == m.start():
                at = m.end() + 1
            else:
                at = m.end()
        return out

    def findall(self, string, pos=0):
        out = []
        for m in self.finditer(string, pos):
            if self.groups == 0:
                out.append(m.group(0))
            elif self.groups == 1:
                v = m.group(1)
                out.append("" if v is None else v)
            else:
                row = []
                g = 1
                while g <= self.groups:
                    v = m.group(g)
                    row.append("" if v is None else v)
                    g = g + 1
                out.append(row)
        return out

    def subn(self, repl, string, count=0):
        out = ""
        last = 0
        made = 0
        for m in self.finditer(string):
            if count > 0 and made >= count:
                break
            out = out + string[last:m.start()]
            if callable(repl):
                out = out + str(repl(m))
            else:
                out = out + _expand(m, repl)
            last = m.end()
            made = made + 1
        return [out + string[last:], made]

    def sub(self, repl, string, count=0):
        return self.subn(repl, string, count)[0]

    def split(self, string, maxsplit=0):
        out = []
        last = 0
        made = 0
        for m in self.finditer(string):
            if maxsplit > 0 and made >= maxsplit:
                break
            if m.end() == m.start() and m.start() == last:
                continue
            out.append(string[last:m.start()])
            g = 1
            while g <= self.groups:
                out.append(m.group(g))
                g = g + 1
            last = m.end()
            made = made + 1
        out.append(string[last:])
        return out

    def __repr__(self):
        return "re.compile(" + repr(self.pattern) + ")"


def _strip_verbose(pattern):
    out = ""
    i = 0
    n = len(pattern)
    while i < n:
        ch = pattern[i]
        if ch == "\\" and i + 1 < n:
            out = out + ch + pattern[i + 1]
            i = i + 2
            continue
        if ch == "#":
            while i < n and pattern[i] != "\n":
                i = i + 1
            continue
        if ch == " " or ch == "\t" or ch == "\n" or ch == "\r":
            i = i + 1
            continue
        out = out + ch
        i = i + 1
    return out


# `\1`, `\g<1>`, `\g<name>`, `\\`, `\n` inside a replacement template.
def _expand(match, template):
    out = ""
    i = 0
    n = len(template)
    while i < n:
        ch = template[i]
        if ch != "\\":
            out = out + ch
            i = i + 1
            continue
        i = i + 1
        if i >= n:
            _fail("bad escape (end of replacement)")
        ch = template[i]
        i = i + 1
        if ch.isdigit():
            num = ch
            while i < n and template[i].isdigit() and len(num) < 2:
                num = num + template[i]
                i = i + 1
            v = match.group(int(num))
            out = out + ("" if v is None else v)
        elif ch == "g":
            if i >= n or template[i] != "<":
                _fail("missing < in group reference")
            i = i + 1
            name = ""
            while i < n and template[i] != ">":
                name = name + template[i]
                i = i + 1
            if i >= n:
                _fail("missing > in group reference")
            i = i + 1
            key = int(name) if name.isdigit() else name
            v = match.group(key)
            out = out + ("" if v is None else v)
        else:
            out = out + _unescape_char(ch)
    return out


# ---------------------------------------------------------------- module API
_cache = {}


def compile(pattern, flags=0):
    if isinstance(pattern, Pattern):
        return pattern
    key = str(flags) + "\x00" + pattern
    hit = _cache.get(key)
    if hit is not None:
        return hit
    rx = Pattern(pattern, flags)
    if len(_cache) > 256:
        _cache.clear()
    _cache[key] = rx
    return rx


def match(pattern, string, flags=0):
    return compile(pattern, flags).match(string)


def fullmatch(pattern, string, flags=0):
    return compile(pattern, flags).fullmatch(string)


def search(pattern, string, flags=0):
    return compile(pattern, flags).search(string)


def findall(pattern, string, flags=0):
    return compile(pattern, flags).findall(string)


def finditer(pattern, string, flags=0):
    return compile(pattern, flags).finditer(string)


def sub(pattern, repl, string, count=0, flags=0):
    return compile(pattern, flags).sub(repl, string, count)


def subn(pattern, repl, string, count=0, flags=0):
    return compile(pattern, flags).subn(repl, string, count)


def split(pattern, string, maxsplit=0, flags=0):
    return compile(pattern, flags).split(string, maxsplit)


def escape(pattern):
    out = ""
    for ch in pattern:
        if ch.isalnum() or ch == "_":
            out = out + ch
        else:
            out = out + "\\" + ch
    return out


def purge():
    _cache.clear()
