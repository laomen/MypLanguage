// bench/go/base64.go — Go 版 Base64 编码（字符映射 + 位打包，与 C++/MYP 同算法）
// 8MB 字节流（LCG 高位）→ base64；verify = 输出字节和 + 采样字符×1000003。
package main

import (
	"fmt"
	"time"
)

func base64Bench(n, seed int) int64 {
	in := make([]byte, n)
	rng := int64(seed)
	for i := 0; i < n; i++ {
		rng = (rng*1103515245 + 12345) % 2147483648
		in[i] = byte((rng >> 16) & 0xFF)
	}
	outn := (n / 3) * 4
	out := make([]byte, outn)
	b64 := "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/"
	for i, j := 0, 0; i+2 < n; i, j = i+3, j+4 {
		b0 := int(in[i])
		b1 := int(in[i+1])
		b2 := int(in[i+2])
		out[j] = b64[b0>>2]
		out[j+1] = b64[((b0&3)<<4)|(b1>>4)]
		out[j+2] = b64[((b1&15)<<2)|(b2>>6)]
		out[j+3] = b64[b2&63]
	}
	var sum int64
	for i := 0; i < outn; i++ {
		sum += int64(out[i])
	}
	return sum + int64(out[123456])*1000003
}

func main() {
	const n = 8388606
	t0 := time.Now()
	v := base64Bench(n, 24680)
	ms := time.Since(t0).Milliseconds()
	fmt.Printf("verify %d\nms %d\n", v, ms)
}
