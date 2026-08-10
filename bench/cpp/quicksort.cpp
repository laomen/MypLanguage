// bench/cpp/quicksort.cpp — 快速排序（Lomuto 分区，递归）
// 随机数组（LCG 全值 0..2^31-1）800000 个；verify = 排序后 a[n/2] + 总和。
#include <chrono>
#include <cstdio>
#include <vector>

static void qs(int* a, int lo, int hi) {
    if (lo >= hi) return;
    int pivot = a[hi];
    int i = lo;
    for (int j = lo; j < hi; ++j) {
        if (a[j] <= pivot) {
            int t = a[i]; a[i] = a[j]; a[j] = t;
            i++;
        }
    }
    int t = a[i]; a[i] = a[hi]; a[hi] = t;
    qs(a, lo, i - 1);
    qs(a, i + 1, hi);
}

static long quicksort(int n, int seed) {
    std::vector<int> a(n);
    long long rng = seed;
    for (int i = 0; i < n; ++i) {
        rng = (rng * 1103515245LL + 12345LL) % 2147483648LL;
        a[i] = (int)rng;
    }
    qs(a.data(), 0, n - 1);
    long sum = 0;
    for (int i = 0; i < n; ++i) sum += a[i];
    return sum + a[n / 2];
}

int main() {
    const int n = 800000;
    auto t0 = std::chrono::steady_clock::now();
    long v = quicksort(n, 24680);
    auto t1 = std::chrono::steady_clock::now();
    double ms = std::chrono::duration<double, std::milli>(t1 - t0).count();
    std::printf("verify %ld\nms %.0f\n", (long)v, ms);
    return 0;
}
