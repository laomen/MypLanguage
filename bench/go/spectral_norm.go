package main

import (
	"fmt"
	"math"
	"time"
)

func spectral(n int) float64 {
	u := make([]float64, n)
	v := make([]float64, n)
	for i := 0; i < n; i++ {
		u[i] = 1.0
	}
	for it := 0; it < 10; it++ {
		for j := 0; j < n; j++ {
			s := 0.0
			for i := 0; i < n; i++ {
				s += 1.0 / (float64((i+j)*(i+j+1))/2.0 + float64(j) + 1.0) * u[i]
			}
			v[j] = s
		}
		for j := 0; j < n; j++ {
			s := 0.0
			for i := 0; i < n; i++ {
				s += 1.0 / (float64((j+i)*(j+i+1))/2.0 + float64(i) + 1.0) * v[i]
			}
			u[j] = s
		}
	}
	vBv, vv := 0.0, 0.0
	for i := 0; i < n; i++ {
		vBv += u[i] * v[i]
		vv += v[i] * v[i]
	}
	return math.Sqrt(vBv / vv)
}

func main() {
	t0 := time.Now()
	v := spectral(5500)
	fmt.Printf("verify %.6f\nms %d\n", v, int(time.Since(t0).Milliseconds()))
}
