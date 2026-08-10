// bench/cpp/tripleloop.cpp — C++ 版三层嵌套循环（与 bench/myp/tripleloop.myp 同算法）
// 输出：verify <累加和> / ms <毫秒>
#include <chrono>
#include <cstdio>

static long tripleLoop(int n) {
    long sum = 0;
    for (int i = 0; i < n; ++i)
        for (int j = 0; j < n; ++j)
            for (int k = 0; k < n; ++k)
                sum += (i * 101 + j * 97 + k * 89) % 997;
    return sum;
}

int main() {
    const int n = 300;
    auto t0 = std::chrono::steady_clock::now();
    long s = tripleLoop(n);
    auto t1 = std::chrono::steady_clock::now();
    double ms = std::chrono::duration<double, std::milli>(t1 - t0).count();
    std::printf("verify %ld\nms %.0f\n", s, ms);
    return 0;
}
