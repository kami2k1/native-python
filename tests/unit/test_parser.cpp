// Parser unit tests (AST S-expression golden checks). Exit code 0 = pass.
#include "../../compiler/src/lexer.h"
#include "../../compiler/src/parser.h"

#include <cstdio>

using namespace kami;

static int failures = 0;

static void check_dump(const char* src, const char* expect, int line) {
    try {
        Module m = parse(lex(src));
        std::string got = dump_module(m);
        if (got != expect) {
            fprintf(stderr, "FAIL line %d\n--- source\n%s\n--- expected\n%s--- got\n%s",
                    line, src, expect, got.c_str());
            failures++;
        }
    } catch (const CompileError& e) {
        fprintf(stderr, "FAIL line %d: unexpected error: %s\n", line, e.what());
        failures++;
    }
}

static void check_error(const char* src, int line) {
    try {
        parse(lex(src));
        fprintf(stderr, "FAIL line %d: expected a parse error for:\n%s\n", line, src);
        failures++;
    } catch (const CompileError&) {
    }
}

#define DUMP(src, expect) check_dump(src, expect, __LINE__)
#define ERR(src) check_error(src, __LINE__)

int main() {
    DUMP("x = 10\n", "(= x 10)\n");
    DUMP("x + 5\n", "(expr (+ x 5))\n");
    // precedence: 1 + 2 * 3 → (+ 1 (* 2 3))
    DUMP("y = 1 + 2 * 3\n", "(= y (+ 1 (* 2 3)))\n");
    DUMP("y = (1 + 2) * 3\n", "(= y (* (+ 1 2) 3))\n");
    DUMP("z = a == b and c < d or not e\n",
         "(= z (or (and (== a b) (< c d)) (not e)))\n");
    DUMP("x = -a + b\n", "(= x (+ (neg a) b))\n");
    DUMP("v = a not in b\n", "(= v (not (in a b)))\n");
    // augmented assignment desugars
    DUMP("x += 2\n", "(= x (+ x 2))\n");
    // function
    DUMP("def add(a, b):\n    return a + b\n",
         "(def add (a b)\n  (return (+ a b))\n)\n");
    // condition + else
    DUMP("if x > 10:\n    print(x)\nelse:\n    pass\n",
         "(if (> x 10)\n  (expr (call print x))\n else\n  (pass)\n)\n");
    // elif desugars to nested if
    DUMP("if a:\n    pass\nelif b:\n    pass\n",
         "(if a\n  (pass)\n else\n  (if b\n    (pass)\n  )\n)\n");
    // loops
    DUMP("while x:\n    x = x - 1\n", "(while x\n  (= x (- x 1))\n)\n");
    DUMP("for i in range(3):\n    print(i)\n",
         "(for i (call range 3)\n  (expr (call print i))\n)\n");
    // collections, index, method calls
    DUMP("a = [1, 2, 3]\n", "(= a (list 1 2 3))\n");
    DUMP("d = {\"k\": 1}\n", "(= d (map (\"k\" 1)))\n");
    DUMP("a[0] = a[1]\n", "(setindex a 0 (index a 1))\n");
    DUMP("xs.append(4)\n", "(expr (method xs .append 4))\n");
    DUMP("import math\nx = math.sqrt(2)\n",
         "(import math)\n(= x (method math .sqrt 2))\n");

    // errors
    ERR("x =\n");
    ERR("if x\n    pass\n");            // missing colon
    ERR("def f(:\n    pass\n");         // bad params
    ERR("return 1\n");                  // return outside function
    ERR("break\n");                     // break outside loop
    ERR("1 + 2 = 3\n");                 // bad assignment target
    ERR("def f():\n    def g():\n        pass\n"); // nested def

    if (failures) {
        fprintf(stderr, "%d parser check(s) failed\n", failures);
        return 1;
    }
    printf("parser: all checks passed\n");
    return 0;
}
