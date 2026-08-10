// bench/cpp/nqueens.cpp — N 皇后（对角数组 O(1) 冲突检查，递归）
// N=14 方案数 = 365596（确定性）；测：递归调用 + 分支密集。
#include <chrono>
#include <cstdio>
#include <vector>

static int g_count, gn;
static std::vector<int> gcol, gd1, gd2;
static void place(int row) {
    if (row == gn) { g_count++; return; }
    for (int c = 0; c < gn; ++c) {
        if (!gcol[c] && !gd1[row + c] && !gd2[row - c + gn - 1]) {
            gcol[c] = gd1[row + c] = gd2[row - c + gn - 1] = 1;
            place(row + 1);
            gcol[c] = gd1[row + c] = gd2[row - c + gn - 1] = 0;
        }
    }
}
static int nqueens(int n) {
    gn = n; g_count = 0;
    gcol.assign(n, 0);
    gd1.assign(2 * n - 1, 0);
    gd2.assign(2 * n - 1, 0);
    place(0);
    return g_count;
}

int main() {
    const int n = 14;
    auto t0 = std::chrono::steady_clock::now();
    int v = nqueens(n);
    auto t1 = std::chrono::steady_clock::now();
    double ms = std::chrono::duration<double, std::milli>(t1 - t0).count();
    std::printf("verify %d\nms %.0f\n", v, ms);
    return 0;
}
