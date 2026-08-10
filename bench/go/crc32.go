// bench/go/crc32.go — Go 版表驱动 CRC-32（uint32 表 + 字节循环，与 C++/MYP 同算法）
// 32MB 数据；verify = CRC-32 值（转 long 为正数）。
package main

import (
	"fmt"
	"time"
)

var crcTable [256]uint32

func crc32c(crc uint32, data []byte, n int) uint32 {
	for i := 0; i < 256; i++ {
		c := uint32(i)
		for k := 0; k < 8; k++ {
			if c&1 != 0 {
				c = 0xEDB88320 ^ (c >> 1)
			} else {
				c >>= 1
			}
		}
		crcTable[i] = c
	}
	crc ^= 0xFFFFFFFF
	for i := 0; i < n; i++ {
		crc = crcTable[(crc^uint32(data[i]))&0xFF] ^ (crc >> 8)
	}
	return crc ^ 0xFFFFFFFF
}

func crc32(n, seed int) int64 {
	data := make([]byte, n)
	rng := int64(seed)
	for i := 0; i < n; i++ {
		rng = (rng*1103515245 + 12345) % 2147483648
		data[i] = byte((rng >> 16) & 0xFF)
	}
	return int64(crc32c(0, data, n))
}

func main() {
	const n = 33554432
	t0 := time.Now()
	v := crc32(n, 24680)
	ms := time.Since(t0).Milliseconds()
	fmt.Printf("verify %d\nms %d\n", v, ms)
}
