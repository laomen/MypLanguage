// bench/cpp/slicevec.cpp — slice<Vec> 结构体切片点积（AoS + 运行时边界检查）
// N=2×10^6 个 Vec{x,y,z}；verify = sum(v[i].x*v[i].y + v[i].z)。同算法同 LCG。
// 与 dotprod 对比：MYP 侧 slice 下标带边界检查（设计使然），测其开销。
#include <chrono>
#include <cstdio>
#include <vector>

struct Vec { int x, y, z; };

static long slicevec(int n, int seed) {
    std::vector<Vec> v(n);
    long long rng = seed;
    for (int i = 0; i < n; ++i) {
        rng = (rng * 1103515245LL + 12345LL) % 2147483648LL;
        v[i].x = (int)((rng >> 16) % 100);
        rng = (rng * 1103515245LL + 12345LL) % 2147483648LL;
        v[i].y = (int)((rng >> 16) % 100);
        rng = (rng * 1103515245LL + 12345LL) % 2147483648LL;
        v[i].z = (int)((rng >> 16) % 100);
    }
    long long sum = 0;
    for (int i = 0; i < n; ++i) sum += (long long)v[i].x * v[i].y + v[i].z;
    return (long)sum;
}

int main() {
    const int n = 2000000;
    auto t0 = std::chrono::steady_clock::now();
    long v = slicevec(n, 24680);
    auto t1 = std::chrono::steady_clock::now();
    double ms = std::chrono::duration<double, std::milli>(t1 - t0).count();
    std::printf("verify %ld\nms %.0f\n", v, ms);
    return 0;
}
