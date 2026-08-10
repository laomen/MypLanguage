// bench/cpp/hashmap.cpp — C++ 版哈希表（std::unordered_map，与 bench/myp/hashmap.myp 同规模）
// 输出：verify <键值和> / ms <毫秒>
#include <chrono>
#include <cstdio>
#include <unordered_map>

static long hashmapBench(int n) {
    std::unordered_map<int, int> m;
    m.reserve(n);
    for (int i = 0; i < n; ++i) m[i] = i * 3;
    long sum = 0;
    for (int i = 0; i < n; ++i) sum += m[i];
    return sum;
}

int main() {
    const int n = 1000000;
    auto t0 = std::chrono::steady_clock::now();
    long s = hashmapBench(n);
    auto t1 = std::chrono::steady_clock::now();
    double ms = std::chrono::duration<double, std::milli>(t1 - t0).count();
    std::printf("verify %ld\nms %.0f\n", s, ms);
    return 0;
}
