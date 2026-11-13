#pragma once
// Building blocks for a tube limiter's control loop (Vari-mu P5; generic), realtime safe, no allocation:
//
//   CubicTable<N,K> K smooth functions on one uniform grid, Catmull-Rom interpolation. For a gain cell's law
//                  built from the plugin's OWN model at prepare() (never from measurements of a reference: house
//                  rule 3), e.g. a tube's plate current and its derivatives over the grid voltage.
//   Detector       the sidechain's peak detector as a current source: full-wave rectifier, dead zone (the
//                  sidechain's bias plus the rectifier's drop), soft knee (softplus over `knee` volts), source
//                  resistance, soft current limit i / sqrt(1 + (i / iMax)^2). The charge current into a timing
//                  network at control voltage vc.
//   RcNetwork      the timing network: the main node R || C to ground, up to three series-RC branches to ground
//                  (switched per Time constant). Explicit Euler at the loop rate, in the same order as the offline
//                  model (lkit-style python models: branches first, then the main node), so a core can be checked
//                  against the model sample for sample.
//
// A gain cell is plugin-specific (its law, its bias network); it plugs in between: per loop sample the core asks
// the cell for its gain at the network's vc, runs the audio, and gives the output to Detector::current().

#include <algorithm>
#include <cmath>

namespace lkit::dyn {

template <int N, int K = 1>
class CubicTable {
    static_assert(N >= 4, "at least 4 points");

public:
    // f sampled at N points on [x0, x1]; outside the range the table holds its end values. K > 1: f(x, out) fills
    // K values per point (several functions of the same variable share one index computation).
    template <typename F>
    void build(double x0, double x1, F&& f)
    {
        x0_ = x0;
        x1_ = x1;
        inv_ = (N - 1) / (x1 - x0);
        for (int i = 0; i < N; ++i) {
            const double x = x0 + (x1 - x0) * i / (N - 1);
            if constexpr (K == 1) v_[i + 1][0] = f(x);
            else f(x, v_[i + 1]);
        }
        for (int k = 0; k < K; ++k) {   // linear extension for the end segments' outer neighbours
            v_[0][k] = 2.0 * v_[1][k] - v_[2][k];
            v_[N + 1][k] = 2.0 * v_[N][k] - v_[N - 1][k];
            v_[N + 2][k] = 2.0 * v_[N + 1][k] - v_[N][k];
        }
    }
    double operator()(double x) const noexcept
    {
        double y[K];
        eval(x, y);
        return y[0];
    }
    void eval(double x, double (&y)[K]) const noexcept
    {
        const double u = (x - x0_) * inv_;
        if (!(u > 0.0) || u >= N - 1) {
            const int e = !(u > 0.0) ? 1 : N;
            for (int k = 0; k < K; ++k) y[k] = v_[e][k];
            return;
        }
        const int i = static_cast<int>(u);
        const double t = u - i;
        // Catmull-Rom weights
        const double t2 = t * t, t3 = t2 * t;
        const double w0 = 0.5 * (-t3 + 2.0 * t2 - t), w1 = 0.5 * (3.0 * t3 - 5.0 * t2 + 2.0);
        const double w2 = 0.5 * (-3.0 * t3 + 4.0 * t2 + t), w3 = 0.5 * (t3 - t2);
        const double(*p)[K] = v_ + i;   // p[1] = f(x_i)
        for (int k = 0; k < K; ++k) y[k] = w0 * p[0][k] + w1 * p[1][k] + w2 * p[2][k] + w3 * p[3][k];
    }
    double lo() const noexcept { return x0_; }
    double hi() const noexcept { return x1_; }

private:
    double x0_ = 0.0, x1_ = 1.0, inv_ = 1.0;
    double v_[N + 3][K] = {};
};

class Detector {
public:
    // gain: volts at the rectifier per volt of signal; offset: dead zone (V); knee: softness (V), 0 = hard;
    // rSource (ohm); iMax (A), <= 0 = no limit.
    void configure(double gain, double offset, double knee, double rSource, double iMax)
    {
        gain_ = gain;
        offset_ = offset;
        rInv_ = 1.0 / rSource;
        iMaxInv_ = iMax > 0.0 ? 1.0 / iMax : 0.0;
        if (std::fabs(knee - knee_) > 0.0) {   // (the knee table is built once per knee)
            knee_ = knee;
            if (knee_ > 0.0) sp_.build(-kSpan, kSpan, [](double z) { return z > 0 ? z + std::log1p(std::exp(-z)) : std::log1p(std::exp(z)); });
        }
    }
    void setGain(double gain) noexcept { gain_ = gain; }
    double gain() const noexcept { return gain_; }

    // The rectifier's voltage after the dead zone and the knee, for a signal sample y.
    double voltage(double y) const noexcept
    {
        const double u = std::fabs(y) * gain_ - offset_;
        if (knee_ <= 0.0) return u > 0.0 ? u : 0.0;
        const double z = u / knee_;
        if (z >= kSpan) return u;
        if (z <= -kSpan) return 0.0;
        return knee_ * sp_(z);
    }
    // Charge current into a network at vc (0 when the rectifier does not conduct).
    double current(double y, double vc) const noexcept
    {
        const double u = std::fabs(y) * gain_ - offset_;
        // softplus(u) <= max(u, 0) + knee ln 2: most samples under gain reduction stop here, before the knee
        if ((u > 0.0 ? u : 0.0) + knee_ * 0.6931471805599453 <= vc) return 0.0;
        const double v = voltage(y);
        if (v <= vc) return 0.0;
        return currentFrom(v - vc);
    }
    // The charge current for a rectifier voltage dv > 0 above the network's (a caller that needs voltage() itself,
    // e.g. to find where conduction starts between two points).
    double currentFrom(double dv) const noexcept
    {
        const double i = dv * rInv_;
        if (iMaxInv_ == 0.0) return i;
        const double r = i * iMaxInv_;
        return i / std::sqrt(1.0 + r * r);
    }

private:
    static constexpr double kSpan = 40.0;   // softplus(z) = z (z > 40) or 0 (z < -40) to 1e-17 of the knee
    double gain_ = 1.0, offset_ = 0.0, knee_ = -1.0, rInv_ = 1.0, iMaxInv_ = 0.0;
    CubicTable<2049> sp_;
};

class RcNetwork {
public:
    static constexpr int kMaxBranches = 3;

    // Main node r || c; branches (series r, c) to ground; dt the loop's step. A new topology keeps the main
    // node's voltage; a branch that was not connected before starts at it (no step: the switch connects a
    // capacitor that takes the node's charge in the hardware within its own R C).
    void configure(double r, double c, const double* br, const double* bc, int nBranches, double dt) noexcept
    {
        const int old = nb_;
        nb_ = std::clamp(nBranches, 0, kMaxBranches);
        cm_ = dt / c;
        g_ = 1.0 / r;
        for (int k = 0; k < nb_; ++k) {
            gb_[k] = 1.0 / br[k];
            kb_[k] = dt / (bc[k] * br[k]);
            if (k >= old || !connected_) vb_[k] = vc_;
        }
        connected_ = true;
    }
    void reset(double vc = 0.0) noexcept
    {
        vc_ = vc;
        for (double& v : vb_) v = vc;
    }
    // One loop step with charge current i; returns the new control voltage.
    double step(double i) noexcept
    {
        double ib = 0.0;
        for (int k = 0; k < nb_; ++k) {
            const double d = vc_ - vb_[k];
            vb_[k] += kb_[k] * d;
            ib += gb_[k] * d;
        }
        vc_ += cm_ * (i - vc_ * g_ - ib);
        return vc_;
    }
    double vc() const noexcept { return vc_; }
    double branch(int k) const noexcept { return vb_[k]; }
    int branches() const noexcept { return nb_; }
    void flushTiny() noexcept
    {
        if (std::fabs(vc_) < 1e-30) vc_ = 0.0;
        for (double& v : vb_)
            if (std::fabs(v) < 1e-30) v = 0.0;
    }

private:
    double vc_ = 0.0, vb_[kMaxBranches] = {}, gb_[kMaxBranches] = {}, kb_[kMaxBranches] = {};
    double cm_ = 0.0, g_ = 0.0;
    int nb_ = 0;
    bool connected_ = false;
};

} // namespace lkit::dyn
