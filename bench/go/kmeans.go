// bench/go/kmeans.go — Go 版 K-means 聚类（浮点距离 + 数据相关分支，与 C++/MYP 同算法）
// N=16384 点 × D=8 维 × K=8 簇 × 400 轮；verify = 最终各点簇号之和。
package main

import (
	"fmt"
	"time"
)

func kmeans(n, d, k, rounds, seed int) int64 {
	pts := make([]float64, n*d)
	rng := int64(seed)
	for i := 0; i < n*d; i++ {
		rng = (rng*1103515245 + 12345) % 2147483648
		pts[i] = float64((rng>>16)%10000) / 100.0
	}
	cen := make([]float64, k*d)
	for i := 0; i < k*d; i++ {
		cen[i] = pts[i]
	}
	assign := make([]int, n)
	sum := make([]float64, k*d)
	cnt := make([]int, k)
	for t := 0; t < rounds; t++ {
		for p := 0; p < n; p++ {
			best := 1e300
			bk := 0
			for c := 0; c < k; c++ {
				s := 0.0
				for q := 0; q < d; q++ {
					df := pts[p*d+q] - cen[c*d+q]
					s += df * df
				}
				if s < best {
					best = s
					bk = c
				}
			}
			assign[p] = bk
		}
		for c := 0; c < k*d; c++ {
			sum[c] = 0.0
		}
		for c := 0; c < k; c++ {
			cnt[c] = 0
		}
		for p := 0; p < n; p++ {
			c := assign[p]
			cnt[c]++
			for q := 0; q < d; q++ {
				sum[c*d+q] += pts[p*d+q]
			}
		}
		for c := 0; c < k; c++ {
			if cnt[c] > 0 {
				for q := 0; q < d; q++ {
					cen[c*d+q] = sum[c*d+q] / float64(cnt[c])
				}
			}
		}
	}
	var v int64
	for p := 0; p < n; p++ {
		v += int64(assign[p])
	}
	return v
}

func main() {
	const n, d, k, rounds = 16384, 8, 8, 400
	t0 := time.Now()
	v := kmeans(n, d, k, rounds, 24680)
	ms := time.Since(t0).Milliseconds()
	fmt.Printf("verify %d\nms %d\n", v, ms)
}
