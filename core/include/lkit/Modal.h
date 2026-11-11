#pragma once
// Modal first-order-hold state-space filter with physical states (Pre 73 P5b; generic in N and block size).
//
// The circuit's continuous state space (physical states: capacitor voltages, inductor currents, lkit/Circuit.h)
// is brought to block-diagonal form by a real similarity x = V s: every block of BS states is the invariant
// subspace of BS eigenvalues of A (BS = 2: a complex pair, or two real poles; the pairing keeps eigenvalues
// that lie close together in the same block, so V stays well conditioned). BS = N means one dense block, V = I.
// Each block is discretised with exact poles by a first-order hold, as lkit/Foh.h. With w[n] = x[n] - G1 u[n-1]
// (x[n] the analog state at t[n-1]) the recursion has one input:
//     s[n+1] = Lambda s[n] + h u[n-1]                       (block diagonal, BS x BS per block)
//     y[n]   = c . s[n] + sum_j d[j] u[n-1-j],  j = 0..kTaps-1
// c and d are fitted by relative least squares so the section matches the analog response times an integer
// delay of `delay` samples (the first-order hold approximates the zeros; the taps correct that). The recursion
// stays the exact FOH: poles exact, states physical.
//
// Coefficient changes go through the physical basis: x = V_old (s + g1_old u[n-1]); s_new = W_new x - g1_new u[n-1]
// (W = V^-1, g1 = W G1), so the capacitor voltages and inductor currents carry over and a change of setting
// does not restart the network. Double precision, no allocation, realtime safe (design() is for the control
// clock, tick() for the audio loop).

#include "lkit/Foh.h"
#include "lkit/LinAlg.h"
#include "lkit/Simd.h"

#include <array>
#include <cmath>
#include <complex>

namespace lkit {

inline constexpr int kModalTaps = 4;   // d[0..3] on u[n-1..n-4] (K = 3 output taps, P5 prototype)

template <int N, int BS>
struct ModalCoeffs {
    static_assert(N % BS == 0, "block size must divide the state count");
    static constexpr int kBlocks = N / BS;
    double lam[N][BS] = {};   // row i: block (i / BS) of the discrete recursion
    double h[N] = {}, c[N] = {}, d[kModalTaps] = {};
    double V[N][N] = {}, W[N][N] = {}, g1[N] = {};   // physical basis: x = V (s + g1 u[n-1])
    double Am[N][BS] = {}, bm[N] = {}, cm[N] = {}, Dm = 0.0;   // continuous modal form (analog response)
    int delay = 1;
    bool valid = false;

    // Discrete response at z^-1 = z1.
    std::complex<double> response(std::complex<double> z1) const noexcept
    {
        using cd = std::complex<double>;
        cd s[N];
        stateResponse(z1, s);
        cd y = 0.0, zk = z1;
        for (int i = 0; i < N; ++i) y += c[i] * s[i];
        for (int j = 0; j < kModalTaps; ++j, zk *= z1) y += d[j] * zk;
        return y;
    }
    // Analog response of the circuit at f (Hz), from the modal form.
    std::complex<double> analog(double freqHz) const noexcept
    {
        using cd = std::complex<double>;
        const cd jw(0.0, 2.0 * 3.14159265358979323846 * freqHz);
        cd y = Dm;
        for (int b = 0; b < kBlocks; ++b) {
            cd M[BS][BS], R[BS][1];
            for (int i = 0; i < BS; ++i) {
                for (int k = 0; k < BS; ++k) M[i][k] = (i == k ? jw : cd(0.0)) - Am[b * BS + i][k];
                R[i][0] = bm[b * BS + i];
            }
            if (!linalg::luSolve<BS, 1, cd>(M, R)) return {0.0, 0.0};
            for (int i = 0; i < BS; ++i) y += cm[b * BS + i] * R[i][0];
        }
        return y;
    }
    // Modal state response to the input, S(z) / U(z) = (zI - Lambda)^-1 h z^-1, per state.
    void stateResponse(std::complex<double> z1, std::complex<double> (&s)[N]) const noexcept
    {
        using cd = std::complex<double>;
        const cd z = 1.0 / z1;
        for (int b = 0; b < kBlocks; ++b) {
            cd M[BS][BS], R[BS][1];
            for (int i = 0; i < BS; ++i) {
                for (int k = 0; k < BS; ++k) M[i][k] = (i == k ? z : cd(0.0)) - lam[b * BS + i][k];
                R[i][0] = h[b * BS + i] * z1;
            }
            if (!linalg::luSolve<BS, 1, cd>(M, R))
                for (int i = 0; i < BS; ++i) R[i][0] = 0.0;
            for (int i = 0; i < BS; ++i) s[b * BS + i] = R[i][0];
        }
    }
};

template <int N, int BS>
class ModalDesigner {
public:
    static constexpr int kPoints = 40;   // output-map fit points, log spaced 10 Hz .. top
    static constexpr int kBlocks = N / BS;

    // fs: filter rate; topHz: top of the fitted band (the fit runs to min(1.15 top, 0.45 fs)).
    void prepare(double fs, double topHz) noexcept
    {
        fs_ = fs;
        const double hi = std::min(topHz * 1.15, 0.45 * fs);
        for (int i = 0; i < kPoints; ++i) f_[static_cast<size_t>(i)] = 10.0 * std::pow(hi / 10.0, i / double(kPoints - 1));
    }
    double sampleRate() const noexcept { return fs_; }

    bool design(const StateSpace<N>& ss, int delay, ModalCoeffs<N, BS>& out) const noexcept
    {
        out.valid = false;
        out.delay = delay;
        if (!decompose(ss, out)) return false;
        if (!discretise(out)) return false;
        if (!refine(out)) return false;
        out.valid = true;
        return true;
    }

private:
    // V, W and the continuous modal blocks.
    bool decompose(const StateSpace<N>& ss, ModalCoeffs<N, BS>& o) const noexcept
    {
        if constexpr (BS == N) {
            for (int i = 0; i < N; ++i) {
                for (int j = 0; j < N; ++j) o.V[i][j] = o.W[i][j] = i == j ? 1.0 : 0.0, o.Am[i][j] = ss.A[i][j];
                o.bm[i] = ss.B[i];
                o.cm[i] = ss.C[i];
            }
            o.Dm = ss.D;
            return true;
        } else {
            static_assert(BS == 2, "modal blocks are 2 x 2 (or one dense block)");
            const double T = 1.0 / fs_;
            double At[N][N];
            for (int i = 0; i < N; ++i)
                for (int j = 0; j < N; ++j) At[i][j] = ss.A[i][j] * T;   // dimensionless
            double re[N], im[N], dsc[N];
            if (!linalg::eigenvalues<N>(At, re, im, dsc)) return false;
            int pair[kBlocks][2];
            if (!pairUp(re, im, pair)) return false;
            // balanced matrix Ab = D^-1 At D: invariant subspaces are better conditioned there; V = D Vb
            double Ab[N][N];
            for (int i = 0; i < N; ++i)
                for (int j = 0; j < N; ++j) Ab[i][j] = At[i][j] * dsc[j] / dsc[i];
            double A2[N][N];
            linalg::matmul<N>(Ab, Ab, A2);
            for (int b = 0; b < kBlocks; ++b) {
                const int p = pair[b][0], q = pair[b][1];
                // (A - l_p)(A - l_q) = A^2 - (l_p + l_q) A + l_p l_q, real for a conjugate or a real pair
                const std::complex<double> lp(re[p], im[p]), lq(re[q], im[q]);
                const double tr = (lp + lq).real(), det = (lp * lq).real();
                double M[N][N], mx = 0.0;
                for (int i = 0; i < N; ++i)
                    for (int j = 0; j < N; ++j) {
                        M[i][j] = A2[i][j] - tr * Ab[i][j] + (i == j ? det : 0.0);
                        mx = std::max(mx, std::fabs(M[i][j]));
                    }
                if (!(mx > 0.0)) return false;
                for (auto& row : M)
                    for (double& v : row) v /= mx;
                double Bk[N][2];
                if (!linalg::nullspace<N, 2>(M, Bk)) return false;
                for (int i = 0; i < N; ++i) o.V[i][2 * b] = Bk[i][0] * dsc[i], o.V[i][2 * b + 1] = Bk[i][1] * dsc[i];
            }
            double Vc[N][N];
            for (int i = 0; i < N; ++i)
                for (int j = 0; j < N; ++j) Vc[i][j] = o.V[i][j], o.W[i][j] = i == j ? 1.0 : 0.0;
            if (!linalg::luSolve<N, N>(Vc, o.W)) return false;
            // Am = W A V (block diagonal part), bm = W B, cm = C V
            double AV[N][N];
            for (int i = 0; i < N; ++i)
                for (int j = 0; j < N; ++j) {
                    double acc = 0.0;
                    for (int k = 0; k < N; ++k) acc += ss.A[i][k] * o.V[k][j];
                    AV[i][j] = acc;
                }
            for (int i = 0; i < N; ++i) {
                const int b0 = (i / BS) * BS;
                for (int k = 0; k < BS; ++k) {
                    double acc = 0.0;
                    for (int m = 0; m < N; ++m) acc += o.W[i][m] * AV[m][b0 + k];
                    o.Am[i][k] = acc;
                }
                double bacc = 0.0, cacc = 0.0;
                for (int m = 0; m < N; ++m) bacc += o.W[i][m] * ss.B[m], cacc += ss.C[m] * o.V[m][i];
                o.bm[i] = bacc;
                o.cm[i] = cacc;
            }
            o.Dm = ss.D;
            return true;
        }
    }

    // Pair the eigenvalues into blocks of two: conjugate pairs together; the real ones by the matching that
    // keeps the smallest relative distance between eigenvalues of different blocks as large as possible.
    static bool pairUp(const double (&re)[N], const double (&im)[N], int (&pair)[kBlocks][2]) noexcept
    {
        int nb = 0, reals[N], nr = 0;
        for (int i = 0; i < N; ++i) {
            if (im[i] > 0.0) {
                if (i + 1 >= N || !(im[i + 1] < 0.0) || nb >= kBlocks) return false;
                pair[nb][0] = i, pair[nb][1] = i + 1, ++nb, ++i;
            } else if (im[i] == 0.0) {
                reals[nr++] = i;
            } else {
                return false;
            }
        }
        if (nr % 2 != 0 || nb + nr / 2 != kBlocks) return false;
        if (nr == 0) return true;
        auto dist = [&](int p, int q) {
            const double dr = re[p] - re[q], di = im[p] - im[q];
            const double sc = std::max({std::hypot(re[p], im[p]), std::hypot(re[q], im[q]), 1e-7});
            return std::hypot(dr, di) / sc;
        };
        // enumerate the perfect matchings of the real eigenvalues (at most 15 for six)
        int best[N][2] = {}, cur[N][2] = {};
        double bestSep = -1.0;
        bool used[N] = {};
        auto score = [&](int npairs) {
            double sep = 1e300;
            int all[kBlocks][2];
            for (int b = 0; b < nb; ++b) all[b][0] = pair[b][0], all[b][1] = pair[b][1];
            for (int k = 0; k < npairs; ++k) all[nb + k][0] = cur[k][0], all[nb + k][1] = cur[k][1];
            for (int b1 = 0; b1 < kBlocks; ++b1)
                for (int b2 = b1 + 1; b2 < kBlocks; ++b2)
                    for (int u : all[b1])
                        for (int v : all[b2]) sep = std::min(sep, dist(u, v));
            return sep;
        };
        auto rec = [&](auto&& self, int k) -> void {
            int first = -1;
            for (int i = 0; i < nr; ++i)
                if (!used[i]) {
                    first = i;
                    break;
                }
            if (first < 0) {
                const double s = score(k);
                if (s > bestSep) {
                    bestSep = s;
                    for (int m = 0; m < k; ++m) best[m][0] = cur[m][0], best[m][1] = cur[m][1];
                }
                return;
            }
            used[first] = true;
            for (int j = first + 1; j < nr; ++j) {
                if (used[j]) continue;
                used[j] = true;
                cur[k][0] = reals[first], cur[k][1] = reals[j];
                self(self, k + 1);
                used[j] = false;
            }
            used[first] = false;
        };
        rec(rec, 0);
        for (int k = 0; k < nr / 2; ++k) pair[nb + k][0] = best[k][0], pair[nb + k][1] = best[k][1];
        return true;
    }

    // First-order hold per block: exp([[Am T, bm T, 0], [0, 0, 1], [0, 0, 0]]) = [[Phi, G0, G1], ...].
    bool discretise(ModalCoeffs<N, BS>& o) const noexcept
    {
        constexpr int M = BS + 2;
        const double T = 1.0 / fs_;
        for (int b = 0; b < kBlocks; ++b) {
            double Mx[M][M] = {}, E[M][M];
            for (int i = 0; i < BS; ++i) {
                for (int k = 0; k < BS; ++k) Mx[i][k] = o.Am[b * BS + i][k] * T;
                Mx[i][BS] = o.bm[b * BS + i] * T;
            }
            Mx[BS][BS + 1] = 1.0;
            if (!linalg::expm<M>(Mx, E)) return false;
            for (int i = 0; i < BS; ++i) {
                const int r = b * BS + i;
                double hp = E[i][BS] - E[i][BS + 1];   // G0 - G1
                for (int k = 0; k < BS; ++k) {
                    o.lam[r][k] = E[i][k];
                    hp += E[i][k] * E[k][BS + 1];      // + Phi G1
                }
                o.h[r] = hp;
                o.g1[r] = E[i][BS + 1];
            }
        }
        return true;
    }

    // c, d by relative least squares against the analog response times z^-delay.
    bool refine(ModalCoeffs<N, BS>& o) const noexcept
    {
        using cd = std::complex<double>;
        constexpr int rows = 2 * kPoints, cols = N + kModalTaps;
        double A[rows][cols], b[rows], x[cols];
        const double w0 = -2.0 * 3.14159265358979323846 / fs_;
        for (int i = 0; i < kPoints; ++i) {
            const double f = f_[static_cast<size_t>(i)];
            const cd z1 = std::polar(1.0, w0 * f);
            const cd t = o.analog(f) * std::pow(z1, o.delay);
            const double w = 1.0 / std::max(std::abs(t), 1e-9);
            cd s[N];
            o.stateResponse(z1, s);
            for (int k = 0; k < N; ++k) A[2 * i][k] = w * s[k].real(), A[2 * i + 1][k] = w * s[k].imag();
            cd zk = z1;
            for (int j = 0; j < kModalTaps; ++j, zk *= z1) A[2 * i][N + j] = w * zk.real(), A[2 * i + 1][N + j] = w * zk.imag();
            b[2 * i] = w * t.real(), b[2 * i + 1] = w * t.imag();
        }
        if (!linalg::lstsq<rows, cols>(A, b, rows, cols, x)) return false;
        for (int k = 0; k < N; ++k) o.c[k] = x[k];
        for (int j = 0; j < kModalTaps; ++j) o.d[j] = x[N + j];
        return true;
    }

    double fs_ = 96000.0;
    std::array<double, kPoints> f_{};
};

// Stereo (2-lane) realtime filter.
template <int N, int BS>
class ModalFilter {
public:
    static constexpr int kBlocks = N / BS;

    // New coefficients; the states go through the physical basis so the network carries on.
    void setCoefficients(const ModalCoeffs<N, BS>& c) noexcept
    {
        if (!c.valid) return;
        if (c_.valid) {
            std::array<d2, N> x{}, s{};
            for (int i = 0; i < N; ++i) {
                d2 acc = 0.0;
                for (int k = 0; k < N; ++k) acc += c_.V[i][k] * (s_[static_cast<size_t>(k)] + c_.g1[k] * u_[0]);
                x[static_cast<size_t>(i)] = acc;
            }
            for (int i = 0; i < N; ++i) {
                d2 acc = -c.g1[i] * u_[0];
                for (int k = 0; k < N; ++k) acc += c.W[i][k] * x[static_cast<size_t>(k)];
                s[static_cast<size_t>(i)] = acc;
            }
            s_ = s;
        }
        c_ = c;
    }
    const ModalCoeffs<N, BS>& coefficients() const noexcept { return c_; }
    void reset() noexcept
    {
        s_.fill(d2{0.0, 0.0});
        u_.fill(d2{0.0, 0.0});
    }
    d2 tick(d2 u) noexcept
    {
        d2 y = c_.d[0] * u_[0];
        for (int j = 1; j < kModalTaps; ++j) y += c_.d[j] * u_[static_cast<size_t>(j)];
        for (int i = 0; i < N; ++i) y += c_.c[i] * s_[static_cast<size_t>(i)];
        std::array<d2, N> sn;
        for (int b = 0; b < kBlocks; ++b)
            for (int i = 0; i < BS; ++i) {
                const int r = b * BS + i;
                d2 acc = c_.h[r] * u_[0];
                for (int k = 0; k < BS; ++k) acc += c_.lam[r][k] * s_[static_cast<size_t>(b * BS + k)];
                sn[static_cast<size_t>(r)] = acc;
            }
        s_ = sn;
        for (int j = kModalTaps - 1; j > 0; --j) u_[static_cast<size_t>(j)] = u_[static_cast<size_t>(j - 1)];
        u_[0] = u;
        return y;
    }
    // Physical states x = V (s + g1 u[n-1]) (tests).
    std::array<d2, N> physicalState() const noexcept
    {
        std::array<d2, N> x{};
        for (int i = 0; i < N; ++i) {
            d2 acc = 0.0;
            for (int k = 0; k < N; ++k) acc += c_.V[i][k] * (s_[static_cast<size_t>(k)] + c_.g1[k] * u_[0]);
            x[static_cast<size_t>(i)] = acc;
        }
        return x;
    }
    void flushTiny() noexcept
    {
        auto flush = [](d2& v) {
            for (int l = 0; l < 2; ++l)
                if (std::fabs(v[l]) < 1e-30) v[l] = 0.0;
        };
        for (auto& v : s_) flush(v);
        for (auto& v : u_) flush(v);
    }
    double minAbsNonzeroState() const noexcept
    {
        double m = 0.0;
        auto see = [&](double v) {
            v = std::fabs(v);
            if (v > 0.0 && (m == 0.0 || v < m)) m = v;
        };
        for (const auto& v : s_) see(v[0]), see(v[1]);
        for (const auto& v : u_) see(v[0]), see(v[1]);
        return m;
    }

private:
    ModalCoeffs<N, BS> c_{};
    std::array<d2, N> s_{};
    std::array<d2, kModalTaps> u_{};
};

} // namespace lkit
