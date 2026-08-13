// bench/cpp/fannkuch.cpp — fannkuch-redux（Benchmarks Game 标准），与 MYP 同算法
#include <chrono>
#include <cstdio>
#include <vector>
using namespace std;

static int fannkuch(int n) {
    vector<int> perm(n), perm1(n), count(n);
    for (int i = 0; i < n; ++i) perm1[i] = i;
    int maxflips = 0, r = n;
    while (true) {
        while (r != 1) { count[r - 1] = r; --r; }
        for (int i = 0; i < n; ++i) perm[i] = perm1[i];
        int flips = 0;
        while (perm[0] != 0) {
            int k = perm[0];
            for (int i = 0, j = k; i < j; ++i, --j) swap(perm[i], perm[j]);
            ++flips;
        }
        if (flips > maxflips) maxflips = flips;
        while (true) {
            if (r == n) return maxflips;
            int perm0 = perm1[0];
            for (int i = 0; i < r; ++i) perm1[i] = perm1[i + 1];
            perm1[r] = perm0;
            --count[r];
            if (count[r] > 0) break;
            ++r;
        }
    }
}

int main() {
    const int n = 11;
    auto t0 = chrono::steady_clock::now();
    int v = fannkuch(n);
    auto t1 = chrono::steady_clock::now();
    double ms = chrono::duration<double, milli>(t1 - t0).count();
    printf("verify %d\nms %.0f\n", v, ms);
    return 0;
}
