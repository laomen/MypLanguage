// bench/cpp/nbody.cpp — C++ 版 N-body（与 bench/myp/nbody.myp 同算法）
// 输出：verify <动能> / ms <毫秒>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <vector>

static double nbodyKE(int n, int steps) {
    std::vector<double> px(n), py(n), pz(n), vx(n, 0), vy(n, 0), vz(n, 0), m(n);
    for (int i = 0; i < n; ++i) {
        px[i] = (i * 37) % 1000;
        py[i] = (i * 53) % 1000;
        pz[i] = (i * 71) % 1000;
        m[i] = 1.0 + (i % 10) * 0.1;
    }
    for (int step = 0; step < steps; ++step) {
        for (int a = 0; a < n; ++a) {
            double ax = 0, ay = 0, az = 0;
            for (int b = 0; b < n; ++b) {
                if (b == a) continue;
                double dx = px[b] - px[a], dy = py[b] - py[a], dz = pz[b] - pz[a];
                double d2 = dx * dx + dy * dy + dz * dz + 1e-6;
                double inv = 1.0 / (d2 * std::sqrt(d2));
                double mm = m[b] * inv;
                ax += dx * mm;
                ay += dy * mm;
                az += dz * mm;
            }
            vx[a] += ax;
            vy[a] += ay;
            vz[a] += az;
        }
        for (int i = 0; i < n; ++i) {
            px[i] += vx[i];
            py[i] += vy[i];
            pz[i] += vz[i];
        }
    }
    double ke = 0;
    for (int i = 0; i < n; ++i)
        ke += 0.5 * m[i] * (vx[i] * vx[i] + vy[i] * vy[i] + vz[i] * vz[i]);
    return ke;
}

int main() {
    const int n = 5000, steps = 2;
    auto t0 = std::chrono::steady_clock::now();
    double ke = nbodyKE(n, steps);
    auto t1 = std::chrono::steady_clock::now();
    double ms = std::chrono::duration<double, std::milli>(t1 - t0).count();
    std::printf("verify %.10g\nms %.0f\n", ke, ms);
    return 0;
}
