// bench/cpp/sieve.cpp — C++ 版埃氏筛（与 bench/myp/sieve.myp 同算法同规模）
// 输出：verify <素数个数> / ms <毫秒>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <vector>

static int countPrimesSieve(int n) {
    std::vector<uint8_t> comp(n + 1, 0);
    int count = 0;
    for (int p = 2; p <= n; ++p) {
        if (!comp[p]) {
            ++count;
            // 用 long 计算 p*p：p 较大时 int 乘法溢出（与 MYP 版一致）
            for (long j = (long)p * p; j <= n; j += p) comp[(int)j] = 1;
        }
    }
    return count;
}

int main() {
    const int N = 10000000;
    auto t0 = std::chrono::steady_clock::now();
    int c = countPrimesSieve(N);
    auto t1 = std::chrono::steady_clock::now();
    double ms = std::chrono::duration<double, std::milli>(t1 - t0).count();
    std::printf("verify %d\nms %.0f\n", c, ms);
    return 0;
}
