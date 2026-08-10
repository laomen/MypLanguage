// bench/go/sieve.go — Go 版埃氏筛（与 bench/myp/sieve.myp 同算法同规模）
// verify <素数个数>；与 C++/MYP 同算法。
package main

import (
	"fmt"
	"time"
)

func countPrimesSieve(n int) int {
	comp := make([]bool, n+1)
	count := 0
	for p := 2; p <= n; p++ {
		if !comp[p] {
			count++
			// int64 计算 p*p（p 较大时 int 乘法溢出，与 C++/MYP 一致）
			for j := int64(p) * int64(p); j <= int64(n); j += int64(p) {
				comp[int(j)] = true
			}
		}
	}
	return count
}

func main() {
	const N = 10000000
	t0 := time.Now()
	c := countPrimesSieve(N)
	ms := time.Since(t0).Milliseconds()
	fmt.Printf("verify %d\nms %d\n", c, ms)
}
