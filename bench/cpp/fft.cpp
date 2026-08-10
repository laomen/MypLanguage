// bench/cpp/fft.cpp — 迭代 radix-2 FFT（与 bench/myp/fft.myp 同算法）
// 输出：verify <频谱能量> / ms <毫秒>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <vector>

static const double PI = 3.141592653589793;

static double fft(int n, int iters) {
    std::vector<double> re(n), im(n);
    for (int it = 0; it < iters; ++it) {
        // 每次重置确定性有界信号（避免反复 FFT 导致溢出）
        for (int i = 0; i < n; ++i) {
            re[i] = std::sin((i % 100) * 0.1234567);
            im[i] = std::cos((i % 100) * 0.07654321);
        }
        // 位反转
        int j = 0;
        for (int i = 1; i < n; ++i) {
            int bit = n >> 1;
            while ((j & bit) != 0) { j ^= bit; bit >>= 1; }
            j ^= bit;
            if (i < j) { std::swap(re[i], re[j]); std::swap(im[i], im[j]); }
        }
        // 蝶形
        for (int len = 2; len <= n; len <<= 1) {
            double ang = -2.0 * PI / len;
            double wRe = std::cos(ang), wIm = std::sin(ang);
            for (int k = 0; k < n; k += len) {
                double curRe = 1.0, curIm = 0.0;
                for (int k2 = 0; k2 < len / 2; ++k2) {
                    int a = k + k2, b = k + k2 + len / 2;
                    double tre = re[b], tim = im[b];
                    double pRe = tre * curRe - tim * curIm;
                    double pIm = tre * curIm + tim * curRe;
                    re[b] = re[a] - pRe;
                    im[b] = im[a] - pIm;
                    re[a] += pRe;
                    im[a] += pIm;
                    double ncRe = curRe * wRe - curIm * wIm;
                    double ncIm = curRe * wIm + curIm * wRe;
                    curRe = ncRe;
                    curIm = ncIm;
                }
            }
        }
    }
    double e = 0.0;
    for (int i = 0; i < n; ++i) e += re[i] * re[i] + im[i] * im[i];
    return e;
}

int main() {
    const int n = 4096, iters = 800;
    auto t0 = std::chrono::steady_clock::now();
    double e = fft(n, iters);
    auto t1 = std::chrono::steady_clock::now();
    double ms = std::chrono::duration<double, std::milli>(t1 - t0).count();
    std::printf("verify %.10g\nms %.0f\n", e, ms);
    return 0;
}
