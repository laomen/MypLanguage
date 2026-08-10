// bench/cpp/parcomp.cpp — 并行计算（std::thread 16 线程分块，写入 vector<long>）
// N=10^6 计算密集迭代（每迭代 200 次浮点乘加），各线程写不重叠槽位。
// verify = out[] 串行求和。与 MYP @parallel for 版同算法（逐迭代确定性一致）。
#include <chrono>
#include <cstdio>
#include <thread>
#include <vector>

static long parcomp(int n, int seed) {
    std::vector<long> out(n);
    unsigned T = std::thread::hardware_concurrency();
    if (T == 0) T = 1;
    auto work = [&](int begin, int end) {
        for (int i = begin; i < end; ++i) {
            double d = 1.0 + (i % 1000) * 0.001;
            for (int k = 0; k < 200; ++k)
                d = d * 1.0001 + 0.00001;
            out[i] = (long)(d * 1000);
        }
    };
    int chunk = (n + (int)T - 1) / (int)T;
    std::vector<std::thread> threads;
    for (unsigned k = 0; k < T; ++k) {
        int begin = (int)k * chunk;
        int end = begin + chunk < n ? begin + chunk : n;
        if (begin >= n) break;
        threads.emplace_back(work, begin, end);
    }
    for (auto& t : threads) t.join();
    long long sum = 0;
    for (int i = 0; i < n; ++i) sum += out[i];
    return (long)sum;
}

int main() {
    const int n = 1000000;
    auto t0 = std::chrono::steady_clock::now();
    long v = parcomp(n, 24680);
    auto t1 = std::chrono::steady_clock::now();
    double ms = std::chrono::duration<double, std::milli>(t1 - t0).count();
    std::printf("verify %ld\nms %.0f\n", v, ms);
    return 0;
}
