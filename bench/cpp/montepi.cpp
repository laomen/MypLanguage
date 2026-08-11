// bench/cpp/montepi.cpp — C++ 版蒙特卡洛求 π（与 bench/myp/montepi.myp 同 LCG 同规模）
// 输出：verify <落圆内点数> / ms <毫秒>
#include <chrono>
#include <cstdint>
#include <cstdio>

static int mcCount(int n, long long seed) {
    long long rng = seed;
    int inside = 0;
    for (int i = 0; i < n; ++i) {
        rng = (rng * 1103515245LL + 12345LL) & 0x7FFFFFFFLL;
        double x = (double)(rng >> 7) / 16777216.0;
        rng = (rng * 1103515245LL + 12345LL) & 0x7FFFFFFFLL;
        double y = (double)(rng >> 7) / 16777216.0;
        if (x * x + y * y <= 1.0) ++inside;
    }
    return inside;
}

int main() {
    const int N = 100000000;
    auto t0 = std::chrono::steady_clock::now();
    int c = mcCount(N, 24680);
    auto t1 = std::chrono::steady_clock::now();
    double ms = std::chrono::duration<double, std::milli>(t1 - t0).count();
    std::printf("verify %d\nms %.0f\n", c, ms);
    return 0;
}
