// bench/go/tripleloop.go — Go 版三层嵌套循环（与 bench/myp/tripleloop.myp 同算法）
// verify <累加和>；与 C++/MYP 同算法。
package main

import (
	"fmt"
	"time"
)

func tripleLoop(n int) int64 {
	var sum int64
	for i := 0; i < n; i++ {
		for j := 0; j < n; j++ {
			for k := 0; k < n; k++ {
				sum += int64((i*101 + j*97 + k*89) % 997)
			}
		}
	}
	return sum
}

func main() {
	const n = 300
	t0 := time.Now()
	s := tripleLoop(n)
	ms := time.Since(t0).Milliseconds()
	fmt.Printf("verify %d\nms %d\n", s, ms)
}
