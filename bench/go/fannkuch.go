package main

import (
	"fmt"
	"time"
)

func fannkuch(n int) int {
	perm := make([]int, n)
	perm1 := make([]int, n)
	count := make([]int, n)
	for i := 0; i < n; i++ {
		perm1[i] = i
	}
	maxflips, r := 0, n
	for {
		for r != 1 {
			count[r-1] = r
			r--
		}
		for i := 0; i < n; i++ {
			perm[i] = perm1[i]
		}
		flips := 0
		for perm[0] != 0 {
			k := perm[0]
			for i, j := 0, k; i < j; i, j = i+1, j-1 {
				perm[i], perm[j] = perm[j], perm[i]
			}
			flips++
		}
		if flips > maxflips {
			maxflips = flips
		}
		for {
			if r == n {
				return maxflips
			}
			perm0 := perm1[0]
			for i := 0; i < r; i++ {
				perm1[i] = perm1[i+1]
			}
			perm1[r] = perm0
			count[r]--
			if count[r] > 0 {
				break
			}
			r++
		}
	}
}

func main() {
	t0 := time.Now()
	v := fannkuch(11)
	fmt.Printf("verify %d\nms %d\n", v, int(time.Since(t0).Milliseconds()))
}
