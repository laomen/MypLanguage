// bench/cpp/dot_f64.cpp — double 点积（浮点归约），与 MYP 同算法
#include <chrono>
#include <cstdio>
#include <vector>

static double dotF64(int n, int seed) {
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
    double v = dotF64(n, 24680);
    auto t1 = std::chrono::steady_clock::now();
    double ms = std::chrono::duration<double, std::milli>(t1 - t0).count();
    std::printf("verify %.6f\nms %.0f\n", v, ms);
    return 0;
}
