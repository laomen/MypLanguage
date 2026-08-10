// bench/go/floyd.go — Go 版 Floyd-Warshall 全源最短路（稠密三层循环，与 C++/MYP 同算法）
// V=600 邻接矩阵 1..1000 边权、对角线 0；verify = 全部 dist 之和。
package main

import (
	"fmt"
	"time"
)

func floyd(n, seed int) int64 {
	d := make([]int, n*n)
	rng := int64(seed)
	for i := 0; i < n*n; i++ {
		rng = (rng*1103515245 + 12345) % 2147483648
		d[i] = int((rng>>16)%1000) + 1
	}
	for i := 0; i < n; i++ {
		d[i*n+i] = 0
	}
	for k := 0; k < n; k++ {
		for i := 0; i < n; i++ {
			dk := d[i*n+k]
			base := i * n
			for j := 0; j < n; j++ {
				nd := dk + d[k*n+j]
				if nd < d[base+j] {
					d[base+j] = nd
				}
			}
		}
	}
	var sum int64
	for i := 0; i < n*n; i++ {
		sum += int64(d[i])
	}
	return sum
}

func main() {
	const n = 600
	t0 := time.Now()
	v := floyd(n, 24680)
	ms := time.Since(t0).Milliseconds()
	fmt.Printf("verify %d\nms %d\n", v, ms)
}
