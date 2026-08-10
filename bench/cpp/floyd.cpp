// bench/cpp/floyd.cpp — Floyd-Warshall 全源最短路（稠密三层循环 + 原地最小）
// V=600 邻接矩阵 1..1000 边权、对角线 0；verify = 全部 dist 之和。同算法同 LCG。
#include <chrono>
#include <cstdio>
#include <vector>

static long floyd(int n, int seed) {
    std::vector<int> d(n * n);
    long long rng = seed;
    for (int i = 0; i < n * n; ++i) {
        rng = (rng * 1103515245LL + 12345LL) % 2147483648LL;
        d[i] = (int)((rng >> 16) % 1000) + 1;   // 1..1000
    }
    for (int i = 0; i < n; ++i) d[i * n + i] = 0;
    for (int k = 0; k < n; ++k)
        for (int i = 0; i < n; ++i) {
            int dk = d[i * n + k];
            int base = i * n;
            for (int j = 0; j < n; ++j) {
                int nd = dk + d[k * n + j];
                if (nd < d[base + j]) d[base + j] = nd;
            }
        }
    long sum = 0;
    for (int i = 0; i < n * n; ++i) sum += d[i];
    return sum;
}

int main() {
    const int n = 600;
    auto t0 = std::chrono::steady_clock::now();
    long v = floyd(n, 24680);
    auto t1 = std::chrono::steady_clock::now();
    double ms = std::chrono::duration<double, std::milli>(t1 - t0).count();
    std::printf("verify %ld\nms %.0f\n", v, ms);
    return 0;
}
