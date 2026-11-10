#pragma once
// Bilinear transform of a 2nd-order analog section and a TDF-II biquad (double).

#include <cmath>

namespace lkit {

struct Biquad {
    double b0 = 1, b1 = 0, b2 = 0, a1 = 0, a2 = 0;
    double z1 = 0, z2 = 0;

    // H(s) = (B2 s^2 + B1 s + B0) / (A2 s^2 + A1 s + A0), s -> k (z - 1)/(z + 1).
    void setAnalog(double B2, double B1, double B0, double A2, double A1, double A0, double k) noexcept
    {
        const double kk = k * k;
        const double n0 = B2 * kk + B1 * k + B0, n1 = 2 * (B0 - B2 * kk), n2 = B2 * kk - B1 * k + B0;
        const double d0 = A2 * kk + A1 * k + A0, d1 = 2 * (A0 - A2 * kk), d2 = A2 * kk - A1 * k + A0;
        b0 = n0 / d0, b1 = n1 / d0, b2 = n2 / d0, a1 = d1 / d0, a2 = d2 / d0;
    }
    // 2nd-order high-pass / low-pass (corner f0, quality q) at sample rate fs, prewarped at
    // fWarp (the frequency where the digital response equals the analog one exactly). Use an
    // in-band frequency: a corner far above the audio band must not be the warp point.
    static double warpK(double fWarp, double fs) noexcept
    {
        const double w = 2 * 3.14159265358979323846 * fWarp;
        return w / std::tan(w / (2 * fs));
    }
    void setHighpass(double f0, double q, double fs, double fWarp) noexcept
    {
        const double w = 2 * 3.14159265358979323846 * f0;
        setAnalog(1, 0, 0, 1, w / q, w * w, warpK(fWarp, fs));
    }
    void setLowpass(double f0, double q, double fs, double fWarp) noexcept
    {
        const double w = 2 * 3.14159265358979323846 * f0;
        setAnalog(0, 0, w * w, 1, w / q, w * w, warpK(fWarp, fs));
    }
    double tick(double x) noexcept
    {
        const double y = b0 * x + z1;
        z1 = b1 * x - a1 * y + z2;
        z2 = b2 * x - a2 * y;
        return y;
    }
    void reset() noexcept { z1 = z2 = 0; }
    void flushTiny() noexcept
    {
        if (std::fabs(z1) < 1e-30) z1 = 0;
        if (std::fabs(z2) < 1e-30) z2 = 0;
    }
};

} // namespace lkit
