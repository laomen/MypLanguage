// bench/go/radixsort.go — Go 版 LSD 基数排序（8-bit 4 趟，与 C++/MYP 同算法）
// N=4*10^6 随机整数；verify = 排序后校验和 sum(a[i]*(i+1))。
package main

import (
	"fmt"
	"time"
)

func radixsort(n, seed int) int64 {
	a := make([]int, n)
	b := make([]int, n)
	rng := int64(seed)
	for i := 0; i < n; i++ {
		rng = (rng*1103515245 + 12345) % 2147483648
		a[i] = int(rng & 0x7FFFFFFF)
	}
	var cnt [256]int
	for shift := 0; shift < 32; shift += 8 {
		for i := 0; i < 256; i++ {
			cnt[i] = 0
		}
		for i := 0; i < n; i++ {
			cnt[(a[i]>>shift)&0xFF]++
		}
		s := 0
		for i := 0; i < 256; i++ {
			t := cnt[i]
			cnt[i] = s
			s += t
		}
		for i := 0; i < n; i++ {
			key := (a[i] >> shift) & 0xFF
			b[cnt[key]] = a[i]
			cnt[key]++
		}
		a, b = b, a
	}
	var sum int64
	for i := 0; i < n; i++ {
		sum += int64(a[i]) * int64(i+1)
	}
	return sum
}

func main() {
	const n = 4000000
	t0 := time.Now()
	v := radixsort(n, 24680)
	ms := time.Since(t0).Milliseconds()
	fmt.Printf("verify %d\nms %d\n", v, ms)
}
