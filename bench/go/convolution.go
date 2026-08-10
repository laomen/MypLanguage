// bench/go/convolution.go — Go 版 2D 图像卷积（5×5 核，滑窗，与 C++/MYP 同算法）
// N=2048 图像；verify = 输出图像元素之和（double，同序求和）。
package main

import (
	"fmt"
	"time"
)

func convolution(n, k, seed int) float64 {
	img := make([]float64, n*n)
	out := make([]float64, n*n)
	ker := make([]float64, k*k)
	rng := int64(seed)
	for i := 0; i < n*n; i++ {
		rng = (rng*1103515245 + 12345) % 2147483648
		img[i] = float64((rng>>16)%1000) / 100.0
	}
	for i := 0; i < k*k; i++ {
		rng = (rng*1103515245 + 12345) % 2147483648
		ker[i] = float64((rng>>16)%100) / 100.0
	}
	hk := k / 2
	for y := hk; y+hk < n; y++ {
		for x := hk; x+hk < n; x++ {
			s := 0.0
			for ky := 0; ky < k; ky++ {
				for kx := 0; kx < k; kx++ {
					s += img[(y+ky-hk)*n+(x+kx-hk)] * ker[ky*k+kx]
				}
			}
			out[y*n+x] = s
		}
	}
	sum := 0.0
	for i := 0; i < n*n; i++ {
		sum += out[i]
	}
	return sum
}

func main() {
	const n, k = 2048, 5
	t0 := time.Now()
	v := convolution(n, k, 24680)
	ms := time.Since(t0).Milliseconds()
	fmt.Printf("verify %.6f\nms %d\n", v, ms)
}
