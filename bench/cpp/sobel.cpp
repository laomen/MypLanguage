// bench/cpp/sobel.cpp — Sobel 边缘检测（3×3 梯度，uint8 灰度图，字节滑窗）
// N=1024 图像；verify = 所有像素梯度幅值之和（|gx|+|gy|，clamp 255）。同算法同 LCG。
#include <chrono>
#include <cstdio>
#include <vector>

static int sobel(int n, int seed) {
    std::vector<unsigned char> img(n * n), out(n * n);
    long long rng = seed;
    for (int i = 0; i < n * n; ++i) {
        rng = (rng * 1103515245LL + 12345LL) % 2147483648LL;
        img[i] = (unsigned char)((rng >> 16) & 0xFF);
    }
    long sum = 0;
    for (int y = 1; y < n - 1; ++y)
        for (int x = 1; x < n - 1; ++x) {
            int gx = -img[(y - 1) * n + x - 1] - 2 * img[y * n + x - 1] - img[(y + 1) * n + x - 1]
                     + img[(y - 1) * n + x + 1] + 2 * img[y * n + x + 1] + img[(y + 1) * n + x + 1];
            int gy = -img[(y - 1) * n + x - 1] - 2 * img[(y - 1) * n + x] - img[(y - 1) * n + x + 1]
                     + img[(y + 1) * n + x - 1] + 2 * img[(y + 1) * n + x] + img[(y + 1) * n + x + 1];
            int m = (gx < 0 ? -gx : gx) + (gy < 0 ? -gy : gy);
            if (m > 255) m = 255;
            out[y * n + x] = (unsigned char)m;
            sum += m;
        }
    return (int)sum;
}

int main() {
    const int n = 2048;
    auto t0 = std::chrono::steady_clock::now();
    int v = sobel(n, 24680);
    auto t1 = std::chrono::steady_clock::now();
    double ms = std::chrono::duration<double, std::milli>(t1 - t0).count();
    std::printf("verify %d\nms %.0f\n", v, ms);
    return 0;
}
