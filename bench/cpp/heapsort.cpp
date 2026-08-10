// bench/cpp/heapsort.cpp — 二叉堆排序（堆化 + sift-down + 交换）
// N=10^6 随机整数；verify = 排序后校验和 sum(a[i]*(i+1))。与 MYP 同算法同 LCG。
#include <chrono>
#include <cstdio>
#include <vector>

static void siftDown(std::vector<int>& a, int start, int end) {
    int root = start;
    while (2 * root + 1 <= end) {
        int child = 2 * root + 1;
        int swap = root;
        if (a[swap] < a[child]) swap = child;
        if (child + 1 <= end && a[swap] < a[child + 1]) swap = child + 1;
        if (swap == root) return;
        int t = a[root]; a[root] = a[swap]; a[swap] = t;
        root = swap;
    }
}

static long heapsort(int n, int seed) {
    std::vector<int> a(n);
    long long rng = seed;
    for (int i = 0; i < n; ++i) {
        rng = (rng * 1103515245LL + 12345LL) % 2147483648LL;
        a[i] = (int)(rng & 0x7FFFFFFF);
    }
    for (int i = n / 2 - 1; i >= 0; --i) siftDown(a, i, n - 1);
    for (int end = n - 1; end > 0; --end) {
        int t = a[0]; a[0] = a[end]; a[end] = t;
        siftDown(a, 0, end - 1);
    }
    long long sum = 0;
    for (int i = 0; i < n; ++i) sum += (long long)a[i] * (i + 1);
    return (long)sum;
}

int main() {
    const int n = 1000000;
    auto t0 = std::chrono::steady_clock::now();
    long v = heapsort(n, 24680);
    auto t1 = std::chrono::steady_clock::now();
    double ms = std::chrono::duration<double, std::milli>(t1 - t0).count();
    std::printf("verify %ld\nms %.0f\n", v, ms);
    return 0;
}
