// bench/cpp/base64.cpp — Base64 编码（字符映射 + 位打包，字符串处理）
// 8MB 字节流（LCG 高位）→ base64；verify = 输出字节和 + 采样字符×1000003。
#include <chrono>
#include <cstdio>
#include <vector>

static long base64_bench(int n, int seed) {
    std::vector<unsigned char> in(n);
    long long rng = seed;
    for (int i = 0; i < n; ++i) {
        rng = (rng * 1103515245LL + 12345LL) % 2147483648LL;
        in[i] = (unsigned char)((rng >> 16) & 0xFF);
    }
    int outn = (n / 3) * 4;
    std::vector<unsigned char> out(outn);
    static const char* B64 = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    for (int i = 0, j = 0; i + 2 < n; i += 3, j += 4) {
        int b0 = in[i], b1 = in[i + 1], b2 = in[i + 2];
        out[j]     = (unsigned char)B64[b0 >> 2];
        out[j + 1] = (unsigned char)B64[((b0 & 3) << 4) | (b1 >> 4)];
        out[j + 2] = (unsigned char)B64[((b1 & 15) << 2) | (b2 >> 6)];
        out[j + 3] = (unsigned char)B64[b2 & 63];
    }
    long sum = 0;
    for (int i = 0; i < outn; ++i) sum += out[i];
    return sum + (long)out[123456] * 1000003L;
}

int main() {
    const int n = 8388606;   // 3 的倍数（8MB）
    auto t0 = std::chrono::steady_clock::now();
    long v = base64_bench(n, 24680);
    auto t1 = std::chrono::steady_clock::now();
    double ms = std::chrono::duration<double, std::milli>(t1 - t0).count();
    std::printf("verify %ld\nms %.0f\n", (long)v, ms);
    return 0;
}
