// bench/cpp/dotprod.cpp — 结构体数组点积（AoS：struct 数组 + 字段访问）
// N=2×10^6 个 Vec{x,y,z}；verify = sum(v[i].x*v[i].y + v[i].z)。同算法同 LCG。
// 压测 MYP 的 struct 数组元素字段读写（v[i].x）——最近补上的 codegen 路径。
#include <chrono>
#include <cstdio>
#include <vector>

struct Vec { int x, y, z; };

static long dotprod(int n, int seed) {
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
    long v = dotprod(n, 24680);
    auto t1 = std::chrono::steady_clock::now();
    double ms = std::chrono::duration<double, std::milli>(t1 - t0).count();
    std::printf("verify %ld\nms %.0f\n", v, ms);
    return 0;
}
