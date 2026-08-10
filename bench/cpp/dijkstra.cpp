// bench/cpp/dijkstra.cpp — 稠密图单源最短路（O(V²)，无堆）
// N=4096 邻接矩阵，权重 1..1000（LCG 高位）。verify = dist 总和 + dist[N-1]×10^6。
#include <chrono>
#include <cstdio>
#include <vector>

static long dijkstra(int n, int seed) {
    std::vector<int> w(n * n);
    long long rng = seed;
    for (int i = 0; i < n * n; ++i) {
        rng = (rng * 1103515245LL + 12345LL) % 2147483648LL;
        w[i] = (int)((rng >> 16) % 1000) + 1;
    }
    std::vector<int> dist(n), visited(n, 0);
    const int INF = 1 << 29;
    for (int i = 0; i < n; ++i) dist[i] = INF;
    dist[0] = 0;
    for (int iter = 0; iter < n; ++iter) {
        int u = -1;
        for (int i = 0; i < n; ++i)
            if (!visited[i] && (u < 0 || dist[i] < dist[u])) u = i;
        if (u < 0 || dist[u] >= INF) break;
        visited[u] = 1;
        for (int v = 0; v < n; ++v) {
            if (!visited[v]) {
                int nd = dist[u] + w[u * n + v];
                if (nd < dist[v]) dist[v] = nd;
            }
        }
    }
    long sum = 0;
    for (int i = 0; i < n; ++i) sum += dist[i];
    return sum + (long)dist[n - 1] * 1000000L;
}

int main() {
    const int n = 4096;
    auto t0 = std::chrono::steady_clock::now();
    long v = dijkstra(n, 13579);
    auto t1 = std::chrono::steady_clock::now();
    double ms = std::chrono::duration<double, std::milli>(t1 - t0).count();
    std::printf("verify %ld\nms %.0f\n", (long)v, ms);
    return 0;
}
