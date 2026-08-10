// bench/cpp/gol.cpp — Game of Life（与 bench/myp/gol.myp 同算法同 LCG）
// 输出：verify <活细胞总数> / ms <毫秒>
#include <chrono>
#include <cstdio>
#include <vector>

static int gol(int n, int gens, int seed) {
    std::vector<int> a(n * n), b(n * n);
    long long rng = seed;
    for (int i = 0; i < n * n; ++i) {
        rng = (rng * 1103515245LL + 12345LL) % 2147483648LL;
        a[i] = ((rng >> 16) < 9830LL) ? 1 : 0;   // LCG 高位（与 MYP 一致）
    }
    for (int g = 0; g < gens; ++g) {
        const int* cur = a.data();
        int* nxt = b.data();
        for (int y = 0; y < n; ++y)
            for (int x = 0; x < n; ++x) {
                int live = 0;
                for (int dy = -1; dy <= 1; ++dy)
                    for (int dx = -1; dx <= 1; ++dx) {
                        if (dx == 0 && dy == 0) continue;
                        int nx = x + dx, ny = y + dy;
                        if (nx < 0) nx += n;
                        if (nx >= n) nx -= n;
                        if (ny < 0) ny += n;
                        if (ny >= n) ny -= n;
                        live += cur[ny * n + nx];
                    }
                int val = 0;
                if (cur[y * n + x]) { if (live == 2 || live == 3) val = 1; }
                else { if (live == 3) val = 1; }
                nxt[y * n + x] = val;
            }
        std::swap(a, b);
    }
    int total = 0;
    for (int i = 0; i < n * n; ++i) total += a[i];
    return total;
}

int main() {
    const int n = 1024, gens = 60;
    auto t0 = std::chrono::steady_clock::now();
    int c = gol(n, gens, 12345);
    auto t1 = std::chrono::steady_clock::now();
    double ms = std::chrono::duration<double, std::milli>(t1 - t0).count();
    std::printf("verify %d\nms %.0f\n", c, ms);
    return 0;
}
