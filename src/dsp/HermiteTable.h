// HermiteTable.h -- uniform cubic-Hermite lookup of a smooth scalar function and its derivative.
#pragma once
#include <cmath>
#include <vector>
namespace cd57 {
struct HermiteTable {
    double x0 = 0, x1 = 0, h = 1, ih = 1;
    std::vector<double> f, d;   // value and derivative at the knots
    template <class Fn> void build(double a, double b, double step, Fn fn)  // fn(x, y&, dy&)
    {
        const int n = (int)std::ceil((b - a) / step) + 1;
        x0 = a; h = (b - a) / (n - 1); ih = 1.0 / h; x1 = a + h * (n - 1);
        f.resize(n); d.resize(n);
        for (int i = 0; i < n; ++i) fn(x0 + i * h, f[i], d[i]);
    }
    bool contains(double x) const { return x >= x0 && x < x1; }
    // caller guarantees contains(x)
    void eval(double x, double& y, double& dy) const
    {
        const double s = (x - x0) * ih;
        const int i = (int)s;
        const double t = s - i, t2 = t * t, t3 = t2 * t;
        const double y0 = f[i], y1 = f[i + 1], m0 = d[i] * h, m1 = d[i + 1] * h;
        const double h00 = 2 * t3 - 3 * t2 + 1, h10 = t3 - 2 * t2 + t, h01 = -2 * t3 + 3 * t2, h11 = t3 - t2;
        y = h00 * y0 + h10 * m0 + h01 * y1 + h11 * m1;
        const double g00 = 6 * t2 - 6 * t, g10 = 3 * t2 - 4 * t + 1, g01 = -6 * t2 + 6 * t, g11 = 3 * t2 - 2 * t;
        dy = (g00 * y0 + g10 * m0 + g01 * y1 + g11 * m1) * ih;
    }
};
}
