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
    for (int r = 0; r < n; ++r)
        for (int c = 0; c < n; ++c) {
            double sum = 0.0;
            for (int k = 0; k < n; ++k) sum += A[r * n + k] * B[k * n + c];
            C[r * n + c] = sum;
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
