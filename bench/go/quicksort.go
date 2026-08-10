// bench/go/quicksort.go — Go 版快速排序（Lomuto 分区，递归，与 C++/MYP 同算法）
// verify = 排序后 a[n/2] + 总和；与 C++/MYP 同 LCG。
package main

import (
	"fmt"
	"time"
)

func qs(a []int, lo, hi int) {
	if lo >= hi {
		return
	}
	pivot := a[hi]
	i := lo
	for j := lo; j < hi; j++ {
		if a[j] <= pivot {
			a[i], a[j] = a[j], a[i]
			i++
		}
	}
	a[i], a[hi] = a[hi], a[i]
	qs(a, lo, i-1)
	qs(a, i+1, hi)
}

func quicksort(n, seed int) int64 {
	a := make([]int, n)
	rng := int64(seed)
	for i := 0; i < n; i++ {
		rng = (rng*1103515245 + 12345) % 2147483648
		a[i] = int(rng)
	}
	qs(a, 0, n-1)
	var sum int64
	for i := 0; i < n; i++ {
		sum += int64(a[i])
	}
	return sum + int64(a[n/2])
}

func main() {
	const n = 800000
	t0 := time.Now()
	v := quicksort(n, 24680)
	ms := time.Since(t0).Milliseconds()
	fmt.Printf("verify %d\nms %d\n", v, ms)
}
