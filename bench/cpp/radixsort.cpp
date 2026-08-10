// bench/cpp/radixsort.cpp — LSD 基数排序（8-bit 4 趟：直方图+前缀和+散布）
// N=10^6 随机整数；verify = 排序后校验和 sum(a[i]*(i+1))。与 MYP 同算法同 LCG。
#include <chrono>
#include <cstdio>
#include <vector>

static long radixsort(int n, int seed) {
    std::vector<int> a(n), b(n);
    long long rng = seed;
    for (int i = 0; i < n; ++i) {
        rng = (rng * 1103515245LL + 12345LL) % 2147483648LL;
        a[i] = (int)(rng & 0x7FFFFFFF);
    }
    int cnt[256];
    for (int shift = 0; shift < 32; shift += 8) {
        for (int i = 0; i < 256; ++i) cnt[i] = 0;
        for (int i = 0; i < n; ++i) cnt[(a[i] >> shift) & 0xFF]++;
        int s = 0;
        for (int i = 0; i < 256; ++i) { int t = cnt[i]; cnt[i] = s; s += t; }
        for (int i = 0; i < n; ++i) b[cnt[(a[i] >> shift) & 0xFF]++] = a[i];
        a.swap(b);
    }
    long long sum = 0;
    for (int i = 0; i < n; ++i) sum += (long long)a[i] * (i + 1);
    return (long)sum;
}

int main() {
    const int n = 4000000;
    auto t0 = std::chrono::steady_clock::now();
    long v = radixsort(n, 24680);
    auto t1 = std::chrono::steady_clock::now();
    double ms = std::chrono::duration<double, std::milli>(t1 - t0).count();
    std::printf("verify %ld\nms %.0f\n", v, ms);
    return 0;
}
