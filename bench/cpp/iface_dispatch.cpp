// bench/cpp/iface_dispatch.cpp — 接口热循环分派基准（虚拟分派对照）
// 与 MYP 端同算法：Shape 接口 3 实现，热循环经基类指针调用 area()/perim()。
// clang++ -O3 会把 `new Circle` 的直接虚调用 devirtualize 掉并内联 → 代表"devirt 后"理想性能。
#include <chrono>
#include <cstdio>

struct Shape {
    virtual ~Shape() {}
    virtual double area() const = 0;
    virtual void grow(double f) = 0;
};

struct Circle : Shape {
    double r;
    explicit Circle(double r0) : r(r0) {}
    double area() const override { return 3.14159265358979 * r * r; }
    void grow(double f) override { r = r * f; }
};

struct Rect : Shape {
    double w, h;
    Rect(double w0, double h0) : w(w0), h(h0) {}
    double area() const override { return w * h; }
    void grow(double f) override { w = w * f; h = h * f; }
};

struct Tri : Shape {
    double b, h;
    Tri(double b0, double h0) : b(b0), h(h0) {}
    double area() const override { return 0.5 * b * h; }
    void grow(double f) override { b = b * f; h = h * f; }
};

static double ifaceDispatch(int iters) {
    // 形态 (a)：单形态基类指针
    Shape* s = new Circle(1.0);
    double acc = 0.0;
    for (int i = 0; i < iters; ++i) {
        s->grow(1.000001);
        acc += s->area();
    }
    // 形态 (b)：混合多态（三个基类指针轮流）
    Shape* s1 = new Circle(1.5);
    Shape* s2 = new Rect(2.0, 3.0);
    Shape* s3 = new Tri(4.0, 5.0);
    for (int i = 0; i < iters; ++i) {
        s1->grow(1.000001);
        s2->grow(1.000001);
        s3->grow(1.000001);
        acc += s1->area() + s2->area() + s3->area();
    }
    delete s; delete s1; delete s2; delete s3;
    return acc;
}

int main() {
    const int iters = 10000000;
    auto t0 = std::chrono::steady_clock::now();
    double v = ifaceDispatch(iters);
    auto t1 = std::chrono::steady_clock::now();
    double ms = std::chrono::duration<double, std::milli>(t1 - t0).count();
    std::printf("verify %.6f\nms %.0f\n", v, ms);
    return 0;
}
