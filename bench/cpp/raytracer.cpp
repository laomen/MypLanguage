// bench/cpp/raytracer.cpp — C++ 版光线追踪（与 bench/myp/raytracer.myp 同算法）
// 同场景/同材质/同软阴影采样（确定性 LCG）/同 800×600 / 2×2 AA / depth=3。
// 输出：verify <像素通道和> / ms <毫秒>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <vector>

struct Vec3 { double x, y, z; };
static inline Vec3 va(Vec3 a, Vec3 b) { return {a.x + b.x, a.y + b.y, a.z + b.z}; }
static inline Vec3 vs(Vec3 a, Vec3 b) { return {a.x - b.x, a.y - b.y, a.z - b.z}; }
static inline Vec3 vm(Vec3 a, double s) { return {a.x * s, a.y * s, a.z * s}; }
static inline double vd(Vec3 a, Vec3 b) { return a.x * b.x + a.y * b.y + a.z * b.z; }
static inline Vec3 vc(Vec3 a, Vec3 b) {
    return {a.y * b.z - a.z * b.y, a.z * b.x - a.x * b.z, a.x * b.y - a.y * b.x};
}
static inline Vec3 vn(Vec3 a) {
    double l = std::sqrt(vd(a, a));
    if (l > 1e-12) return {a.x / l, a.y / l, a.z / l};
    return {0, 0, 0};
}
static inline Vec3 vr(Vec3 d, Vec3 n) { return vs(d, vm(n, 2 * vd(d, n))); }
static inline double vl(Vec3 a) { return std::sqrt(vd(a, a)); }
static inline Vec3 mk(double x, double y, double z) { return {x, y, z}; }
// 与 MYP Math.pi() 完全一致的字面量
static const double PI = 3.141592653589793;

struct Hit { double t; int idx; };

static int W = 800, H = 600, N;
static std::vector<double> cx, cy, cz, rad, kr, kg, kb;
static std::vector<int> mat;
static double lightX, lightY, lightZ;
static long gSeed;

static long lcg(long x) { return (x * 1103515245L + 12345L) % 2147483648L; }
static Vec3 cen(int i) { return mk(cx[i], cy[i], cz[i]); }

static Hit sceneHit(Vec3 ro, Vec3 rd) {
    Hit best{-1.0, -1};
    double bestT = 1e30;
    for (int i = 0; i < N; ++i) {
        Vec3 oc = vs(ro, cen(i));
        double a = vd(rd, rd);
        double b = 2.0 * vd(oc, rd);
        double c = vd(oc, oc) - rad[i] * rad[i];
        double disc = b * b - 4.0 * a * c;
        if (disc > 0.0) {
            double sq = std::sqrt(disc);
            double t0 = (-b - sq) / (2.0 * a);
            double t1 = (-b + sq) / (2.0 * a);
            if (t0 > 1e-4 && t0 < bestT) { bestT = t0; best.idx = i; }
            else if (t1 > 1e-4 && t1 < bestT) { bestT = t1; best.idx = i; }
        }
    }
    if (rd.y < -1e-6) {
        double tp = -ro.y / rd.y;
        if (tp > 1e-4 && tp < bestT) { best.t = tp; best.idx = -2; return best; }
    }
    if (best.idx >= 0) best.t = bestT;
    return best;
}

static double sphereFarT(Vec3 ro, Vec3 rd, int idx) {
    Vec3 oc = vs(ro, cen(idx));
    double a = vd(rd, rd);
    double b = 2.0 * vd(oc, rd);
    double c = vd(oc, oc) - rad[idx] * rad[idx];
    double disc = b * b - 4.0 * a * c;
    if (disc <= 0.0) return -1.0;
    double t1 = (-b + std::sqrt(disc)) / (2.0 * a);
    if (t1 > 1e-4) return t1;
    return -1.0;
}

static Vec3 refract(Vec3 d, Vec3 n, double eta, double cosi) {
    double k = 1.0 - eta * eta * (1.0 - cosi * cosi);
    if (k < 0.0) return {0, 0, 0};
    double s = eta * cosi - std::sqrt(k);
    return {eta * d.x + s * n.x, eta * d.y + s * n.y, eta * d.z + s * n.z};
}

static Vec3 perp(Vec3 a) {
    Vec3 tmp;
    if (std::fabs(a.x) < 0.9) tmp = mk(1.0, 0.0, 0.0);
    else tmp = mk(0.0, 1.0, 0.0);
    return vn(vc(a, tmp));
}

static double softShadow(Vec3 p2, Vec3 L) {
    Vec3 u = perp(L);
    Vec3 v = vc(L, u);
    long rng = gSeed;
    double occl = 0.0;
    for (int s = 0; s < 4; ++s) {
        rng = lcg(rng);
        double a1 = (rng % 100000L) / 100000.0;
        rng = lcg(rng);
        double a2 = (rng % 100000L) / 100000.0;
        double ang = a1 * 2.0 * PI;
        double rr = std::sqrt(a2) * 0.35;
        Vec3 target = va(mk(lightX, lightY, lightZ),
                         va(vm(u, std::cos(ang) * rr), vm(v, std::sin(ang) * rr)));
        Vec3 sd = vn(vs(target, p2));
        double sld = vl(vs(target, p2));
        Hit sh = sceneHit(p2, sd);
        if (sh.idx >= 0 && sh.t < sld) occl += 1.0;
    }
    return 1.0 - occl / 4.0;
}

static Vec3 shade(Vec3 ro, Vec3 rd, Vec3 p, Vec3 n, int idx, int depth);

static Vec3 trace(Vec3 ro, Vec3 rd, int depth) {
    Hit h = sceneHit(ro, rd);
    if (h.idx == -2) {
        Vec3 p{ro.x + rd.x * h.t, 0.0, ro.z + rd.z * h.t};
        int ix = (int)std::floor(p.x);
        int iz = (int)std::floor(p.z);
        double checker = 1.0;
        if (((ix + iz) % 2) != 0) checker = 0.72;
        Vec3 n = mk(0.0, 1.0, 0.0);
        Vec3 p2 = va(p, vm(n, 1e-3));
        Vec3 lv = vs(mk(lightX, lightY, lightZ), p2);
        Vec3 L = vn(lv);
        double diff = vd(n, L);
        if (diff < 0.0) diff = 0.0;
        double shad = softShadow(p2, L);
        double k = 0.13 + 0.87 * diff * shad;
        return {0.55 * checker * k, 0.55 * checker * k, 0.55 * checker * k};
    }
    if (h.idx >= 0) {
        Vec3 p = va(ro, vm(rd, h.t));
        Vec3 n = vn(vs(p, cen(h.idx)));
        return shade(ro, rd, p, n, h.idx, depth);
    }
    double t = 0.5 * (rd.y + 1.0);
    return {0.25 + 0.35 * t, 0.35 + 0.35 * t, 0.55 + 0.25 * t};
}

static Vec3 shadeGlass(Vec3 ro, Vec3 rd, Vec3 p, Vec3 n, int idx, int depth);

static Vec3 shade(Vec3 ro, Vec3 rd, Vec3 p, Vec3 n, int idx, int depth) {
    if (mat[idx] == 2) return shadeGlass(ro, rd, p, n, idx, depth);
    double cr = kr[idx], cg = kg[idx], cb = kb[idx];
    Vec3 p2 = va(p, vm(n, 1e-3));
    Vec3 lv = vs(mk(lightX, lightY, lightZ), p2);
    Vec3 L = vn(lv);
    double diff = vd(n, L);
    if (diff < 0.0) diff = 0.0;
    double shad = softShadow(p2, L);
    Vec3 V = vm(rd, -1.0);
    Vec3 H = vn(va(L, V));
    double spec = vd(n, H);
    if (spec < 0.0) spec = 0.0;
    spec = std::pow(spec, 32.0);
    double ir = cr * (0.15 + 0.85 * diff * shad) + 0.9 * spec * shad;
    double ig = cg * (0.15 + 0.85 * diff * shad) + 0.9 * spec * shad;
    double ib = cb * (0.15 + 0.85 * diff * shad) + 0.9 * spec * shad;
    if (mat[idx] == 1 && depth > 0) {
        Vec3 rc = trace(p2, vr(rd, n), depth - 1);
        ir = ir * 0.35 + rc.x * 0.65;
        ig = ig * 0.35 + rc.y * 0.65;
        ib = ib * 0.35 + rc.z * 0.65;
    }
    return {ir, ig, ib};
}

static Vec3 shadeGlass(Vec3 ro, Vec3 rd, Vec3 p, Vec3 n, int idx, int depth) {
    if (depth <= 0) return {0, 0, 0};
    double cosi = -vd(rd, n);
    if (cosi < 0.0) cosi = 0.0;
    double r0 = (1.0 - 1.5) / (1.0 + 1.5);
    r0 = r0 * r0;
    double fr = r0 + (1.0 - r0) * std::pow(1.0 - cosi, 5.0);
    Vec3 pp = va(p, vm(n, 1e-3));
    Vec3 cref = trace(pp, vr(rd, n), depth - 1);
    Vec3 T = refract(rd, n, 1.0 / 1.5, cosi);
    if (T.x == 0.0 && T.y == 0.0 && T.z == 0.0) return cref;
    double tf = sphereFarT(p, T, idx);
    if (tf < 1e-4) return cref;
    Vec3 xp = va(p, vm(T, tf));
    Vec3 n2 = vn(vs(xp, cen(idx)));
    double cosi2 = -vd(T, n2);
    if (cosi2 < 0.0) cosi2 = 0.0;
    Vec3 T2 = refract(T, n2, 1.5, cosi2);
    Vec3 ctrans;
    if (T2.x == 0.0 && T2.y == 0.0 && T2.z == 0.0)
        ctrans = trace(va(xp, vm(n2, 1e-3)), vr(T, n2), depth - 1);
    else
        ctrans = trace(va(xp, vm(n2, 1e-3)), T2, depth - 1);
    return {fr * cref.x + (1.0 - fr) * ctrans.x,
            fr * cref.y + (1.0 - fr) * ctrans.y,
            fr * cref.z + (1.0 - fr) * ctrans.z};
}

static int clampByte(double v) {
    if (v < 0.0) v = 0.0;
    if (v > 1.0) v = 1.0;
    return (int)(v * 255.0);
}

int main() {
    W = 800; H = 600;
    N = 6;
    cx.resize(N); cy.resize(N); cz.resize(N); rad.resize(N);
    kr.resize(N); kg.resize(N); kb.resize(N); mat.resize(N);
    cx[0] = 0.0;  cy[0] = 1.0;  cz[0] = 0.0;  rad[0] = 1.0;
    kr[0] = 0.85; kg[0] = 0.20; kb[0] = 0.20; mat[0] = 1;
    cx[1] = -2.2; cy[1] = 0.7;  cz[1] = -0.5; rad[1] = 0.7;
    kr[1] = 0.20; kg[1] = 0.80; kb[1] = 0.20; mat[1] = 0;
    cx[2] = 2.2;  cy[2] = 0.7;  cz[2] = 0.3;  rad[2] = 0.7;
    kr[2] = 0.20; kg[2] = 0.30; kb[2] = 0.85; mat[2] = 0;
    cx[3] = 0.9;  cy[3] = 0.4;  cz[3] = 1.1;  rad[3] = 0.4;
    kr[3] = 0.9;  kg[3] = 0.9;  kb[3] = 0.9;  mat[3] = 2;
    cx[4] = -1.0; cy[4] = 0.4;  cz[4] = -1.6; rad[4] = 0.45;
    kr[4] = 0.70; kg[4] = 0.30; kb[4] = 0.75; mat[4] = 0;
    cx[5] = -0.5; cy[5] = 2.1;  cz[5] = -0.3; rad[5] = 0.5;
    kr[5] = 0.90; kg[5] = 0.80; kb[5] = 0.15; mat[5] = 1;
    lightX = 2.5; lightY = 5.0; lightZ = -2.5;

    double eyeX = 0.0, eyeY = 1.3, eyeZ = 4.2;
    Vec3 target = mk(0.0, 1.0, 0.0);
    Vec3 up = mk(0.0, 1.0, 0.0);
    Vec3 fwd = vn(vs(target, mk(eyeX, eyeY, eyeZ)));
    Vec3 right = vn(vc(fwd, up));
    Vec3 upv = vc(right, fwd);
    double tanHalf = std::tan(30.0 * PI / 180.0);
    double aspect = (W * 1.0) / H;

    auto t0 = std::chrono::steady_clock::now();
    long verify = 0;
    for (int y = 0; y < H; ++y) {
        for (int x = 0; x < W; ++x) {
            double r = 0, g = 0, b = 0;
            for (int sx = 0; sx < 2; ++sx)
                for (int sy = 0; sy < 2; ++sy) {
                    double fx = (x + (sx + 0.5) / 2.0) / W;
                    double fy = (y + (sy + 0.5) / 2.0) / H;
                    double ndcX = (2.0 * fx - 1.0) * tanHalf * aspect;
                    double ndcY = (1.0 - 2.0 * fy) * tanHalf;
                    gSeed = ((y * 7919L + x * 104729L) * 2L) + (sx * 2 + sy);
                    Vec3 dir = vn(va(va(fwd, vm(right, ndcX)), vm(upv, ndcY)));
                    Vec3 c = trace(mk(eyeX, eyeY, eyeZ), dir, 3);
                    r += c.x; g += c.y; b += c.z;
                }
            r /= 4.0; g /= 4.0; b /= 4.0;
            verify += clampByte(r) + clampByte(g) + clampByte(b);
        }
    }
    auto t1 = std::chrono::steady_clock::now();
    double ms = std::chrono::duration<double, std::milli>(t1 - t0).count();
    std::printf("verify %ld\nms %.0f\n", verify, ms);
    return 0;
}
