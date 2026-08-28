// bench/go/coro_spawn.go — goroutine spawn 开销（与 MYP @coro 对比）
// K=20000 goroutine 全部启动并挂起后统一释放，对齐 MYP 首次启动到 await、
// 再逐个 resume 完成的两阶段生命周期。
package main

import (
	"fmt"
	"time"
)

func main() {
	const K = 20000
	done := make(chan struct{}, K)
	ready := make(chan struct{}, K)
	startAll := make(chan struct{})
	start := time.Now()
	for i := 0; i < K; i++ {
		go func(x int) {
			_ = x * 2
			ready <- struct{}{}
			<-startAll
			done <- struct{}{}
		}(i)
	}
	for i := 0; i < K; i++ {
		<-ready
	}
	close(startAll)
	for i := 0; i < K; i++ {
		<-done
	}
	ms := time.Since(start).Milliseconds()
	fmt.Printf("verify %d\nms %d\n", K, ms)
}
