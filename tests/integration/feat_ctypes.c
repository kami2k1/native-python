// Compiled and linked into feat_ctypes by the same clang invocation as the
// Python program (see feat_ctypes.args), so the ctypes calls resolve directly.
#include <stdio.h>

int kami_demo_add(int a, int b) { return a + b; }
double kami_demo_hypot2(double a, double b) { return a * a + b * b; }
long kami_demo_len(const char* s) {
    const char* p = s;
    while (*p) p++;
    return p - s;
}
const char* kami_demo_name(void) { return "libdemo"; }
void kami_demo_say(const char* who) { printf("C says hi to %s\n", who); }
