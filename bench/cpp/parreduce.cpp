// bench/cpp/parreduce.cpp — 并行归约（std::thread 16 线程各自累加，无锁）
// N=10^6 随机小整数（0..999，LCG），verify = 总和（无溢出，精确一致）。
// 与 MYP @parallel for + Atomic 版同算法：各线程累加自己的部分和，最后求和。
#include <chrono>
#include <cstdio>
#include <thread>
#include <vector>

static long parreduce(int n, int seed) {
    std::vector<int> a(n);
    long long rng = seed;
    for (int i = 0; i < n; ++i) {
        rng = (rng * 1103515245LL + 12345LL) % 2147483648LL;
        a[i] = (int)((rng >> 16) % 1000);
    }
    unsigned T = std::thread::hardware_concurrency();
    if (T == 0) T = 1;
    std::vector<long long> tally(T, 0);
    int chunk = (n + (int)T - 1) / (int)T;
    std::vector<std::thread> threads;
    for (unsigned k = 0; k < T; ++k) {
        int begin = (int)k * chunk;
        int end = begin + chunk < n ? begin + chunk : n;
        if (begin >= n) break;
        threads.emplace_back([&, k, begin, end]() {
            long long s = 0;
            for (int i = begin; i < end; ++i) s += a[i];
            tally[k] = s;
        });
    }
    for (auto& t : threads) t.join();
    long long sum = 0;
    for (unsigned k = 0; k < T; ++k) sum += tally[k];
    return (long)sum;
}

int main() {
    const int n = 1000000;
    auto t0 = std::chrono::steady_clock::now();
    long v = parreduce(n, 24680);
    auto t1 = std::chrono::steady_clock::now();
    double ms = std::chrono::duration<double, std::milli>(t1 - t0).count();
    std::printf("verify %ld\nms %.0f\n", v, ms);
    return 0;
}
