// bench/go/sobel.go — Go 版 Sobel 边缘检测（3×3 梯度，uint8 灰度图，与 C++/MYP 同算法）
// N=2048 图像；verify = 所有像素梯度幅值之和（|gx|+|gy|，clamp 255）。
package main

import (
	"fmt"
	"time"
)

func sobel(n, seed int) int {
	img := make([]byte, n*n)
	rng := int64(seed)
	for i := 0; i < n*n; i++ {
		rng = (rng*1103515245 + 12345) % 2147483648
		img[i] = byte((rng >> 16) & 0xFF)
	}
	var sum int64
	for y := 1; y < n-1; y++ {
		for x := 1; x < n-1; x++ {
			gx := -int(img[(y-1)*n+x-1]) - 2*int(img[y*n+x-1]) - int(img[(y+1)*n+x-1]) +
				int(img[(y-1)*n+x+1]) + 2*int(img[y*n+x+1]) + int(img[(y+1)*n+x+1])
			gy := -int(img[(y-1)*n+x-1]) - 2*int(img[(y-1)*n+x]) - int(img[(y-1)*n+x+1]) +
				int(img[(y+1)*n+x-1]) + 2*int(img[(y+1)*n+x]) + int(img[(y+1)*n+x+1])
			m := 0
			if gx < 0 {
				m += -gx
			} else {
				m += gx
			}
			if gy < 0 {
				m += -gy
			} else {
				m += gy
			}
			if m > 255 {
				m = 255
			}
			sum += int64(m)
		}
	}
	return int(sum)
}

func main() {
	const n = 2048
	t0 := time.Now()
	v := sobel(n, 24680)
	ms := time.Since(t0).Milliseconds()
	fmt.Printf("verify %d\nms %d\n", v, ms)
}
