// bench/cpp/matmul.cpp — C++ 版矩阵乘法（与 bench/myp/matmul.myp 同算法）
// 输出：verify <迹> / ms <毫秒>
#include <chrono>
#include <cstdio>
#include <vector>

static double matmulTrace(int n) {
    std::vector<double> A(n * n), B(n * n), C(n * n, 0.0);
    for (int i = 0; i < n * n; ++i) {
        A[i] = (i % 1000);
        B[i] = (i % 7);
    }
    // 分块(blocked)矩阵乘：内层 jj 循环 C/B 连续 + A 元素不变 → 可向量化
    const int BS = 64;
    for (int i0 = 0; i0 < n; i0 += BS)
        for (int j0 = 0; j0 < n; j0 += BS)
            for (int k0 = 0; k0 < n; k0 += BS)
                for (int i1 = i0; i1 < i0 + BS && i1 < n; ++i1)
                    for (int k1 = k0; k1 < k0 + BS && k1 < n; ++k1) {
                        double av = A[i1 * n + k1];
                        for (int j1 = j0; j1 < j0 + BS && j1 < n; ++j1)
                            C[i1 * n + j1] += av * B[k1 * n + j1];
                    }
    double trace = 0.0;
    for (int i = 0; i < n; ++i) trace += C[i * n + i];
    return trace;
}

int main() {
    const int n = 512;
    auto t0 = std::chrono::steady_clock::now();
    double tr = matmulTrace(n);
    auto t1 = std::chrono::steady_clock::now();
    double ms = std::chrono::duration<double, std::milli>(t1 - t0).count();
    std::printf("verify %.10g\nms %.0f\n", tr, ms);
    return 0;
}
