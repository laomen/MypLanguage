// bench/cpp/spectral_norm.cpp — spectral-norm（Benchmarks Game 标准），与 MYP 同算法
#include <chrono>
#include <cmath>
#include <cstdio>
#include <vector>
using namespace std;

static double spectral(int n) {
    vector<double> u(n, 1.0), v(n, 0.0);
    for (int it = 0; it < 10; ++it) {
        for (int j = 0; j < n; ++j) {
            double s = 0.0;
            for (int i = 0; i < n; ++i)
                s += 1.0 / ((double)((i + j) * (i + j + 1)) / 2.0 + j + 1.0) * u[i];
            v[j] = s;
        }
        for (int j = 0; j < n; ++j) {
            double s = 0.0;
            for (int i = 0; i < n; ++i)
                s += 1.0 / ((double)((j + i) * (j + i + 1)) / 2.0 + i + 1.0) * v[i];
            u[j] = s;
        }
    }
    double vBv = 0.0, vv = 0.0;
    for (int i = 0; i < n; ++i) { vBv += u[i] * v[i]; vv += v[i] * v[i]; }
    return sqrt(vBv / vv);
}

int main() {
    const int n = 5500;
    auto t0 = chrono::steady_clock::now();
    double v = spectral(n);
    auto t1 = chrono::steady_clock::now();
    double ms = chrono::duration<double, milli>(t1 - t0).count();
    printf("verify %.6f\nms %.0f\n", v, ms);
    return 0;
}
