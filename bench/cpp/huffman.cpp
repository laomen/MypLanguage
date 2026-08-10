// bench/cpp/huffman.cpp — Huffman 编码（字节计数 + 二叉最小堆建树 + 码长）
// 8MB 字节流（LCG）；verify = 总码长 ∑cnt[i]×depth[i] + 符号数×10^9。
#include <chrono>
#include <cstdio>
#include <vector>

static long huffman(int n, int seed) {
    std::vector<unsigned char> msg(n);
    long long rng = seed;
    for (int i = 0; i < n; ++i) {
        rng = (rng * 1103515245LL + 12345LL) % 2147483648LL;
        msg[i] = (unsigned char)((rng >> 16) & 0xFF);
    }
    int cnt[256] = {0};
    for (int i = 0; i < n; ++i) cnt[msg[i]]++;
    // 节点：freq / left / right
    int MAX = 512;
    std::vector<int> freq(MAX), left(MAX), right(MAX);
    int nid = 0;
    for (int i = 0; i < 256; ++i)
        if (cnt[i] > 0) { freq[nid] = cnt[i]; left[nid] = right[nid] = -1; nid++; }
    // 用无序池 + 每轮线性扫描取最小两个（完全确定、无堆实现细节差异）
    std::vector<int> pool;
    for (int i = 0; i < nid; ++i) pool.push_back(i);
    auto popMin = [&]() -> int {
        int bi = 0;
        for (int i = 1; i < (int)pool.size(); ++i)
            if (freq[pool[i]] < freq[pool[bi]] ||
                (freq[pool[i]] == freq[pool[bi]] && pool[i] < pool[bi])) bi = i;
        int v = pool[bi];
        pool[bi] = pool.back(); pool.pop_back();
        return v;
    };
    while (pool.size() > 1) {
        int a = popMin();
        int b = popMin();
        freq[nid] = freq[a] + freq[b];
        left[nid] = a; right[nid] = b;
        pool.push_back(nid);
        nid++;
    }
    int root = pool.empty() ? -1 : pool[0];
    // 深度（码长）统计
    int depth[512] = {0};
    int stack[512]; int stackDepth[512];
    int sp = 0;
    if (root >= 0) { stack[sp] = root; stackDepth[sp] = 0; sp++; }
    long sum = 0;
    int nsym = 0;
    while (sp > 0) {
        sp--;
        int node = stack[sp], dep = stackDepth[sp];
        if (left[node] < 0 && right[node] < 0) {
            depth[node] = dep;
            // node < 256 才是原始符号
            if (node < 256) { sum += (long)cnt[node] * dep; nsym++; }
        } else {
            stack[sp] = left[node]; stackDepth[sp] = dep + 1; sp++;
            stack[sp] = right[node]; stackDepth[sp] = dep + 1; sp++;
        }
    }
    return sum + (long)nsym * 1000000000L;
}

int main() {
    const int n = 8388608;   // 8MB
    auto t0 = std::chrono::steady_clock::now();
    long v = huffman(n, 98765);
    auto t1 = std::chrono::steady_clock::now();
    double ms = std::chrono::duration<double, std::milli>(t1 - t0).count();
    std::printf("verify %ld\nms %.0f\n", (long)v, ms);
    return 0;
}
