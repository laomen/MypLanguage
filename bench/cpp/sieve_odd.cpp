// bench/cpp/sieve_odd.cpp — C++ 版只筛奇数埃氏筛（与 bench/myp/sieve_odd.myp 同算法同规模）
// 输出：verify <素数个数> / ms <毫秒>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <vector>

static int countPrimesOdd(int n) {
    // idx = (k-3)/2 ↔ k = 2*idx+3（只表示奇数 3,5,7,...）
    std::vector<uint8_t> comp((n - 3) / 2 + 1, 0);
    int count = 1;  // 2
    for (int p = 3; p <= n; p += 2) {
        int idx = (p - 3) / 2;
        if (!comp[idx]) {
            ++count;
            // 只标记奇数倍：从 p*p 起步长 2p（long 防 p*p 溢出，与 MYP 版一致）
            for (long j = (long)p * p; j <= n; j += 2L * p)
                comp[(int)((j - 3) / 2)] = 1;
        }
    }
    return count;
}

int main() {
    const int N = 10000000;
    auto t0 = std::chrono::steady_clock::now();
    int c = countPrimesOdd(N);
    auto t1 = std::chrono::steady_clock::now();
    double ms = std::chrono::duration<double, std::milli>(t1 - t0).count();
    std::printf("verify %d\nms %.0f\n", c, ms);
    return 0;
}
