// bench/cpp/mandelbrot.cpp — C++ 版 Mandelbrot（与 bench/myp/mandelbrot.myp 同算法）
// 输出：verify <总迭代> / ms <毫秒>
#include <chrono>
#include <cstdio>

static long mandelbrot(int width, int height, int maxIter) {
    long total = 0;
    for (int y = 0; y < height; ++y)
        for (int x = 0; x < width; ++x) {
            double cr = (double)x / width * 3.5 - 2.5;
            double ci = (double)y / height * 2.0 - 1.0;
            double zr = 0, zi = 0;
            int iter = 0;
            while (iter < maxIter) {
                double zr2 = zr * zr, zi2 = zi * zi;
                if (zr2 + zi2 > 4.0) break;
                zi = 2.0 * zr * zi + ci;
                zr = zr2 - zi2 + cr;
                ++iter;
            }
            total += iter;
        }
    return total;
}

int main() {
    const int w = 1000, h = 1000, mi = 256;
    auto t0 = std::chrono::steady_clock::now();
    long t = mandelbrot(w, h, mi);
    auto t1 = std::chrono::steady_clock::now();
    double ms = std::chrono::duration<double, std::milli>(t1 - t0).count();
    std::printf("verify %ld\nms %.0f\n", t, ms);
    return 0;
}
