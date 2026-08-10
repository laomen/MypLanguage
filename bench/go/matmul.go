// bench/go/matmul.go — Go 版矩阵乘法（分块，与 bench/myp/matmul.myp 同算法）
// verify <迹>（浮点，容差对比）；与 C++/MYP 同算法。
package main

import (
	"fmt"
	"time"
)

func matmulTrace(n int) float64 {
	A := make([]float64, n*n)
	B := make([]float64, n*n)
	C := make([]float64, n*n)
	for i := 0; i < n*n; i++ {
		A[i] = float64(i % 1000)
		B[i] = float64(i % 7)
	}
	const BS = 64
	for i0 := 0; i0 < n; i0 += BS {
		for j0 := 0; j0 < n; j0 += BS {
			for k0 := 0; k0 < n; k0 += BS {
				for i1 := i0; i1 < i0+BS && i1 < n; i1++ {
					for k1 := k0; k1 < k0+BS && k1 < n; k1++ {
						av := A[i1*n+k1]
						for j1 := j0; j1 < j0+BS && j1 < n; j1++ {
							C[i1*n+j1] += av * B[k1*n+j1]
						}
					}
				}
			}
		}
	}
	trace := 0.0
	for i := 0; i < n; i++ {
		trace += C[i*n+i]
	}
	return trace
}

func main() {
	const n = 512
	t0 := time.Now()
	tr := matmulTrace(n)
	ms := time.Since(t0).Milliseconds()
	fmt.Printf("verify %.10g\nms %d\n", tr, ms)
}
