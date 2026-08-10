// bench/go/kmp.go — Go 版 KMP 字符串匹配（失配表 + 单趟扫描，与 C++/MYP 同算法）
// 32MB 文本 × 256 字节模式（均 LCG）；verify = 匹配次数。
package main

import (
	"fmt"
	"time"
)

func kmp(n, m, seed int) int64 {
	text := make([]byte, n)
	pat := make([]byte, m)
	rng := int64(seed)
	for i := 0; i < n; i++ {
		rng = (rng*1103515245 + 12345) % 2147483648
		text[i] = byte((rng >> 16) & 0xFF)
	}
	for i := 0; i < m; i++ {
		rng = (rng*1103515245 + 12345) % 2147483648
		pat[i] = byte((rng >> 16) & 0xFF)
	}
	fail := make([]int, m)
	fail[0] = 0
	j := 0
	for i := 1; i < m; i++ {
		for j > 0 && pat[i] != pat[j] {
			j = fail[j-1]
		}
		if pat[i] == pat[j] {
			j++
		}
		fail[i] = j
	}
	var count int64
	j = 0
	for i := 0; i < n; i++ {
		for j > 0 && text[i] != pat[j] {
			j = fail[j-1]
		}
		if text[i] == pat[j] {
			j++
		}
		if j == m {
			count++
			j = fail[j-1]
		}
	}
	return count
}

func main() {
	const n, m = 33554432, 256
	t0 := time.Now()
	v := kmp(n, m, 98765)
	ms := time.Since(t0).Milliseconds()
	fmt.Printf("verify %d\nms %d\n", v, ms)
}
