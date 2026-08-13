// bench/cpp/fib_matrix.cpp — 矩阵快速幂斐波那契，与 MYP 同算法
#include <chrono>
#include <cstdio>

static long fibMatrix(int n) {
    const long long M = 1000000007LL;
    long long a00 = 1, a01 = 1, a10 = 1, a11 = 0;
    long long r00 = 1, r01 = 0, r10 = 0, r11 = 1;
    while (n > 0) {
        if (n & 1) {
            long long n00 = (r00 * a00 + r01 * a10) % M;
            long long n01 = (r00 * a01 + r01 * a11) % M;
            long long n10 = (r10 * a00 + r11 * a10) % M;
            long long n11 = (r10 * a01 + r11 * a11) % M;
            r00 = n00; r01 = n01; r10 = n10; r11 = n11;
        }
        long long m00 = (a00 * a00 + a01 * a10) % M;
        long long m01 = (a00 * a01 + a01 * a11) % M;
        long long m10 = (a10 * a00 + a11 * a10) % M;
        long long m11 = (a10 * a01 + a11 * a11) % M;
        a00 = m00; a01 = m01; a10 = m10; a11 = m11;
        n >>= 1;
    }
    return (long)r01;
}

int main() {
    const int n = 100000000;
    auto t0 = std::chrono::steady_clock::now();
    long v = 0;
    for (int r = 0; r < 50000; ++r) v += fibMatrix(n);
    auto t1 = std::chrono::steady_clock::now();
    double ms = std::chrono::duration<double, std::milli>(t1 - t0).count();
    std::printf("verify %ld\nms %.0f\n", v, ms);
    return 0;
}
