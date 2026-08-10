// bench/go/coro_spawn.go — goroutine spawn 开销（与 MYP @coro 对比）
// K=20000 goroutine 各执行一次后退出。
package main

import (
	"fmt"
	"time"
)

func main() {
	const K = 20000
	done := make(chan struct{}, K)
	start := time.Now()
	for i := 0; i < K; i++ {
		go func(x int) {
			_ = x * 2
			done <- struct{}{}
		}(i)
	}
	for i := 0; i < K; i++ {
		<-done
	}
	ms := time.Since(start).Milliseconds()
	fmt.Printf("verify %d\nms %d\n", K, ms)
}
