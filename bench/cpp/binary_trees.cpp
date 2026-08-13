// bench/cpp/binary_trees.cpp — binary-trees（数组树 + 递归 item_check），与 MYP 同算法
#include <chrono>
#include <cstdio>
#include <vector>
using namespace std;

static void buildTree(vector<int>& tree, int d, int pos, int val) {
    tree[pos] = val;
    if (d > 0) {
        buildTree(tree, d - 1, 2 * pos + 1, val - 1);
        buildTree(tree, d - 1, 2 * pos + 2, val - 1);
    }
}

static int itemCheck(const vector<int>& tree, int pos, int size) {
    if (2 * pos + 2 >= size) return tree[pos];
    return tree[pos] + itemCheck(tree, 2 * pos + 1, size) + itemCheck(tree, 2 * pos + 2, size);
}

static long binTrees(int maxDepth) {
    int size = (1 << (maxDepth + 1)) - 1;
    vector<int> tree(size);
    long check = 0;
    for (int d = 4; d <= maxDepth; d += 2) {
        buildTree(tree, d, 0, d);
        check += itemCheck(tree, 0, size);
    }
    return check;
}

int main() {
    const int maxDepth = 17;
    auto t0 = chrono::steady_clock::now();
    long v = binTrees(maxDepth);
    auto t1 = chrono::steady_clock::now();
    double ms = chrono::duration<double, milli>(t1 - t0).count();
    printf("verify %ld\nms %.0f\n", v, ms);
    return 0;
}
