// bench/cpp/crc32.cpp — 表驱动 CRC-32（uint32 表 + 字节循环 + 位移异或）
// 32MB 数据；verify = CRC-32 值（转 long 为正数）。同算法同 LCG。
#include <chrono>
#include <cstdio>
#include <vector>

static unsigned g_crc_table[256];

static unsigned crc32c(unsigned crc, const std::vector<unsigned char>& data, int n) {
    static bool init = false;
    if (!init) {
        for (unsigned i = 0; i < 256; ++i) {
            unsigned c = i;
            for (int k = 0; k < 8; ++k) c = (c & 1) ? 0xEDB88320u ^ (c >> 1) : (c >> 1);
            g_crc_table[i] = c;
        }
        init = true;
    }
    crc ^= 0xFFFFFFFFu;
    for (int i = 0; i < n; ++i) crc = g_crc_table[(crc ^ data[i]) & 0xFFu] ^ (crc >> 8);
    return crc ^ 0xFFFFFFFFu;
}

static long crc32(int n, int seed) {
    std::vector<unsigned char> data(n);
    long long rng = seed;
    for (int i = 0; i < n; ++i) {
        rng = (rng * 1103515245LL + 12345LL) % 2147483648LL;
        data[i] = (unsigned char)((rng >> 16) & 0xFF);
    }
    return (long)crc32c(0, data, n);
}

int main() {
    const int n = 33554432;   // 32MB
    auto t0 = std::chrono::steady_clock::now();
    long v = crc32(n, 24680);
    auto t1 = std::chrono::steady_clock::now();
    double ms = std::chrono::duration<double, std::milli>(t1 - t0).count();
    std::printf("verify %ld\nms %.0f\n", v, ms);
    return 0;
}
