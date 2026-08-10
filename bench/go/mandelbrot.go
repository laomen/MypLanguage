// bench/go/mandelbrot.go — Go 版 Mandelbrot（与 bench/myp/mandelbrot.myp 同算法）
// verify <总迭代>；与 C++/MYP 同算法。
package main

import (
	"fmt"
	"time"
)

func mandelbrot(width, height, maxIter int) int64 {
	var total int64
	for y := 0; y < height; y++ {
		for x := 0; x < width; x++ {
			cr := float64(x)/float64(width)*3.5 - 2.5
			ci := float64(y)/float64(height)*2.0 - 1.0
			zr, zi := 0.0, 0.0
			iter := 0
			for iter < maxIter {
				zr2 := zr * zr
				zi2 := zi * zi
				if zr2+zi2 > 4.0 {
					break
				}
				zi = 2.0*zr*zi + ci
				zr = zr2 - zi2 + cr
				iter++
			}
			total += int64(iter)
		}
	}
	return total
}

func main() {
	const w, h, mi = 1000, 1000, 256
	t0 := time.Now()
	t := mandelbrot(w, h, mi)
	ms := time.Since(t0).Milliseconds()
	fmt.Printf("verify %d\nms %d\n", t, ms)
}
