// bench/cpp/kmeans.cpp — K-means 聚类（浮点距离 + 数据相关分支）
// N=4096 点 × D=8 维 × K=8 簇 × 100 轮；verify = 最终各点簇号之和。
#include <chrono>
#include <cstdio>
#include <vector>

static long kmeans(int n, int d, int k, int rounds, int seed) {
    std::vector<double> pts(n * d);
    long long rng = seed;
    for (int i = 0; i < n * d; ++i) {
        rng = (rng * 1103515245LL + 12345LL) % 2147483648LL;
        pts[i] = (double)((rng >> 16) % 10000) / 100.0;   // 0..99.99
    }
    std::vector<double> cen(k * d);
    for (int i = 0; i < k * d; ++i) cen[i] = pts[i];
    std::vector<int> assign(n);
    std::vector<double> sum(k * d);
    std::vector<int> cnt(k);
    for (int t = 0; t < rounds; ++t) {
        for (int p = 0; p < n; ++p) {
            double best = 1e300; int bk = 0;
            for (int c = 0; c < k; ++c) {
                double s = 0.0;
                for (int q = 0; q < d; ++q) {
                    double df = pts[p * d + q] - cen[c * d + q];
                    s += df * df;
                }
                if (s < best) { best = s; bk = c; }
            }
            assign[p] = bk;
        }
        for (int c = 0; c < k * d; ++c) sum[c] = 0.0;
        for (int c = 0; c < k; ++c) cnt[c] = 0;
        for (int p = 0; p < n; ++p) {
            int c = assign[p];
            cnt[c]++;
            for (int q = 0; q < d; ++q) sum[c * d + q] += pts[p * d + q];
        }
        for (int c = 0; c < k; ++c)
            if (cnt[c] > 0)
                for (int q = 0; q < d; ++q) cen[c * d + q] = sum[c * d + q] / cnt[c];
    }
    long v = 0;
    for (int p = 0; p < n; ++p) v += assign[p];
    return v;
}

int main() {
    const int n = 16384, d = 8, k = 8, rounds = 400;
    auto t0 = std::chrono::steady_clock::now();
    long v = kmeans(n, d, k, rounds, 24680);
    auto t1 = std::chrono::steady_clock::now();
    double ms = std::chrono::duration<double, std::milli>(t1 - t0).count();
    std::printf("verify %ld\nms %.0f\n", (long)v, ms);
    return 0;
}
