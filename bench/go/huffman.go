// bench/go/huffman.go — Go 版 Huffman 编码（字节计数 + 线性扫描最小池建树 + 码长）
// 8MB 字节流（LCG）；verify = 总码长 ∑cnt[i]×depth[i] + 符号数×10^9。
package main

import (
	"fmt"
	"time"
)

func huffman(n, seed int) int64 {
	msg := make([]byte, n)
	rng := int64(seed)
	for i := 0; i < n; i++ {
		rng = (rng*1103515245 + 12345) % 2147483648
		msg[i] = byte((rng >> 16) & 0xFF)
	}
	var cnt [256]int
	for i := 0; i < n; i++ {
		cnt[msg[i]]++
	}
	const MAX = 512
	freq := make([]int, MAX)
	left := make([]int, MAX)
	right := make([]int, MAX)
	nid := 0
	for i := 0; i < 256; i++ {
		if cnt[i] > 0 {
			freq[nid] = cnt[i]
			left[nid] = -1
			right[nid] = -1
			nid++
		}
	}
	pool := make([]int, 0, nid)
	for i := 0; i < nid; i++ {
		pool = append(pool, i)
	}
	popMin := func() int {
		bi := 0
		for i := 1; i < len(pool); i++ {
			if freq[pool[i]] < freq[pool[bi]] ||
				(freq[pool[i]] == freq[pool[bi]] && pool[i] < pool[bi]) {
				bi = i
			}
		}
		v := pool[bi]
		pool[bi] = pool[len(pool)-1]
		pool = pool[:len(pool)-1]
		return v
	}
	for len(pool) > 1 {
		a := popMin()
		b := popMin()
		freq[nid] = freq[a] + freq[b]
		left[nid] = a
		right[nid] = b
		pool = append(pool, nid)
		nid++
	}
	root := -1
	if len(pool) > 0 {
		root = pool[0]
	}
	var depth [512]int
	stack := make([]int, 0, 512)
	stackDepth := make([]int, 0, 512)
	if root >= 0 {
		stack = append(stack, root)
		stackDepth = append(stackDepth, 0)
	}
	var sum int64
	nsym := 0
	for len(stack) > 0 {
		sp := len(stack) - 1
		node := stack[sp]
		dep := stackDepth[sp]
		stack = stack[:sp]
		stackDepth = stackDepth[:sp]
		if left[node] < 0 && right[node] < 0 {
			depth[node] = dep
			if node < 256 {
				sum += int64(cnt[node]) * int64(dep)
				nsym++
			}
		} else {
			stack = append(stack, left[node])
			stackDepth = append(stackDepth, dep+1)
			stack = append(stack, right[node])
			stackDepth = append(stackDepth, dep+1)
		}
	}
	return sum + int64(nsym)*1000000000
}

func main() {
	const n = 8388608
	t0 := time.Now()
	v := huffman(n, 98765)
	ms := time.Since(t0).Milliseconds()
	fmt.Printf("verify %d\nms %d\n", v, ms)
}
