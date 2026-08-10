// bench/go/nbody.go — Go 版 N-body（与 bench/myp/nbody.myp 同算法）
// verify <动能>（浮点，容差对比）；与 C++/MYP 同算法。
package main

import (
	"fmt"
	"math"
	"time"
)

func nbodyKE(n, steps int) float64 {
	px := make([]float64, n)
	py := make([]float64, n)
	pz := make([]float64, n)
	vx := make([]float64, n)
	vy := make([]float64, n)
	vz := make([]float64, n)
	m := make([]float64, n)
	for i := 0; i < n; i++ {
		px[i] = float64((i * 37) % 1000)
		py[i] = float64((i * 53) % 1000)
		pz[i] = float64((i * 71) % 1000)
		m[i] = 1.0 + float64(i%10)*0.1
	}
	for step := 0; step < steps; step++ {
		for a := 0; a < n; a++ {
			ax, ay, az := 0.0, 0.0, 0.0
			for b := 0; b < n; b++ {
				if b == a {
					continue
				}
				dx := px[b] - px[a]
				dy := py[b] - py[a]
				dz := pz[b] - pz[a]
				d2 := dx*dx + dy*dy + dz*dz + 1e-6
				inv := 1.0 / (d2 * math.Sqrt(d2))
				mm := m[b] * inv
				ax += dx * mm
				ay += dy * mm
				az += dz * mm
			}
			vx[a] += ax
			vy[a] += ay
			vz[a] += az
		}
		for i := 0; i < n; i++ {
			px[i] += vx[i]
			py[i] += vy[i]
			pz[i] += vz[i]
		}
	}
	ke := 0.0
	for i := 0; i < n; i++ {
		ke += 0.5 * m[i] * (vx[i]*vx[i] + vy[i]*vy[i] + vz[i]*vz[i])
	}
	return ke
}

func main() {
	const n, steps = 5000, 2
	t0 := time.Now()
	ke := nbodyKE(n, steps)
	ms := time.Since(t0).Milliseconds()
	fmt.Printf("verify %.10g\nms %d\n", ke, ms)
}
