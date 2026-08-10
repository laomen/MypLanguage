// bench/cpp/nbodybg.cpp — C++ 版 benchmarksgame n-body（与 bench/myp/nbodybg.myp 同算法）
// 数据/算法与
//   https://benchmarksgame-team.pages.debian.net/benchmarksgame/program/nbody-gcc-9.html
// 一致（标量形式；gcc#9 是手写 AVX 版）。输出格式同 MYP：首行初始能量（%.9f），
// verify=advance 后末能量（%.9f），ms=advance 耗时。
#include <chrono>
#include <cmath>
#include <cstdio>

static constexpr double PI = 3.141592653589793;
static constexpr double SOLAR_MASS = 4.0 * PI * PI;
static constexpr double DAYS_PER_YEAR = 365.24;

static double energy(int n, double* mass, double* px, double* py, double* pz,
                     double* vx, double* vy, double* vz) {
    double e = 0.0;
    for (int i = 0; i < n; ++i) {
        e += 0.5 * mass[i] * (vx[i] * vx[i] + vy[i] * vy[i] + vz[i] * vz[i]);
        for (int j = i + 1; j < n; ++j) {
            double dx = px[i] - px[j];
            double dy = py[i] - py[j];
            double dz = pz[i] - pz[j];
            double dist = std::sqrt(dx * dx + dy * dy + dz * dz);
            e -= (mass[i] * mass[j]) / dist;
        }
    }
    return e;
}

static void advance(int n, double dt, double* mass, double* px, double* py,
                    double* pz, double* vx, double* vy, double* vz) {
    for (int i = 0; i < n; ++i) {
        for (int j = i + 1; j < n; ++j) {
            double dx = px[i] - px[j];
            double dy = py[i] - py[j];
            double dz = pz[i] - pz[j];
            double d2 = dx * dx + dy * dy + dz * dz;
            double mag = dt / (d2 * std::sqrt(d2));
            vx[i] -= dx * mass[j] * mag;
            vy[i] -= dy * mass[j] * mag;
            vz[i] -= dz * mass[j] * mag;
            vx[j] += dx * mass[i] * mag;
            vy[j] += dy * mass[i] * mag;
            vz[j] += dz * mass[i] * mag;
        }
    }
    for (int i = 0; i < n; ++i) {
        px[i] += dt * vx[i];
        py[i] += dt * vy[i];
        pz[i] += dt * vz[i];
    }
}

int main() {
    const int n = 5;
    double mass[n], px[n], py[n], pz[n], vx[n], vy[n], vz[n];

    // sun
    mass[0] = SOLAR_MASS; px[0] = 0; py[0] = 0; pz[0] = 0; vx[0] = 0; vy[0] = 0; vz[0] = 0;
    // jupiter
    mass[1] = 9.54791938424326609e-04 * SOLAR_MASS;
    px[1] = 4.84143144246472090e+00; py[1] = -1.16032004402742839e+00; pz[1] = -1.03622044471123109e-01;
    vx[1] = 1.66007664274403694e-03 * DAYS_PER_YEAR; vy[1] = 7.69901118419740425e-03 * DAYS_PER_YEAR; vz[1] = -6.90460016972063023e-05 * DAYS_PER_YEAR;
    // saturn
    mass[2] = 2.85885980666130812e-04 * SOLAR_MASS;
    px[2] = 8.34336671824457987e+00; py[2] = 4.12479856412430479e+00; pz[2] = -4.03523417114321381e-01;
    vx[2] = -2.76742510726862411e-03 * DAYS_PER_YEAR; vy[2] = 4.99852801234917238e-03 * DAYS_PER_YEAR; vz[2] = 2.30417297573763929e-05 * DAYS_PER_YEAR;
    // uranus
    mass[3] = 4.36624404335156298e-05 * SOLAR_MASS;
    px[3] = 1.28943695621391310e+01; py[3] = -1.51111514016986312e+01; pz[3] = -2.23307578892655734e-01;
    vx[3] = 2.96460137564761618e-03 * DAYS_PER_YEAR; vy[3] = 2.37847173959480950e-03 * DAYS_PER_YEAR; vz[3] = -2.96589568540237556e-05 * DAYS_PER_YEAR;
    // neptune
    mass[4] = 5.15138902046611451e-05 * SOLAR_MASS;
    px[4] = 1.53796971148509165e+01; py[4] = -2.59193146099879641e+01; pz[4] = 1.79258772950371181e-01;
    vx[4] = 2.68067772490389322e-03 * DAYS_PER_YEAR; vy[4] = 1.62824170038242295e-03 * DAYS_PER_YEAR; vz[4] = -9.51592254519715870e-05 * DAYS_PER_YEAR;

    // offset momentum
    double mpx = 0, mpy = 0, mpz = 0;
    for (int i = 0; i < n; ++i) {
        mpx += vx[i] * mass[i];
        mpy += vy[i] * mass[i];
        mpz += vz[i] * mass[i];
    }
    vx[0] = -mpx / SOLAR_MASS;
    vy[0] = -mpy / SOLAR_MASS;
    vz[0] = -mpz / SOLAR_MASS;

    double e0 = energy(n, mass, px, py, pz, vx, vy, vz);

    const int steps = 5000000;
    auto t0 = std::chrono::steady_clock::now();
    for (int s = 0; s < steps; ++s)
        advance(n, 0.01, mass, px, py, pz, vx, vy, vz);
    auto t1 = std::chrono::steady_clock::now();
    double ms = std::chrono::duration<double, std::milli>(t1 - t0).count();

    double e1 = energy(n, mass, px, py, pz, vx, vy, vz);
    std::printf("%.9f\n", e0);
    std::printf("verify %.9f\n", e1);
    std::printf("ms %.0f\n", ms);
    return 0;
}
