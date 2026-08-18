// bench/cpp/slicedot.cpp — slice<double> 点积对照（std::vector 下标无检查，可向量化）
// 与 MYP 端同算法：N=10^6 两个 double 数组内积。
#include <chrono>
#include <cstdio>
#include <vector>

static double slicedot(int n, int seed) {
    std::vector<double> a(n), b(n);
    long long rng = seed;
    for (int i = 0; i < n; ++i) {
        rng = (rng * 1103515245LL + 12345LL) % 2147483648LL;
        a[i] = (double)((rng >> 16) % 1000) / 100.0;
        rng = (rng * 1103515245LL + 12345LL) % 2147483648LL;
        b[i] = (double)((rng >> 16) % 1000) / 100.0;
    }
    double sum = 0.0;
    for (int i = 0; i < n; ++i) sum += a[i] * b[i];
    return sum;
}

int main() {
    const int n = 1000000;
    auto t0 = std::chrono::steady_clock::now();
    double v = slicedot(n, 24680);
    auto t1 = std::chrono::steady_clock::now();
    double ms = std::chrono::duration<double, std::milli>(t1 - t0).count();
    std::printf("verify %.6f\nms %.0f\n", v, ms);
    return 0;
}
