// bench/go/fft.go — Go 版迭代 radix-2 FFT（与 bench/myp/fft.myp 同算法）
// verify <频谱能量>（浮点，容差对比）；与 C++/MYP 同算法。
package main

import (
	"fmt"
	"math"
	"time"
)

const pi = 3.141592653589793

func fft(n, iters int) float64 {
	re := make([]float64, n)
	im := make([]float64, n)
	for it := 0; it < iters; it++ {
		for i := 0; i < n; i++ {
			re[i] = math.Sin(float64(i%100) * 0.1234567)
			im[i] = math.Cos(float64(i%100) * 0.07654321)
		}
		j := 0
		for i := 1; i < n; i++ {
			bit := n >> 1
			for (j & bit) != 0 {
				j ^= bit
				bit >>= 1
			}
			j ^= bit
			if i < j {
				re[i], re[j] = re[j], re[i]
				im[i], im[j] = im[j], im[i]
			}
		}
		for length := 2; length <= n; length <<= 1 {
			ang := -2.0 * pi / float64(length)
			wRe := math.Cos(ang)
			wIm := math.Sin(ang)
			for k := 0; k < n; k += length {
				curRe, curIm := 1.0, 0.0
				for k2 := 0; k2 < length/2; k2++ {
					a := k + k2
					b := k + k2 + length/2
					tre := re[b]
					tim := im[b]
					pRe := tre*curRe - tim*curIm
					pIm := tre*curIm + tim*curRe
					re[b] = re[a] - pRe
					im[b] = im[a] - pIm
					re[a] += pRe
					im[a] += pIm
					ncRe := curRe*wRe - curIm*wIm
					ncIm := curRe*wIm + curIm*wRe
					curRe = ncRe
					curIm = ncIm
				}
			}
		}
	}
	e := 0.0
	for i := 0; i < n; i++ {
		e += re[i]*re[i] + im[i]*im[i]
	}
	return e
}

func main() {
	const n, iters = 4096, 800
	t0 := time.Now()
	e := fft(n, iters)
	ms := time.Since(t0).Milliseconds()
	fmt.Printf("verify %.10g\nms %d\n", e, ms)
}
