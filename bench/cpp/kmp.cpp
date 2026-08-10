// bench/cpp/kmp.cpp — KMP 字符串匹配（失配表 + 单趟扫描，串模式）
// 32MB 文本 × 256 字节模式（均 LCG）；verify = 匹配次数。
#include <chrono>
#include <cstdio>
#include <vector>

static long kmp(int n, int m, int seed) {
    std::vector<unsigned char> text(n), pat(m);
    long long rng = seed;
    for (int i = 0; i < n; ++i) {
        rng = (rng * 1103515245LL + 12345LL) % 2147483648LL;
        text[i] = (unsigned char)((rng >> 16) & 0xFF);
    }
    for (int i = 0; i < m; ++i) {
        rng = (rng * 1103515245LL + 12345LL) % 2147483648LL;
        pat[i] = (unsigned char)((rng >> 16) & 0xFF);
    }
    std::vector<int> fail(m);
    fail[0] = 0;
    int j = 0;
    for (int i = 1; i < m; ++i) {
        while (j > 0 && pat[i] != pat[j]) j = fail[j - 1];
        if (pat[i] == pat[j]) j++;
        fail[i] = j;
    }
    long count = 0;
    j = 0;
    for (int i = 0; i < n; ++i) {
        while (j > 0 && text[i] != pat[j]) j = fail[j - 1];
        if (text[i] == pat[j]) j++;
        if (j == m) { count++; j = fail[j - 1]; }
    }
    return count;
}

int main() {
    const int n = 33554432, m = 256;   // 32MB 文本
    auto t0 = std::chrono::steady_clock::now();
    long v = kmp(n, m, 98765);
    auto t1 = std::chrono::steady_clock::now();
    double ms = std::chrono::duration<double, std::milli>(t1 - t0).count();
    std::printf("verify %ld\nms %.0f\n", (long)v, ms);
    return 0;
}
