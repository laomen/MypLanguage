// bench/cpp/bigint.cpp — 大数乘法（schoolbook，uint16 字 + 64 位进位链）
// 512 字（8192 位）× 500 次（每次 LCG 重新生成操作数）；
// verify = 结果字滚动校验和。
#include <chrono>
#include <cstdio>
#include <vector>

static long bigint(int n, int iters, int seed) {
    std::vector<unsigned short> A(n), B(n), C(2 * n);
    long long rng = seed;
    long checksum = 0;
    for (int it = 0; it < iters; ++it) {
        for (int i = 0; i < n; ++i) {
            rng = (rng * 1103515245LL + 12345LL) % 2147483648LL;
            A[i] = (unsigned short)(rng >> 16);       // 0..32767
            rng = (rng * 1103515245LL + 12345LL) % 2147483648LL;
            B[i] = (unsigned short)(rng >> 16);
        }
        for (int i = 0; i < 2 * n; ++i) C[i] = 0;
        for (int i = 0; i < n; ++i) {
            unsigned long long carry = 0;
            unsigned int av = A[i];
            for (int j = 0; j < n; ++j) {
                unsigned long long cur = (unsigned long long)C[i + j] + (unsigned long long)av * B[j] + carry;
                C[i + j] = (unsigned short)(cur & 0xFFFFu);
                carry = cur >> 16;
            }
            int pos = i + n;
            while (carry && pos < 2 * n) {
                unsigned long long s = C[pos] + carry;
                C[pos] = (unsigned short)(s & 0xFFFFu);
                carry = s >> 16;
                pos++;
            }
        }
        // 滚动校验和（叠加结果字，重排不改变加和顺序）
        for (int i = 0; i < 2 * n; ++i) checksum += C[i] * (i + 1);
    }
    return checksum;
}

int main() {
    const int n = 512, iters = 500;
    auto t0 = std::chrono::steady_clock::now();
    long v = bigint(n, iters, 13579);
    auto t1 = std::chrono::steady_clock::now();
    double ms = std::chrono::duration<double, std::milli>(t1 - t0).count();
    std::printf("verify %ld\nms %.0f\n", (long)v, ms);
    return 0;
}
