// bench/go/spmv.go — Go 版稀疏矩阵×稠密向量（CSR 风格 gather，与 C++/MYP 同算法）
// N=65536 行 × K=64 非零/行；verify = 结果向量 y 之和（double）。
package main

import (
	"fmt"
	"time"
)

func spmv(n, k, seed int) float64 {
	col := make([]int, n*k)
	val := make([]float64, n*k)
	x := make([]float64, n)
	rng := int64(seed)
	for i := 0; i < n*k; i++ {
		rng = (rng*1103515245 + 12345) % 2147483648
		col[i] = int((rng >> 16) % int64(n))
		rng = (rng*1103515245 + 12345) % 2147483648
		val[i] = float64((rng>>16)%1000) / 100.0
	}
	for i := 0; i < n; i++ {
		rng = (rng*1103515245 + 12345) % 2147483648
		x[i] = float64((rng>>16)%1000) / 100.0
	}
	y := make([]float64, n)
	for i := 0; i < n; i++ {
		s := 0.0
		for j := 0; j < k; j++ {
			s += val[i*k+j] * x[col[i*k+j]]
		}
		y[i] = s
	}
	sum := 0.0
	for i := 0; i < n; i++ {
		sum += y[i]
	}
	return sum
}

func main() {
	const n, k = 65536, 64
	t0 := time.Now()
	v := spmv(n, k, 13579)
	ms := time.Since(t0).Milliseconds()
	fmt.Printf("verify %.6f\nms %d\n", v, ms)
}
