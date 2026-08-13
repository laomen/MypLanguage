// bench/cpp/lcs.cpp — 最长公共子序列（DP 扁平表），与 MYP 同算法
#include <chrono>
#include <cstdio>
#include <vector>

static int lcs(int n, int seed) {
    std::vector<unsigned char> s(n), t(n);
    long long rng = seed;
    for (int i = 0; i < n; ++i) {
        rng = (rng * 1103515245LL + 12345LL) % 2147483648LL;
        s[i] = (unsigned char)((rng >> 16) % 4);
        rng = (rng * 1103515245LL + 12345LL) % 2147483648LL;
        t[i] = (unsigned char)((rng >> 16) % 4);
    }
    int W = n + 1;
    std::vector<int> dp(W * W, 0);
    for (int i = 1; i <= n; ++i)
        for (int j = 1; j <= n; ++j) {
            if (s[i - 1] == t[j - 1])
                dp[i * W + j] = dp[(i - 1) * W + (j - 1)] + 1;
            else if (dp[(i - 1) * W + j] >= dp[i * W + (j - 1)])
                dp[i * W + j] = dp[(i - 1) * W + j];
            else
                dp[i * W + j] = dp[i * W + (j - 1)];
        }
    return dp[n * W + n];
}

int main() {
    const int n = 2000;
    auto t0 = std::chrono::steady_clock::now();
    int v = lcs(n, 24680);
    auto t1 = std::chrono::steady_clock::now();
    double ms = std::chrono::duration<double, std::milli>(t1 - t0).count();
    std::printf("verify %d\nms %.0f\n", v, ms);
    return 0;
}
