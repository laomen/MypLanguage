// bench/cpp/spmv.cpp — 稀疏矩阵×稠密向量（CSR 风格 gather，cactuBSSN 类）
// N=8192 行 × K=32 非零/行，列号 LCG 随机（cache 不友好 gather）。
// verify = 结果向量 y 之和（double，同序求和位级一致）。
#include <chrono>
#include <cstdio>
#include <vector>

static double spmv(int n, int k, int seed) {
    std::vector<int> col(n * k);
    std::vector<double> val(n * k);
    std::vector<double> x(n);
    long long rng = seed;
    for (int i = 0; i < n * k; ++i) {
        rng = (rng * 1103515245LL + 12345LL) % 2147483648LL;
        col[i] = (int)((rng >> 16) % n);
        rng = (rng * 1103515245LL + 12345LL) % 2147483648LL;
        val[i] = (double)((rng >> 16) % 1000) / 100.0;
    }
    for (int i = 0; i < n; ++i) {
        rng = (rng * 1103515245LL + 12345LL) % 2147483648LL;
        x[i] = (double)((rng >> 16) % 1000) / 100.0;
    }
    std::vector<double> y(n, 0.0);
    for (int i = 0; i < n; ++i) {
        double s = 0.0;
        for (int j = 0; j < k; ++j) s += val[i * k + j] * x[col[i * k + j]];
        y[i] = s;
    }
    double sum = 0.0;
    for (int i = 0; i < n; ++i) sum += y[i];
    return sum;
}

int main() {
    const int n = 65536, k = 64;
    auto t0 = std::chrono::steady_clock::now();
    double v = spmv(n, k, 13579);
    auto t1 = std::chrono::steady_clock::now();
    double ms = std::chrono::duration<double, std::milli>(t1 - t0).count();
    std::printf("verify %.6f\nms %.0f\n", v, ms);
    return 0;
}
