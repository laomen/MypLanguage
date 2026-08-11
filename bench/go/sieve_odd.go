// bench/go/sieve_odd.go — Go 版只筛奇数埃氏筛（与 bench/myp/sieve_odd.myp 同算法同规模）
// verify <素数个数>；与 C++/MYP 同算法。
package main

import (
	"fmt"
	"time"
)

func countPrimesOdd(n int) int {
	// idx = (k-3)/2 ↔ k = 2*idx+3（只表示奇数 3,5,7,...）
	comp := make([]bool, (n-3)/2+1)
	count := 1 // 2
	for p := 3; p <= n; p += 2 {
		idx := (p - 3) / 2
		if !comp[idx] {
			count++
			// 只标记奇数倍：从 p*p 起步长 2p（int64 防 p*p 溢出，与 C++/MYP 一致）
			for j := int64(p) * int64(p); j <= int64(n); j += 2 * int64(p) {
				comp[(int(j)-3)/2] = true
			}
		}
	}
	return count
}

func main() {
	const N = 10000000
	t0 := time.Now()
	c := countPrimesOdd(N)
	ms := time.Since(t0).Milliseconds()
	fmt.Printf("verify %d\nms %d\n", c, ms)
}
