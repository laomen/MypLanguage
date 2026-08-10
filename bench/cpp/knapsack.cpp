// bench/cpp/knapsack.cpp — 0/1 背包（动态规划，1D 滚动数组 + 数据相关分支）
// N=5000 物品、容量 5000、重量 1..20、价值 1..1000；verify = dp[容量]。
#include <chrono>
#include <cstdio>
#include <vector>

static long knapsack(int n, int cap, int seed) {
    std::vector<int> wt(n), val(n);
    long long rng = seed;
    for (int i = 0; i < n; ++i) {
        rng = (rng * 1103515245LL + 12345LL) % 2147483648LL;
        wt[i] = (int)((rng >> 16) % 20) + 1;      // 1..20
        rng = (rng * 1103515245LL + 12345LL) % 2147483648LL;
        val[i] = (int)((rng >> 16) % 1000) + 1;   // 1..1000
    }
    std::vector<int> dp(cap + 1, 0);
    for (int i = 0; i < n; ++i)
        for (int w = cap; w >= wt[i]; --w) {
            int cand = dp[w - wt[i]] + val[i];
            if (cand > dp[w]) dp[w] = cand;
        }
    return dp[cap];
}

int main() {
    const int n = 10000, cap = 10000;
    auto t0 = std::chrono::steady_clock::now();
    long v = knapsack(n, cap, 13579);
    auto t1 = std::chrono::steady_clock::now();
    double ms = std::chrono::duration<double, std::milli>(t1 - t0).count();
    std::printf("verify %ld\nms %.0f\n", (long)v, ms);
    return 0;
}
