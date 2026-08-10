// bench/go/io_socket.go — Go goroutine I/O 密集（与 MYP @coro await fd 对比）
// 回环 TCP 逐字节 ping-pong N 轮：客户端写 "x"、读回显 "y"；服务器读/回显。
// goroutine + 阻塞 socket（对应 MYP 的 @coro + Coro.waitFd + 非阻塞 socket）。
// verify = 客户端收到的字节数 = N。
package main

import (
	"fmt"
	"net"
	"time"
)

func main() {
	const N = 20000
	ln, err := net.Listen("tcp", "127.0.0.1:0")
	if err != nil {
		panic(err)
	}
	defer ln.Close()

	serverDone := make(chan int64)
	go func() { // 服务器 goroutine：读 "x" → 回显 "y"
		conn, err := ln.Accept()
		if err != nil {
			serverDone <- -1
			return
		}
		defer conn.Close()
		var got int64
		buf := make([]byte, 16)
		for i := int64(0); i < N; i++ {
			n, err := conn.Read(buf)
			if n == 0 || err != nil {
				serverDone <- -1
				return
			}
			if _, err := conn.Write([]byte("y")); err != nil {
				serverDone <- -1
				return
			}
			got++
		}
		serverDone <- got
	}()

	conn, err := net.Dial("tcp", ln.Addr().String())
	if err != nil {
		panic(err)
	}
	defer conn.Close()

	start := time.Now()
	var got int64
	buf := make([]byte, 16)
	for i := int64(0); i < N; i++ { // 客户端：写 "x" → 读回显
		if _, err := conn.Write([]byte("x")); err != nil {
			break
		}
		n, err := conn.Read(buf)
		if n == 0 || err != nil {
			break
		}
		got++
	}
	elapsed := time.Since(start).Milliseconds()
	_ = <-serverDone
	fmt.Printf("verify %d\nms %d\n", got, elapsed)
}
