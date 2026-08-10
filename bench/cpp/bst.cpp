// bench/cpp/bst.cpp — 二叉搜索树（数组节点 left/right/key，指针追逐）
// 262144 个随机键（LCG>>1, 0..2^30-1）插入 BST，中序遍历求和 + 有序性检查。
// verify = 键和 + 违序数×10^9（BST 正确时违序=0）。
#include <chrono>
#include <cstdio>
#include <vector>

static long bst(int n, int seed) {
    std::vector<int> key(n), left(n, -1), right(n, -1), stack(n);
    long long rng = seed;
    int root = -1;
    for (int i = 0; i < n; ++i) {
        rng = (rng * 1103515245LL + 12345LL) % 2147483648LL;
        int k = (int)(rng >> 1);
        key[i] = k;
        if (root < 0) { root = i; }
        else {
            int cur = root;
            for (;;) {
                if (k <= key[cur]) {
                    if (left[cur] < 0) { left[cur] = i; break; }
                    cur = left[cur];
                } else {
                    if (right[cur] < 0) { right[cur] = i; break; }
                    cur = right[cur];
                }
            }
        }
    }
    long sum = 0;
    int violations = 0, sp = 0, cur = root;
    bool havePrev = false;
    int prev = -1;
    while (cur >= 0 || sp > 0) {
        while (cur >= 0) { stack[sp++] = cur; cur = left[cur]; }
        cur = stack[--sp];
        if (havePrev && key[prev] > key[cur]) violations++;
        prev = cur; havePrev = true;
        sum += key[cur];
        cur = right[cur];
    }
    return sum + (long)violations * 1000000000L;
}

int main() {
    const int n = 262144;
    auto t0 = std::chrono::steady_clock::now();
    long v = bst(n, 98765);
    auto t1 = std::chrono::steady_clock::now();
    double ms = std::chrono::duration<double, std::milli>(t1 - t0).count();
    std::printf("verify %ld\nms %.0f\n", (long)v, ms);
    return 0;
}
