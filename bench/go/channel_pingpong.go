// bench/go/channel_pingpong.go — Go channel ping-pong（与 MYP Channel 对比）
// 有缓冲 channel(capacity=1)：生产者 goroutine send 0..N-1，消费者 goroutine
// 累加。容量 1 时每次 send 满 / recv 空都会阻塞换出，等价 MYP 的协程 park。
// verify = Σ(0..N-1) = N*(N-1)/2。
package main

import (
	"fmt"
	"time"
)

func main() {
	const N int64 = 100000
	ch := make(chan int64, 1)
	done := make(chan struct{})
	var sum int64
	go func() { // 消费者
		var s int64
		for i := int64(0); i < N; i++ {
			s += <-ch
		}
		sum = s
		done <- struct{}{}
	}()
	start := time.Now()
	for i := int64(0); i < N; i++ { // 生产者（主 goroutine）
		ch <- i
	}
	<-done
	ms := time.Since(start).Milliseconds()
	fmt.Printf("verify %d\nms %d\n", sum, ms)
}
