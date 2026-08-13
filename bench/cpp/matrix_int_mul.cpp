// bench/cpp/matrix_int_mul.cpp — 整型矩阵乘法 256×256 (int)，与 MYP 同算法
#include <chrono>
#include <cstdio>
#include <vector>

static long matrixIntMul(int n) {
    std::vector<int> A(n * n), B(n * n), C(n * n, 0);
    for (int i = 0; i < n * n; ++i) {
        A[i] = i % 1000;
        B[i] = i % 7;
    }
    for (int i0 = 0; i0 < n; ++i0)
        for (int k0 = 0; k0 < n; ++k0) {
            int av = A[i0 * n + k0];
            for (int j1 = 0; j1 < n; ++j1)
                C[i0 * n + j1] += av * B[k0 * n + j1];
        }
    long trace = 0;
    for (int i = 0; i < n; ++i) trace += C[i * n + i];
    return trace;
}

int main() {
    const int n = 256;
    auto t0 = std::chrono::steady_clock::now();
    long v = matrixIntMul(n);
    auto t1 = std::chrono::steady_clock::now();
    double ms = std::chrono::duration<double, std::milli>(t1 - t0).count();
    std::printf("verify %ld\nms %.0f\n", v, ms);
    return 0;
}
