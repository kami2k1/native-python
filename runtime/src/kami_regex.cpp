// A compact backtracking regular-expression engine for KamiPython's `re`.
//
// Supported syntax (a practical subset of Python's re):
//   literals, '.', '*', '+', '?', '{m}', '{m,}', '{m,n}' (greedy + lazy '?'),
//   character classes '[...]' with ranges and negation '[^...]',
//   anchors '^' '$', groups '(...)', non-capturing '(?:...)', alternation '|',
//   escapes: \d \D \w \W \s \S \b \B \n \t \r \\ and escaped metachars.
// Flags: re.IGNORECASE via (?i) is not parsed; pass through as literal — callers
// that need it will get a clear mismatch rather than a crash.
#include "kami_regex.h"

#include <cctype>
#include <cstring>
#include <functional>

namespace kami {
namespace re {

// ---------------- AST ----------------
enum class NKind { Char, Any, Class, Star, Plus, Quest, Repeat, Concat, Alt, Group, Anchor, Word };

struct Node {
    NKind kind;
    // Char
    char ch = 0;
    // Class: sorted list of [lo,hi] ranges + negate + special sets
    bool negate = false;
    std::vector<std::pair<unsigned char, unsigned char>> ranges;
    std::string classSpecials; // 'd','w','s','D','W','S'
    // quantifiers
    bool lazy = false;
    int rmin = 0, rmax = 0; // Repeat
    // children
    std::vector<Node*> kids;
    // Group
    int groupIndex = -1; // -1 => non-capturing
    // Anchor: ch = '^' or '$'; Word: ch='b' or 'B'
};

struct Arena {
    std::vector<Node*> all;
    Node* make(NKind k) {
        Node* n = new Node();
        n->kind = k;
        all.push_back(n);
        return n;
    }
    ~Arena() {
        for (Node* n : all) delete n;
    }
};

// ---------------- parser ----------------
struct ParseError {
    std::string msg;
};

struct Parser {
    const std::string& src;
    size_t pos = 0;
    Arena& arena;
    int& groupCount;

    Parser(const std::string& s, Arena& a, int& gc) : src(s), arena(a), groupCount(gc) {}

    char peek() { return pos < src.size() ? src[pos] : '\0'; }
    char get() { return src[pos++]; }
    bool eof() { return pos >= src.size(); }
    [[noreturn]] void fail(const std::string& m) { throw ParseError{m}; }

    Node* parse() {
        Node* n = parseAlt();
        if (!eof()) fail("unexpected ')' or trailing input");
        return n;
    }

    Node* parseAlt() {
        Node* left = parseConcat();
        if (peek() != '|') return left;
        Node* alt = arena.make(NKind::Alt);
        alt->kids.push_back(left);
        while (peek() == '|') {
            get();
            alt->kids.push_back(parseConcat());
        }
        return alt;
    }

    Node* parseConcat() {
        Node* cat = arena.make(NKind::Concat);
        while (!eof() && peek() != '|' && peek() != ')') {
            cat->kids.push_back(parseQuant());
        }
        return cat;
    }

    Node* parseQuant() {
        Node* atom = parseAtom();
        char c = peek();
        Node* q = nullptr;
        if (c == '*') { get(); q = arena.make(NKind::Star); q->kids.push_back(atom); }
        else if (c == '+') { get(); q = arena.make(NKind::Plus); q->kids.push_back(atom); }
        else if (c == '?') { get(); q = arena.make(NKind::Quest); q->kids.push_back(atom); }
        else if (c == '{') {
            size_t save = pos;
            get();
            std::string a, b;
            bool comma = false;
            while (isdigit((unsigned char)peek())) a += get();
            if (peek() == ',') { comma = true; get(); while (isdigit((unsigned char)peek())) b += get(); }
            if (peek() != '}') { pos = save; return atom; } // literal '{'
            get();
            if (a.empty() && !comma) { pos = save; return atom; }
            q = arena.make(NKind::Repeat);
            q->kids.push_back(atom);
            q->rmin = a.empty() ? 0 : atoi(a.c_str());
            q->rmax = comma ? (b.empty() ? -1 : atoi(b.c_str())) : q->rmin;
        } else {
            return atom;
        }
        if (peek() == '?') { get(); q->lazy = true; }
        return q;
    }

    Node* parseAtom() {
        char c = peek();
        if (c == '(') {
            get();
            int gidx = -1;
            if (peek() == '?') {
                get();
                char t = get();
                if (t != ':') fail("only (?:...) non-capturing groups are supported");
            } else {
                gidx = ++groupCount;
            }
            Node* inner = parseAlt();
            if (peek() != ')') fail("missing ')'");
            get();
            Node* g = arena.make(NKind::Group);
            g->groupIndex = gidx;
            g->kids.push_back(inner);
            return g;
        }
        if (c == '[') return parseClass();
        if (c == '.') { get(); return arena.make(NKind::Any); }
        if (c == '^') { get(); Node* n = arena.make(NKind::Anchor); n->ch = '^'; return n; }
        if (c == '$') { get(); Node* n = arena.make(NKind::Anchor); n->ch = '$'; return n; }
        if (c == '\\') {
            get();
            char e = get();
            if (e == 'b' || e == 'B') { Node* n = arena.make(NKind::Word); n->ch = e; return n; }
            if (strchr("dDwWsS", e)) {
                Node* n = arena.make(NKind::Class);
                n->classSpecials += e;
                return n;
            }
            Node* n = arena.make(NKind::Char);
            n->ch = unescape(e);
            return n;
        }
        if (c == '\0' && eof()) fail("unexpected end of pattern");
        get();
        Node* n = arena.make(NKind::Char);
        n->ch = c;
        return n;
    }

    static char unescape(char e) {
        switch (e) {
        case 'n': return '\n';
        case 't': return '\t';
        case 'r': return '\r';
        case 'f': return '\f';
        case 'v': return '\v';
        case '0': return '\0';
        default: return e;
        }
    }

    Node* parseClass() {
        get(); // '['
        Node* n = arena.make(NKind::Class);
        if (peek() == '^') { get(); n->negate = true; }
        bool first = true;
        while (!eof() && (peek() != ']' || first)) {
            first = false;
            char c = get();
            if (c == '\\') {
                char e = get();
                if (strchr("dDwWsS", e)) { n->classSpecials += e; continue; }
                c = unescape(e);
            }
            if (peek() == '-' && pos + 1 < src.size() && src[pos + 1] != ']') {
                get(); // '-'
                char hi = get();
                if (hi == '\\') hi = unescape(get());
                n->ranges.push_back({(unsigned char)c, (unsigned char)hi});
            } else {
                n->ranges.push_back({(unsigned char)c, (unsigned char)c});
            }
        }
        if (peek() != ']') fail("missing ']'");
        get();
        return n;
    }
};

static bool classMatch(const Node* n, unsigned char c) {
    bool m = false;
    for (auto& r : n->ranges)
        if (c >= r.first && c <= r.second) { m = true; break; }
    if (!m) {
        for (char sp : n->classSpecials) {
            switch (sp) {
            case 'd': if (isdigit(c)) m = true; break;
            case 'D': if (!isdigit(c)) m = true; break;
            case 'w': if (isalnum(c) || c == '_') m = true; break;
            case 'W': if (!(isalnum(c) || c == '_')) m = true; break;
            case 's': if (isspace(c)) m = true; break;
            case 'S': if (!isspace(c)) m = true; break;
            }
            if (m) break;
        }
    }
    return n->negate ? !m : m;
}

// ---------------- matcher (backtracking, continuation-passing) ----------------
struct Matcher {
    const std::string& text;
    std::vector<int>& gstart;
    std::vector<int>& gend;

    Matcher(const std::string& t, std::vector<int>& gs, std::vector<int>& ge)
        : text(t), gstart(gs), gend(ge) {}

    static bool isWord(char c) { return isalnum((unsigned char)c) || c == '_'; }

    using Cont = std::function<bool(int)>;

    // Match node n starting at pos; on success call cont(newpos). Returns true
    // if some path (node + continuation) succeeded.
    bool match(const Node* n, int pos, const Cont& cont);

    bool matchSeq(const std::vector<Node*>& kids, size_t i, int pos, const Cont& cont) {
        if (i == kids.size()) return cont(pos);
        return match(kids[i], pos, [&](int np) { return matchSeq(kids, i + 1, np, cont); });
    }

    bool matchRepeat(const Node* atom, int count, int minc, int maxc, bool lazy, int pos,
                     const Cont& cont) {
        bool canStop = count >= minc;
        bool canMore = (maxc < 0) || (count < maxc);
        if (lazy) {
            if (canStop && cont(pos)) return true;
            if (canMore)
                return match(atom, pos, [&](int np) {
                    if (np == pos) return false; // avoid infinite loop on empty match
                    return matchRepeat(atom, count + 1, minc, maxc, lazy, np, cont);
                });
            return false;
        }
        if (canMore) {
            if (match(atom, pos, [&](int np) {
                    if (np == pos) return false;
                    return matchRepeat(atom, count + 1, minc, maxc, lazy, np, cont);
                }))
                return true;
        }
        if (canStop) return cont(pos);
        return false;
    }
};

bool Matcher::match(const Node* n, int pos, const Cont& cont) {
    int len = (int)text.size();
    switch (n->kind) {
    case NKind::Char:
        if (pos < len && text[pos] == n->ch) return cont(pos + 1);
        return false;
    case NKind::Any:
        if (pos < len && text[pos] != '\n') return cont(pos + 1);
        return false;
    case NKind::Class:
        if (pos < len && classMatch(n, (unsigned char)text[pos])) return cont(pos + 1);
        return false;
    case NKind::Anchor:
        if (n->ch == '^') return pos == 0 ? cont(pos) : false;
        return pos == len ? cont(pos) : false;
    case NKind::Word: {
        bool before = pos > 0 && isWord(text[pos - 1]);
        bool after = pos < len && isWord(text[pos]);
        bool boundary = before != after;
        if (n->ch == 'b') return boundary ? cont(pos) : false;
        return !boundary ? cont(pos) : false;
    }
    case NKind::Concat:
        return matchSeq(n->kids, 0, pos, cont);
    case NKind::Alt:
        for (Node* k : n->kids)
            if (match(k, pos, cont)) return true;
        return false;
    case NKind::Group: {
        int gi = n->groupIndex;
        if (gi < 0) return match(n->kids[0], pos, cont);
        int savedS = gstart[gi], savedE = gend[gi];
        gstart[gi] = pos;
        bool ok = match(n->kids[0], pos, [&](int np) {
            int se = gend[gi];
            gend[gi] = np;
            if (cont(np)) return true;
            gend[gi] = se;
            return false;
        });
        if (!ok) { gstart[gi] = savedS; gend[gi] = savedE; }
        return ok;
    }
    case NKind::Star: return matchRepeat(n->kids[0], 0, 0, -1, n->lazy, pos, cont);
    case NKind::Plus: return matchRepeat(n->kids[0], 0, 1, -1, n->lazy, pos, cont);
    case NKind::Quest: return matchRepeat(n->kids[0], 0, 0, 1, n->lazy, pos, cont);
    case NKind::Repeat:
        return matchRepeat(n->kids[0], 0, n->rmin, n->rmax, n->lazy, pos, cont);
    }
    return false;
}

// ---------------- public API (pImpl) ----------------
struct Regex::Impl {
    Arena arena;
    Node* root = nullptr;
    int groupCount = 0;
    std::string error;

    explicit Impl(const std::string& pattern) {
        try {
            Parser p(pattern, arena, groupCount);
            root = p.parse();
        } catch (ParseError& e) {
            error = e.msg;
        }
    }

    bool matchAt(const std::string& text, int start, std::vector<int>& gs, std::vector<int>& ge,
                 bool fullOnly) {
        gs.assign((size_t)groupCount + 1, -1);
        ge.assign((size_t)groupCount + 1, -1);
        Matcher m(text, gs, ge);
        int endText = (int)text.size();
        gs[0] = start;
        return m.match(root, start, [&](int np) {
            if (fullOnly && np != endText) return false;
            ge[0] = np;
            return true;
        });
    }

    int search(const std::string& text, int from, std::vector<int>& gs, std::vector<int>& ge) {
        for (int i = from; i <= (int)text.size(); i++)
            if (matchAt(text, i, gs, ge, false)) return i;
        return -1;
    }
};

Regex::Regex(const std::string& pattern) : impl(new Impl(pattern)) {}
Regex::~Regex() { delete impl; }
bool Regex::ok() const { return impl->error.empty() && impl->root != nullptr; }
const std::string& Regex::err() const { return impl->error; }
int Regex::group_count() const { return impl->groupCount; }
bool Regex::matchAt(const std::string& text, int start, std::vector<int>& gs,
                    std::vector<int>& ge, bool fullOnly) {
    return impl->matchAt(text, start, gs, ge, fullOnly);
}
int Regex::search(const std::string& text, int from, std::vector<int>& gs, std::vector<int>& ge) {
    return impl->search(text, from, gs, ge);
}

} // namespace re
} // namespace kami

