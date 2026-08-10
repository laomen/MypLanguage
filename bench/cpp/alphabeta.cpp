// bench/cpp/alphabeta.cpp — 极小极大 + α-β 剪枝（游戏树搜索，deepsjeng 类）
// B=6 分支 D=14 层；叶子值 = 路径确定性哈希（-1000..1000）。verify = 根值×10^6+节点数。
#include <chrono>
#include <cstdio>

static const int B = 6, D = 14;
static long long g_nodes;
static const int INF = 1000000000;

static int leafVal(int seed, int path) {
    long long h = seed;
    h = (h * 1103515245LL + (long long)path * 97 + 12345) % 2147483648LL;
    return (int)(h % 2001) - 1000;   // -1000..1000
}

static int ab(int depth, int alpha, int beta, int player, int seed, int path) {
    if (depth == D) { g_nodes++; return leafVal(seed, path); }
    int best = player ? -INF : INF;
    for (int m = 0; m < B; ++m) {
        int v = ab(depth + 1, alpha, beta, !player, seed, path * B + m);
        if (player) {
            if (v > best) best = v;
            if (best > alpha) alpha = best;
        } else {
            if (v < best) best = v;
            if (best < beta) beta = best;
        }
        if (beta <= alpha) break;   // 剪枝
    }
    return best;
}

static long alphabeta(int seed) {
    g_nodes = 0;
    int root = ab(0, -INF, INF, 1, seed, 0);
    return (long)root * 1000000L + g_nodes;
}

int main() {
    auto t0 = std::chrono::steady_clock::now();
    long v = alphabeta(12345);
    auto t1 = std::chrono::steady_clock::now();
    double ms = std::chrono::duration<double, std::milli>(t1 - t0).count();
    std::printf("verify %ld\nms %.0f\n", (long)v, ms);
    return 0;
}
