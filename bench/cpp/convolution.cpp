// bench/cpp/convolution.cpp — 2D 图像卷积（5×5 核，滑窗，imagick 类）
// N=2048 图像；verify = 输出图像元素之和（double，同序求和位级一致）。
#include <chrono>
#include <cstdio>
#include <vector>

static double convolution(int n, int k, int seed) {
    std::vector<double> img(n * n), out(n * n, 0.0), ker(k * k);
    long long rng = seed;
    for (int i = 0; i < n * n; ++i) {
        rng = (rng * 1103515245LL + 12345LL) % 2147483648LL;
        img[i] = (double)((rng >> 16) % 1000) / 100.0;
    }
    for (int i = 0; i < k * k; ++i) {
        rng = (rng * 1103515245LL + 12345LL) % 2147483648LL;
        ker[i] = (double)((rng >> 16) % 100) / 100.0;
    }
    int hk = k / 2;
    for (int y = hk; y + hk < n; ++y)
        for (int x = hk; x + hk < n; ++x) {
            double s = 0.0;
            for (int ky = 0; ky < k; ++ky)
                for (int kx = 0; kx < k; ++kx)
                    s += img[(y + ky - hk) * n + (x + kx - hk)] * ker[ky * k + kx];
            out[y * n + x] = s;
        }
    double sum = 0.0;
    for (int i = 0; i < n * n; ++i) sum += out[i];
    return sum;
}

int main() {
    const int n = 2048, k = 5;
    auto t0 = std::chrono::steady_clock::now();
    double v = convolution(n, k, 24680);
    auto t1 = std::chrono::steady_clock::now();
    double ms = std::chrono::duration<double, std::milli>(t1 - t0).count();
    std::printf("verify %.6f\nms %.0f\n", v, ms);
    return 0;
}
