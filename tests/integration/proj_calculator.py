# Real project: an arithmetic expression interpreter (tokenizer + recursive
# descent parser + evaluator) written in KamiPython and compiled to native.
# Grammar: expr := term (('+'|'-') term)* ; term := factor (('*'|'/') factor)*
#          factor := NUM | '(' expr ')' | '-' factor        ('/' = integer div)

def tokenize(s):
    toks = []
    i = 0
    n = len(s)
    while i < n:
        c = s[i]
        if c == " ":
            i += 1
            continue
        if c >= "0" and c <= "9":
            num = 0
            while i < n and s[i] >= "0" and s[i] <= "9":
                num = num * 10 + (ord(s[i]) - 48)
                i += 1
            toks.append(["num", num])
            continue
        toks.append(["op", c])
        i += 1
    return toks

def peek(st):
    if st[1] < len(st[0]):
        return st[0][st[1]]
    return ["end", ""]

def advance(st):
    t = peek(st)
    st[1] = st[1] + 1
    return t

def parse_factor(st):
    t = peek(st)
    if t[0] == "num":
        advance(st)
        return t[1]
    if t[0] == "op" and t[1] == "(":
        advance(st)
        v = parse_expr(st)
        advance(st)  # ')'
        return v
    if t[0] == "op" and t[1] == "-":
        advance(st)
        return -parse_factor(st)
    print("parse error at", t[1])
    return 0

def parse_term(st):
    v = parse_factor(st)
    while True:
        t = peek(st)
        if t[0] == "op" and (t[1] == "*" or t[1] == "/"):
            advance(st)
            r = parse_factor(st)
            if t[1] == "*":
                v = v * r
            else:
                v = v // r
        else:
            break
    return v

def parse_expr(st):
    v = parse_term(st)
    while True:
        t = peek(st)
        if t[0] == "op" and (t[1] == "+" or t[1] == "-"):
            advance(st)
            r = parse_term(st)
            if t[1] == "+":
                v = v + r
            else:
                v = v - r
        else:
            break
    return v

def calc(s):
    st = [tokenize(s), 0]
    return parse_expr(st)

print(calc("1 + 2 * 3"))
print(calc("(1 + 2) * (4 - 1)"))
print(calc("100 / 7"))
print(calc("-5 + 3 * (2 + 8) / 4"))
print(calc("2 * (3 + (4 * 5))"))
print(calc("((((7))))"))
print(calc("1000 - 8 * 8 * 8 - 488"))
