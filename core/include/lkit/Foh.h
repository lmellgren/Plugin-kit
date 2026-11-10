#pragma once
// Exact-pole state-space filter with physical states (the LOMAS Passive EQ M7b network engine,
// generic in the number of states N).
//
// A plugin supplies the circuit's continuous state space with physical states (capacitor voltages,
// inductor currents): dx/dt = A x + B u, y = C x + D u. FohDesigner discretises it with exact poles by
// a first-order hold (ramp-invariant):
//   x[n+1] = Phi x[n] + (G0 - G1) u[n-1] + G1 u[n],   y[n] = C x[n] + D u[n-1]
// with Phi = exp(A T) and G0, G1 from one augmented matrix exponential. C and D stay the circuit's own
// output map. A short output FIR (<= kMaxFir taps), fitted by least squares to
// target(f) / (first-order-hold response) over the audio band, supplies whatever the plugin puts in the
// target besides the network itself (make-up gain, a transformer low-pass, a fractional delay).
// Changing the FIR only rescales the output: the states keep their meaning and coefficient updates
// do not cause transients. Double precision, no allocation, realtime safe.

#include "lkit/LinAlg.h"
#include "lkit/Simd.h"

#include <array>
#include <cmath>
#include <complex>

namespace lkit {

template <int N>
struct StateSpace {
    double A[N][N] = {}, B[N] = {}, C[N] = {}, D = 0.0;

    // Analog response C (jw - A)^-1 B + D at one frequency (Hz).
    std::complex<double> response(double freqHz) const noexcept
    {
        using cd = std::complex<double>;
        cd M[N][N], R[N][1];
        const cd jw(0.0, 2.0 * 3.14159265358979323846 * freqHz);
        for (int i = 0; i < N; ++i) {
            for (int j = 0; j < N; ++j) M[i][j] = (i == j ? jw : cd(0.0)) - A[i][j];
            R[i][0] = B[i];
        }
        if (!linalg::luSolve<N, 1, cd>(M, R)) return {0.0, 0.0};
        cd h = D;
        for (int i = 0; i < N; ++i) h += C[i] * R[i][0];
        return h;
    }
};

inline constexpr int kMaxFir = 8;

template <int N>
struct FohCoeffs {
    double Phi[N][N] = {}, G0m[N] = {}, G1[N] = {}, C[N] = {}, D = 0.0;
    double fir[kMaxFir] = {1.0};
    int nFir = 1;

    // Response of the state-space part alone at z^-1 = z1:
    // C (zI - Phi)^-1 ((G0 - G1) z^-1 + G1) + D z^-1
    std::complex<double> stateResponse(std::complex<double> z1) const noexcept
    {
        using cd = std::complex<double>;
        const cd z = 1.0 / z1;
        cd M[N][N], R[N][1];
        for (int r = 0; r < N; ++r) {
            for (int k = 0; k < N; ++k) M[r][k] = (r == k ? z : cd(0.0)) - Phi[r][k];
            R[r][0] = G0m[r] * z1 + G1[r];
        }
        if (!linalg::luSolve<N, 1, cd>(M, R)) return {0.0, 0.0};
        cd h = D * z1;
        for (int i = 0; i < N; ++i) h += C[i] * R[i][0];
        return h;
    }
    std::complex<double> firResponse(std::complex<double> z1) const noexcept
    {
        std::complex<double> h = 0.0, zk = 1.0;
        for (int k = 0; k < nFir; ++k, zk *= z1) h += fir[k] * zk;
        return h;
    }
    std::complex<double> response(std::complex<double> z1) const noexcept { return stateResponse(z1) * firResponse(z1); }
    std::complex<double> responseAt(double freqHz, double fs) const noexcept
    {
        return response(std::exp(std::complex<double>(0.0, -2.0 * 3.14159265358979323846 * freqHz / fs)));
    }
};

template <int N>
class FohDesigner {
public:
    static constexpr int kBand = 32;
    static constexpr int kRows = 2 * kBand;

    // fs: filter rate; firTaps: 1..kMaxFir; loHz..topHz: band of the FIR fit (log spaced, kBand points).
    void prepare(double fs, int firTaps, double loHz = 20.0, double topHz = 20000.0) noexcept
    {
        fs_ = fs;
        nFir_ = std::clamp(firTaps, 1, kMaxFir);
        for (int i = 0; i < kBand; ++i) f_[static_cast<size_t>(i)] = loHz * std::pow(topHz / loHz, i / double(kBand - 1));
    }
    double sampleRate() const noexcept { return fs_; }

    // State part only (FIR = 1).
    bool discretise(const StateSpace<N>& ss, FohCoeffs<N>& out) const noexcept
    {
        // exp([[A T, B T, 0], [0, 0, 1], [0, 0, 0]]) = [[Phi, G0, G1], ...]
        constexpr int M = N + 2;
        const double T = 1.0 / fs_;
        double Mx[M][M] = {}, E[M][M];
        for (int i = 0; i < N; ++i) {
            for (int j = 0; j < N; ++j) Mx[i][j] = ss.A[i][j] * T;
            Mx[i][N] = ss.B[i] * T;
        }
        Mx[N][N + 1] = 1.0;
        if (!linalg::expm<M>(Mx, E)) return false;
        for (int i = 0; i < N; ++i) {
            for (int j = 0; j < N; ++j) out.Phi[i][j] = E[i][j];
            out.G0m[i] = E[i][N] - E[i][N + 1];
            out.G1[i] = E[i][N + 1];
            out.C[i] = ss.C[i];
        }
        out.D = ss.D;
        out.nFir = 1;
        for (int k = 0; k < kMaxFir; ++k) out.fir[k] = k == 0 ? 1.0 : 0.0;
        return true;
    }

    // Discretise and fit the output FIR so that the whole filter matches target(f) (a callable
    // returning std::complex<double>, typically ss.response(f) * extras(f) * exp(-j w delay)).
    template <typename Target>
    bool design(const StateSpace<N>& ss, Target&& target, FohCoeffs<N>& out) const noexcept
    {
        using cd = std::complex<double>;
        if (!discretise(ss, out)) return false;
        double A[kRows][kMaxFir] = {}, b[kRows] = {}, x[kMaxFir];
        for (int i = 0; i < kBand; ++i) {
            const double f = f_[static_cast<size_t>(i)];
            const cd z1 = std::exp(cd(0.0, -2.0 * 3.14159265358979323846 * f / fs_));
            const cd r = cd(target(f)) / out.stateResponse(z1);
            cd zk = 1.0;
            for (int k = 0; k < nFir_; ++k, zk *= z1) {
                A[2 * i][k] = zk.real();
                A[2 * i + 1][k] = zk.imag();
            }
            b[2 * i] = r.real();
            b[2 * i + 1] = r.imag();
        }
        if (!linalg::lstsq<kRows, kMaxFir>(A, b, kRows, nFir_, x)) return false;
        out.nFir = nFir_;
        for (int k = 0; k < kMaxFir; ++k) out.fir[k] = k < nFir_ ? x[k] : 0.0;
        return true;
    }

private:
    double fs_ = 96000.0;
    int nFir_ = 6;
    std::array<double, kBand> f_{};
};

// Stereo (2-lane) realtime filter.
template <int N>
class FohFilter {
public:
    void setCoefficients(const FohCoeffs<N>& c) noexcept { c_ = c; }
    const FohCoeffs<N>& coefficients() const noexcept { return c_; }
    void reset() noexcept
    {
        x_.fill(d2{0.0, 0.0});
        uPrev_ = d2{0.0, 0.0};
        line_.fill(d2{0.0, 0.0});
        pos_ = 0;
    }
    d2 tick(d2 u) noexcept
    {
        d2 y = c_.D * uPrev_;
        for (int i = 0; i < N; ++i) y += c_.C[i] * x_[static_cast<size_t>(i)];
        std::array<d2, N> xn;
        for (int i = 0; i < N; ++i) {
            d2 acc = c_.G0m[i] * uPrev_ + c_.G1[i] * u;
            for (int j = 0; j < N; ++j) acc += c_.Phi[i][j] * x_[static_cast<size_t>(j)];
            xn[static_cast<size_t>(i)] = acc;
        }
        x_ = xn;
        uPrev_ = u;
        pos_ = (pos_ + kMaxFir - 1) & (kMaxFir - 1);
        line_[static_cast<size_t>(pos_)] = y;
        d2 out = 0.0;
        for (int k = 0; k < c_.nFir; ++k) out += c_.fir[k] * line_[static_cast<size_t>((pos_ + k) & (kMaxFir - 1))];
        return out;
    }
    void flushTiny() noexcept
    {
        for (auto& v : x_)
            for (int l = 0; l < 2; ++l)
                if (std::fabs(v[l]) < 1e-30) v[l] = 0.0;
    }
    // Smallest non-zero |state| (subnormal checks in tests).
    double minAbsNonzeroState() const noexcept
    {
        double m = 0.0;
        auto see = [&](double v) {
            v = std::fabs(v);
            if (v > 0.0 && (m == 0.0 || v < m)) m = v;
        };
        for (const auto& v : x_) see(v[0]), see(v[1]);
        for (const auto& v : line_) see(v[0]), see(v[1]);
        return m;
    }
    const std::array<d2, N>& state() const noexcept { return x_; }

private:
    static_assert((kMaxFir & (kMaxFir - 1)) == 0, "FIR ring must be a power of two");
    FohCoeffs<N> c_{};
    std::array<d2, N> x_{};
    d2 uPrev_ = 0.0;
    std::array<d2, kMaxFir> line_{};
    int pos_ = 0;
};

} // namespace lkit
