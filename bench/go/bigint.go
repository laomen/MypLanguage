// bench/go/bigint.go — Go 版大数乘法（schoolbook，uint16 字 + 64 位进位链，与 C++/MYP 同算法）
// 512 字（8192 位）× 500 次（每次 LCG 重新生成操作数）；verify = 结果字滚动校验和。
package main

import (
	"fmt"
	"time"
)

func bigint(n, iters, seed int) int64 {
	A := make([]uint16, n)
	B := make([]uint16, n)
	C := make([]uint16, 2*n)
	rng := int64(seed)
	var checksum int64
	for it := 0; it < iters; it++ {
		for i := 0; i < n; i++ {
			rng = (rng*1103515245 + 12345) % 2147483648
			A[i] = uint16(rng >> 16)
			rng = (rng*1103515245 + 12345) % 2147483648
			B[i] = uint16(rng >> 16)
		}
		for i := 0; i < 2*n; i++ {
			C[i] = 0
		}
		for i := 0; i < n; i++ {
			var carry uint64
			av := uint64(A[i])
			for j := 0; j < n; j++ {
				cur := uint64(C[i+j]) + av*uint64(B[j]) + carry
				C[i+j] = uint16(cur & 0xFFFF)
				carry = cur >> 16
			}
			pos := i + n
			for carry != 0 && pos < 2*n {
				s := uint64(C[pos]) + carry
				C[pos] = uint16(s & 0xFFFF)
				carry = s >> 16
				pos++
			}
		}
		for i := 0; i < 2*n; i++ {
			checksum += int64(C[i]) * int64(i+1)
		}
	}
	return checksum
}

func main() {
	const n, iters = 512, 500
	t0 := time.Now()
	v := bigint(n, iters, 13579)
	ms := time.Since(t0).Milliseconds()
	fmt.Printf("verify %d\nms %d\n", v, ms)
}
