// Go reference implementation of benchmarks/bench.py (same algorithms).
package main

import (
	"fmt"
	"strings"
	"time"
)

func integerLoop(n int64) int64 {
	var total int64 = 0
	for i := int64(0); i < n; i++ {
		total = (total + i*3) % 1000000007
	}
	return total
}

func mathTest(n int64) float64 {
	acc := 0.0
	for i := int64(0); i < n; i++ {
		acc = acc + float64(i)*0.5 - acc/3.0
	}
	return acc
}

func fib(n int64) int64 {
	if n < 2 {
		return n
	}
	return fib(n-1) + fib(n-2)
}

// Idiomatic Go: += on strings is O(n^2), so Go programs use strings.Builder.
// (KamiPython's escape analysis gives plain `s += "x"` the same behavior.)
func stringTest(n int64) int {
	var b strings.Builder
	for i := int64(0); i < n; i++ {
		b.WriteString("x")
	}
	return len(b.String())
}

func bench(name string, f func() interface{}) {
	t0 := time.Now()
	r := f()
	dt := time.Since(t0).Seconds()
	fmt.Printf("%s,%v,%v\n", name, dt, r)
}

func main() {
	bench("integer_loop", func() interface{} { return integerLoop(100000000) })
	bench("math", func() interface{} { return mathTest(100000000) })
	bench("recursion", func() interface{} { return fib(32) })
	bench("string_test", func() interface{} { return stringTest(1000000) })
}
