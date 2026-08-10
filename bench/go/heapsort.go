// bench/go/heapsort.go — Go 版二叉堆排序（堆化 + sift-down，与 C++/MYP 同算法）
// N=10^6 随机整数；verify = 排序后校验和 sum(a[i]*(i+1))。
package main

import (
	"fmt"
	"time"
)

func siftDown(a []int, start, end int) {
	root := start
	for 2*root+1 <= end {
		child := 2*root + 1
		swap := root
		if a[swap] < a[child] {
			swap = child
		}
		if child+1 <= end && a[swap] < a[child+1] {
			swap = child + 1
		}
		if swap == root {
			return
		}
		a[root], a[swap] = a[swap], a[root]
		root = swap
	}
}

func heapsort(n, seed int) int64 {
	a := make([]int, n)
	rng := int64(seed)
	for i := 0; i < n; i++ {
		rng = (rng*1103515245 + 12345) % 2147483648
		a[i] = int(rng & 0x7FFFFFFF)
	}
	for i := n/2 - 1; i >= 0; i-- {
		siftDown(a, i, n-1)
	}
	for end := n - 1; end > 0; end-- {
		a[0], a[end] = a[end], a[0]
		siftDown(a, 0, end-1)
	}
	var sum int64
	for i := 0; i < n; i++ {
		sum += int64(a[i]) * int64(i+1)
	}
	return sum
}

func main() {
	const n = 1000000
	t0 := time.Now()
	v := heapsort(n, 24680)
	ms := time.Since(t0).Milliseconds()
	fmt.Printf("verify %d\nms %d\n", v, ms)
}
