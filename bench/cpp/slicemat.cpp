// bench/cpp/slicemat.cpp — 嵌套 slice<slice<int>> 矩阵求和（二维运行时切片）
// M=2048×2048；verify = 全部元素之和。同算法同 LCG。压测 slice-of-slice 双下标。
#include <chrono>
#include <cstdio>
#include <vector>

static long slicemat(int n, int seed) {
    std::vector<std::vector<int>> rows(n, std::vector<int>(n));
    long long rng = seed;
    for (int i = 0; i < n; ++i)
        for (int j = 0; j < n; ++j) {
            rng = (rng * 1103515245LL + 12345LL) % 2147483648LL;
            rows[i][j] = (int)(rng & 0xFF);
        }
    long long sum = 0;
    for (int i = 0; i < n; ++i)
        for (int j = 0; j < n; ++j) sum += rows[i][j];
    return (long)sum;
}

int main() {
    const int n = 2048;
    auto t0 = std::chrono::steady_clock::now();
    long v = slicemat(n, 24680);
    auto t1 = std::chrono::steady_clock::now();
    double ms = std::chrono::duration<double, std::milli>(t1 - t0).count();
    std::printf("verify %ld\nms %.0f\n", v, ms);
    return 0;
}
