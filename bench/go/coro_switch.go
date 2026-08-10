// bench/go/coro_switch.go — goroutine 上下文切换吞吐（与 MYP @coro 对比）
// K=200 goroutine × M=10000 次 runtime.Gosched() = 200 万次调度切换。
package main

import (
	"fmt"
	"runtime"
	"time"
)

func main() {
	const K = 200
	const M = 10000
	done := make(chan struct{}, K)
	start := time.Now()
	for i := 0; i < K; i++ {
		go func() {
			for j := 0; j < M; j++ {
				runtime.Gosched()
			}
			done <- struct{}{}
		}()
	}
	for i := 0; i < K; i++ {
		<-done
	}
	ms := time.Since(start).Milliseconds()
	fmt.Printf("verify %d\nms %d\n", K*M, ms)
}
