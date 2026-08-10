// bench/cpp/astar.cpp — A* 网格寻路（与 bench/myp/astar.myp 同算法）
// 高速公路网格（每 8 行/列强制空地）+ 20% 障碍（LCG 高位）+ 二叉最小堆。
// 输出：verify <扩展节点数> / ms <毫秒>
#include <chrono>
#include <cstdio>
#include <vector>

static int manhattan(int node, int goal, int n) {
    int x = node % n, y = node / n;
    int gx = goal % n, gy = goal / n;
    int dx = x - gx, dy = y - gy;
    if (dx < 0) dx = -dx;
    if (dy < 0) dy = -dy;
    return dx + dy;
}

static int astar(int n, int seed) {
    std::vector<int> grid(n * n);
    long long rng = seed;
    for (int i = 0; i < n * n; ++i) {
        rng = (rng * 1103515245LL + 12345LL) % 2147483648LL;
        int v = 0;
        int xx = i % n, yy = i / n;
        if (xx % 8 != 0 && yy % 8 != 0) {
            if ((rng >> 16) < 6554LL) v = 1;
        }
        grid[i] = v;
    }
    std::vector<int> heapNode(n * n * 2), heapF(n * n * 2);
    std::vector<int> g(n * n, 1 << 29), closed(n * n, 0);
    int heapSize = 0;
    int goal = n * n - 1, start = 0;
    g[start] = 0;
    heapNode[0] = start;
    heapF[0] = manhattan(start, goal, n);
    heapSize = 1;
    int expansions = 0;
    const int ddx[4] = {0, 0, -1, 1};
    const int ddy[4] = {-1, 1, 0, 0};
    while (heapSize > 0) {
        int cur = heapNode[0];
        --heapSize;
        heapNode[0] = heapNode[heapSize];
        heapF[0] = heapF[heapSize];
        int idx = 0;
        for (;;) {
            int l = idx * 2 + 1, r = idx * 2 + 2, smallest = idx;
            if (l < heapSize) {
                if (heapF[l] < heapF[smallest] ||
                    (heapF[l] == heapF[smallest] && heapNode[l] < heapNode[smallest]))
                    smallest = l;
            }
            if (r < heapSize) {
                if (heapF[r] < heapF[smallest] ||
                    (heapF[r] == heapF[smallest] && heapNode[r] < heapNode[smallest]))
                    smallest = r;
            }
            if (smallest == idx) break;
            std::swap(heapNode[idx], heapNode[smallest]);
            std::swap(heapF[idx], heapF[smallest]);
            idx = smallest;
        }
        if (closed[cur]) continue;
        closed[cur] = 1;
        ++expansions;
        if (cur == goal) break;
        int cx = cur % n, cy = cur / n;
        for (int d = 0; d < 4; ++d) {
            int nx = cx + ddx[d], ny = cy + ddy[d];
            if (nx >= 0 && nx < n && ny >= 0 && ny < n) {
                int nb = ny * n + nx;
                if (grid[nb] == 0) {
                    int tentative = g[cur] + 1;
                    if (tentative < g[nb]) {
                        g[nb] = tentative;
                        int f = tentative + manhattan(nb, goal, n);
                        heapNode[heapSize] = nb;
                        heapF[heapSize] = f;
                        int pos = heapSize++;
                        while (pos > 0) {
                            int par = (pos - 1) / 2;
                            if (heapF[pos] < heapF[par] ||
                                (heapF[pos] == heapF[par] && heapNode[pos] < heapNode[par])) {
                                std::swap(heapNode[pos], heapNode[par]);
                                std::swap(heapF[pos], heapF[par]);
                                pos = par;
                            } else break;
                        }
                    }
                }
            }
        }
    }
    return expansions;
}

int main() {
    const int n = 512;
    auto t0 = std::chrono::steady_clock::now();
    int exp = astar(n, 987654);
    auto t1 = std::chrono::steady_clock::now();
    double ms = std::chrono::duration<double, std::milli>(t1 - t0).count();
    std::printf("verify %d\nms %.0f\n", exp, ms);
    return 0;
}
