// bench/cpp/sha256.cpp — SHA-256（与 bench/myp/sha256.myp 同算法同填充）
// 消息 64KB（LCG 高位填充），verify = 哈希前 64 位（有符号 long，%ld 打印）。
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <vector>

static uint32_t rotr(uint32_t x, int n) { return (x >> n) | (x << (32 - n)); }

static int64_t sha256(int msglen, int seed) {
    std::vector<uint8_t> msg(msglen);
    long long rng = seed;
    for (int i = 0; i < msglen; ++i) {
        rng = (rng * 1103515245LL + 12345LL) % 2147483648LL;
        msg[i] = (uint8_t)((rng >> 16) & 0xFF);
    }
    int total = (msglen + 72) / 64 * 64;
    if (total < msglen + 9) total += 64;
    std::vector<uint8_t> data(total, 0);
    for (int i = 0; i < msglen; ++i) data[i] = msg[i];
    data[msglen] = 0x80;
    uint64_t bitlen = (uint64_t)msglen * 8;
    for (int i = 0; i < 8; ++i) data[total - 1 - i] = (bitlen >> (8 * i)) & 0xFF;
    static const uint32_t K[64] = {
        0x428a2f98u,0x71374491u,0xb5c0fbcfu,0xe9b5dba5u,0x3956c25bu,0x59f111f1u,0x923f82a4u,0xab1c5ed5u,
        0xd807aa98u,0x12835b01u,0x243185beu,0x550c7dc3u,0x72be5d74u,0x80deb1feu,0x9bdc06a7u,0xc19bf174u,
        0xe49b69c1u,0xefbe4786u,0x0fc19dc6u,0x240ca1ccu,0x2de92c6fu,0x4a7484aau,0x5cb0a9dcu,0x76f988dau,
        0x983e5152u,0xa831c66du,0xb00327c8u,0xbf597fc7u,0xc6e00bf3u,0xd5a79147u,0x06ca6351u,0x14292967u,
        0x27b70a85u,0x2e1b2138u,0x4d2c6dfcu,0x53380d13u,0x650a7354u,0x766a0abbu,0x81c2c92eu,0x92722c85u,
        0xa2bfe8a1u,0xa81a664bu,0xc24b8b70u,0xc76c51a3u,0xd192e819u,0xd6990624u,0xf40e3585u,0x106aa070u,
        0x19a4c116u,0x1e376c08u,0x2748774cu,0x34b0bcb5u,0x391c0cb3u,0x4ed8aa4au,0x5b9cca4fu,0x682e6ff3u,
        0x748f82eeu,0x78a5636fu,0x84c87814u,0x8cc70208u,0x90befffau,0xa4506cebu,0xbef9a3f7u,0xc67178f2u
    };
    uint32_t h0 = 0x6a09e667u, h1 = 0xbb67ae85u, h2 = 0x3c6ef372u, h3 = 0xa54ff53au,
             h4 = 0x510e527fu, h5 = 0x9b05688cu, h6 = 0x1f83d9abu, h7 = 0x5be0cd19u;
    uint32_t W[64];
    for (int blk = 0; blk < total / 64; ++blk) {
        int base = blk * 64;
        for (int i = 0; i < 16; ++i)
            W[i] = (uint32_t(data[base + i * 4]) << 24) | (uint32_t(data[base + i * 4 + 1]) << 16)
                 | (uint32_t(data[base + i * 4 + 2]) << 8) | uint32_t(data[base + i * 4 + 3]);
        for (int i = 16; i < 64; ++i) {
            uint32_t s0 = rotr(W[i - 15], 7) ^ rotr(W[i - 15], 18) ^ (W[i - 15] >> 3);
            uint32_t s1 = rotr(W[i - 2], 17) ^ rotr(W[i - 2], 19) ^ (W[i - 2] >> 10);
            W[i] = W[i - 16] + s0 + W[i - 7] + s1;
        }
        uint32_t a = h0, b = h1, c = h2, d = h3, e = h4, f = h5, g = h6, h = h7;
        for (int i = 0; i < 64; ++i) {
            uint32_t S1 = rotr(e, 6) ^ rotr(e, 11) ^ rotr(e, 25);
            uint32_t ch = (e & f) ^ ((~e) & g);
            uint32_t t1 = h + S1 + ch + K[i] + W[i];
            uint32_t S0 = rotr(a, 2) ^ rotr(a, 13) ^ rotr(a, 22);
            uint32_t maj = (a & b) ^ (a & c) ^ (b & c);
            uint32_t t2 = S0 + maj;
            h = g; g = f; f = e; e = d + t1; d = c; c = b; b = a; a = t1 + t2;
        }
        h0 += a; h1 += b; h2 += c; h3 += d; h4 += e; h5 += f; h6 += g; h7 += h;
    }
    return (int64_t)(((uint64_t)h0 << 32) | h1);
}

int main() {
    const int msglen = 4194304;   // 4MB，让耗时可测（64KB 时两语言都 <1ms）
    auto t0 = std::chrono::steady_clock::now();
    int64_t v = sha256(msglen, 24680);
    auto t1 = std::chrono::steady_clock::now();
    double ms = std::chrono::duration<double, std::milli>(t1 - t0).count();
    std::printf("verify %ld\nms %.0f\n", (long)v, ms);
    return 0;
}
