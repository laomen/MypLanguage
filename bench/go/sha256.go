// bench/go/sha256.go — Go 版 SHA-256（与 bench/myp/sha256.myp 同算法同填充）
// 消息 64KB（LCG 高位填充），verify = 哈希前 64 位（有符号 long）。
package main

import (
	"fmt"
	"time"
)

func rotr(x uint32, n uint) uint32 { return (x >> n) | (x << (32 - n)) }

func sha256(msglen, seed int) int64 {
	msg := make([]byte, msglen)
	rng := int64(seed)
	for i := 0; i < msglen; i++ {
		rng = (rng*1103515245 + 12345) % 2147483648
		msg[i] = byte((rng >> 16) & 0xFF)
	}
	total := (msglen + 72) / 64 * 64
	if total < msglen+9 {
		total += 64
	}
	data := make([]byte, total)
	for i := 0; i < msglen; i++ {
		data[i] = msg[i]
	}
	data[msglen] = 0x80
	bitlen := uint64(msglen) * 8
	for i := 0; i < 8; i++ {
		data[total-1-i] = byte((bitlen >> (8 * i)) & 0xFF)
	}
	K := [64]uint32{
		0x428a2f98, 0x71374491, 0xb5c0fbcf, 0xe9b5dba5, 0x3956c25b, 0x59f111f1, 0x923f82a4, 0xab1c5ed5,
		0xd807aa98, 0x12835b01, 0x243185be, 0x550c7dc3, 0x72be5d74, 0x80deb1fe, 0x9bdc06a7, 0xc19bf174,
		0xe49b69c1, 0xefbe4786, 0x0fc19dc6, 0x240ca1cc, 0x2de92c6f, 0x4a7484aa, 0x5cb0a9dc, 0x76f988da,
		0x983e5152, 0xa831c66d, 0xb00327c8, 0xbf597fc7, 0xc6e00bf3, 0xd5a79147, 0x06ca6351, 0x14292967,
		0x27b70a85, 0x2e1b2138, 0x4d2c6dfc, 0x53380d13, 0x650a7354, 0x766a0abb, 0x81c2c92e, 0x92722c85,
		0xa2bfe8a1, 0xa81a664b, 0xc24b8b70, 0xc76c51a3, 0xd192e819, 0xd6990624, 0xf40e3585, 0x106aa070,
		0x19a4c116, 0x1e376c08, 0x2748774c, 0x34b0bcb5, 0x391c0cb3, 0x4ed8aa4a, 0x5b9cca4f, 0x682e6ff3,
		0x748f82ee, 0x78a5636f, 0x84c87814, 0x8cc70208, 0x90befffa, 0xa4506ceb, 0xbef9a3f7, 0xc67178f2,
	}
	h0 := uint32(0x6a09e667)
	h1 := uint32(0xbb67ae85)
	h2 := uint32(0x3c6ef372)
	h3 := uint32(0xa54ff53a)
	h4 := uint32(0x510e527f)
	h5 := uint32(0x9b05688c)
	h6 := uint32(0x1f83d9ab)
	h7 := uint32(0x5be0cd19)
	var W [64]uint32
	for blk := 0; blk < total/64; blk++ {
		base := blk * 64
		for i := 0; i < 16; i++ {
			W[i] = uint32(data[base+i*4])<<24 | uint32(data[base+i*4+1])<<16 |
				uint32(data[base+i*4+2])<<8 | uint32(data[base+i*4+3])
		}
		for i := 16; i < 64; i++ {
			s0 := rotr(W[i-15], 7) ^ rotr(W[i-15], 18) ^ (W[i-15] >> 3)
			s1 := rotr(W[i-2], 17) ^ rotr(W[i-2], 19) ^ (W[i-2] >> 10)
			W[i] = W[i-16] + s0 + W[i-7] + s1
		}
		a := h0
		b := h1
		c := h2
		d := h3
		e := h4
		f := h5
		g := h6
		h := h7
		for i := 0; i < 64; i++ {
			S1 := rotr(e, 6) ^ rotr(e, 11) ^ rotr(e, 25)
			ch := (e & f) ^ (^e & g)
			t1 := h + S1 + ch + K[i] + W[i]
			S0 := rotr(a, 2) ^ rotr(a, 13) ^ rotr(a, 22)
			maj := (a & b) ^ (a & c) ^ (b & c)
			t2 := S0 + maj
			h = g
			g = f
			f = e
			e = d + t1
			d = c
			c = b
			b = a
			a = t1 + t2
		}
		h0 += a
		h1 += b
		h2 += c
		h3 += d
		h4 += e
		h5 += f
		h6 += g
		h7 += h
	}
	return int64(uint64(h0)<<32 | uint64(h1))
}

func main() {
	const msglen = 4194304 // 4MB，与 C++/MYP 同规模
	t0 := time.Now()
	v := sha256(msglen, 24680)
	ms := time.Since(t0).Milliseconds()
	fmt.Printf("verify %d\nms %d\n", v, ms)
}
