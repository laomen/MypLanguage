// bench/go/montepi.go — Go 版蒙特卡洛求 π（与 bench/myp/montepi.myp 同 LCG 同规模）
// verify <落圆内点数>；与 C++/MYP 随机序列逐位一致。
package main

import (
	"fmt"
	"time"
)

func mcCount(n int, seed int64) int {
	rng := seed
	inside := 0
	for i := 0; i < n; i++ {
		rng = (rng*1103515245 + 12345) & 0x7fffffff
		x := float64(rng>>7) / 16777216.0
		rng = (rng*1103515245 + 12345) & 0x7fffffff
		y := float64(rng>>7) / 16777216.0
		if x*x+y*y <= 1.0 {
			inside++
		}
	}
	return inside
}

func main() {
	const N = 100000000
	t0 := time.Now()
	c := mcCount(N, 24680)
	ms := time.Since(t0).Milliseconds()
	fmt.Printf("verify %d\nms %d\n", c, ms)
}
