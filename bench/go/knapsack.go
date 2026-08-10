// bench/go/knapsack.go — Go 版 0/1 背包（1D 滚动数组，与 C++/MYP 同算法）
// N=10000 物品、容量 10000；verify = dp[容量]。
package main

import (
	"fmt"
	"time"
)

func knapsack(n, cap, seed int) int64 {
	wt := make([]int, n)
	val := make([]int, n)
	rng := int64(seed)
	for i := 0; i < n; i++ {
		rng = (rng*1103515245 + 12345) % 2147483648
		wt[i] = int((rng>>16)%20) + 1
		rng = (rng*1103515245 + 12345) % 2147483648
		val[i] = int((rng>>16)%1000) + 1
	}
	dp := make([]int, cap+1)
	for i := 0; i < n; i++ {
		for w := cap; w >= wt[i]; w-- {
			cand := dp[w-wt[i]] + val[i]
			if cand > dp[w] {
				dp[w] = cand
			}
		}
	}
	return int64(dp[cap])
}

func main() {
	const n, cap = 10000, 10000
	t0 := time.Now()
	v := knapsack(n, cap, 13579)
	ms := time.Since(t0).Milliseconds()
	fmt.Printf("verify %d\nms %d\n", v, ms)
}
