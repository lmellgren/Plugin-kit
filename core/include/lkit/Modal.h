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
//     y[n]   = c . s[n] + sum_j d[j] u[n-1-j],  j = 0..TAPS-1
// c and d are fitted by relative least squares so the section matches the analog response times an integer
// delay of `delay` samples (the first-order hold approximates the zeros; the taps correct that). The recursion
// stays the exact FOH: poles exact, states physical. Each 2 x 2 block's basis is then rotated so that its input
// vector is (eta, 0): the same filter with fewer coefficients per sample (the audio loop is load-bound).
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

inline constexpr int kModalTaps = 4;   // default: d[0..3] on u[n-1..n-4] (K = 3 output taps, P5 prototype)

namespace modal_detail {
// Plain complex arithmetic for the design loops (std::complex multiplication and division carry the C99
// NaN/Inf recovery paths, which dominate the cost of a redesign).
struct cx {
    double r = 0.0, i = 0.0;
};
inline cx operator+(cx a, cx b) noexcept { return {a.r + b.r, a.i + b.i}; }
inline cx operator-(cx a, cx b) noexcept { return {a.r - b.r, a.i - b.i}; }
inline cx operator*(cx a, cx b) noexcept { return {a.r * b.r - a.i * b.i, a.r * b.i + a.i * b.r}; }
inline cx operator*(double a, cx b) noexcept { return {a * b.r, a * b.i}; }
inline cx inv(cx a) noexcept
{
    const double m = 1.0 / (a.r * a.r + a.i * a.i);
    return {a.r * m, -a.i * m};
}
inline double abs2(cx a) noexcept { return a.r * a.r + a.i * a.i; }

// out = (p I - B)^-1 r * scale for one BS x BS real block (rows B[0..BS-1]), complex p, real r.
template <int BS>
inline void resolvent(cx p, const double (*B)[BS], const double* r, cx scale, cx* out) noexcept
{
    if constexpr (BS == 2) {
        const cx pa = p - cx{B[0][0], 0.0}, pd = p - cx{B[1][1], 0.0};
        const cx q = inv(pa * pd - cx{B[0][1] * B[1][0], 0.0}) * scale;
        out[0] = (r[0] * pd + cx{B[0][1] * r[1], 0.0}) * q;
        out[1] = (cx{B[1][0] * r[0], 0.0} + r[1] * pa) * q;
    } else {
        using cd = std::complex<double>;
        cd M[BS][BS], R[BS][1];
        for (int i = 0; i < BS; ++i) {
            for (int k = 0; k < BS; ++k) M[i][k] = (i == k ? cd(p.r, p.i) : cd(0.0)) - B[i][k];
            R[i][0] = r[i];
        }
        const bool ok = linalg::luSolve<BS, 1, cd>(M, R);
        for (int i = 0; i < BS; ++i) out[i] = ok ? cx{R[i][0].real(), R[i][0].imag()} * scale : cx{};
    }
}
} // namespace modal_detail

template <int N, int BS, int TAPS = kModalTaps>
struct ModalCoeffs {
    static_assert(N % BS == 0, "block size must divide the state count");
    static constexpr int kBlocks = N / BS;
    double lam[N][BS] = {};   // row i: block (i / BS) of the discrete recursion
    double h[N] = {}, c[N] = {}, d[TAPS] = {};   // BS = 2: h = (eta, 0) per block (normalised)
    double V[N][N] = {}, W[N][N] = {}, g1[N] = {};   // physical basis: x = V (s + g1 u[n-1])
    double Am[N][BS] = {}, bm[N] = {}, cm[N] = {}, Dm = 0.0;   // continuous modal form (analog response)
    int delay = 1;
    bool valid = false;

    // Discrete response at z^-1 = z1.
    std::complex<double> response(std::complex<double> z1) const noexcept
    {
        const modal_detail::cx r = responseFast({z1.real(), z1.imag()});
        return {r.r, r.i};
    }
    // Analog response of the circuit at f (Hz), from the modal form.
    std::complex<double> analog(double freqHz) const noexcept
    {
        const modal_detail::cx r = analogFast({0.0, 2.0 * 3.14159265358979323846 * freqHz});
        return {r.r, r.i};
    }
    // Modal state response to the input, S(z) / U(z) = (zI - Lambda)^-1 h z^-1, per state.
    void stateResponse(modal_detail::cx z1, modal_detail::cx (&s)[N]) const noexcept
    {
        const modal_detail::cx z = modal_detail::inv(z1);
        for (int b = 0; b < kBlocks; ++b) modal_detail::resolvent<BS>(z, lam + b * BS, h + b * BS, z1, s + b * BS);
    }
    modal_detail::cx responseFast(modal_detail::cx z1) const noexcept
    {
        modal_detail::cx s[N], y{}, zk = z1;
        stateResponse(z1, s);
        for (int i = 0; i < N; ++i) y = y + c[i] * s[i];
        for (int j = 0; j < TAPS; ++j, zk = zk * z1) y = y + d[j] * zk;
        return y;
    }
    modal_detail::cx analogFast(modal_detail::cx jw) const noexcept
    {
        modal_detail::cx y{Dm, 0.0}, r[N];
        for (int b = 0; b < kBlocks; ++b) modal_detail::resolvent<BS>(jw, Am + b * BS, bm + b * BS, {1.0, 0.0}, r + b * BS);
        for (int i = 0; i < N; ++i) y = y + cm[i] * r[i];
        return y;
    }
};

template <int N, int BS, int TAPS = kModalTaps, int POINTS = 24>
class ModalDesigner {
public:
    static constexpr int kPoints = POINTS;   // output-map fit points, log spaced 10 Hz .. top
    static constexpr int kCoarse = 2;    // coarse fit (while a knob moves): every second point
    static constexpr int kBlocks = N / BS;

    // fs: filter rate; topHz: top of the fitted band (the fit runs to min(1.15 top, 0.45 fs)).
    void prepare(double fs, double topHz) noexcept
    {
        fs_ = fs;
        const double hi = std::min(topHz * 1.15, 0.45 * fs);
        for (size_t i = 0; i < static_cast<size_t>(kPoints); ++i) {
            f_[i] = 10.0 * std::pow(hi / 10.0, static_cast<double>(i) / (kPoints - 1));
            const double w = 2.0 * 3.14159265358979323846 * f_[i];
            jw_[i] = {0.0, w};
            modal_detail::cx z1{std::cos(w / fs), -std::sin(w / fs)}, zk{1.0, 0.0};
            for (auto& v : zp_[i]) v = zk, zk = zk * z1;
        }
    }
    double sampleRate() const noexcept { return fs_; }

    // coarse: fit the output map on every kCoarse-th point only (cheaper, for designs while a knob moves).
    bool design(const StateSpace<N>& ss, int delay, ModalCoeffs<N, BS, TAPS>& out, bool coarse = false) const noexcept
    {
        out.valid = false;
        out.delay = delay;
        if (!decompose(ss, out)) return false;
        if (!discretise(out)) return false;
        if (!refine(out, coarse)) return false;
        if constexpr (BS == 2) normalise(out);
        out.valid = true;
        return true;
    }

    // The three steps of design(), public for per-step benchmarks.
    // V, W and the continuous modal blocks.
    bool decompose(const StateSpace<N>& ss, ModalCoeffs<N, BS, TAPS>& o) const noexcept
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
    bool discretise(ModalCoeffs<N, BS, TAPS>& o) const noexcept
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
    // Regularised towards the circuit's own output map (y = C x + D u[n-1], in the modal basis: c = C V,
    // d = (C G1 + D, 0, ...)): a small weight relative to each column, plus a floor relative to the largest column.
    // Directions the band does not determine (a near-DC mode, inert parts) keep their physical coefficient instead
    // of an arbitrary large one, so states converted at a switch cannot produce an output spike.
    static constexpr double kRidge = 1e-5, kRidgeFloor = 1e-9;
    bool refine(ModalCoeffs<N, BS, TAPS>& o, bool coarse = false) const noexcept
    {
        using modal_detail::cx;
        constexpr int rows = 2 * kPoints + N + TAPS, cols = N + TAPS;
        double A[cols][rows], b[rows], x[cols];   // by columns (lstsqColumns)
        const int dl = std::clamp(o.delay, 0, kMaxDelay);
        const size_t step = coarse ? kCoarse : 1;
        int row = 0;
        for (size_t i = 0; i < static_cast<size_t>(kPoints); i += step, row += 2) {
            const auto& zp = zp_[i];   // z^-1 powers 0 .. TAPS + kMaxDelay
            const cx t = o.analogFast(jw_[i]) * zp[static_cast<size_t>(dl)];
            const double w = 1.0 / std::sqrt(std::max(modal_detail::abs2(t), 1e-18));
            cx s[N];
            o.stateResponse(zp[1], s);
            const int r0 = row, r1 = row + 1;
            for (int k = 0; k < N; ++k) A[k][r0] = w * s[k].r, A[k][r1] = w * s[k].i;
            for (int j = 0; j < TAPS; ++j) A[N + j][r0] = w * zp[static_cast<size_t>(j + 1)].r, A[N + j][r1] = w * zp[static_cast<size_t>(j + 1)].i;
            b[r0] = w * t.r, b[r1] = w * t.i;
        }
        double x0[cols] = {}, d0 = o.Dm;
        for (int k = 0; k < N; ++k) x0[k] = o.cm[k], d0 += o.cm[k] * o.g1[k];
        x0[N] = d0;
        double n2[cols], mx = 0.0;
        for (int k = 0; k < cols; ++k) {
            n2[k] = 0.0;
            for (int i = 0; i < row; ++i) n2[k] += A[k][i] * A[k][i];
            mx = std::max(mx, n2[k]);
        }
        for (int k = 0; k < cols; ++k, ++row) {
            const double mu = kRidge * std::sqrt(n2[k]) + kRidgeFloor * std::sqrt(mx);
            for (int j = 0; j < cols; ++j) A[j][row] = j == k ? mu : 0.0;
            b[row] = mu * x0[k];
        }
        if (!linalg::lstsqColumns<cols, rows>(A, b, row, cols, x)) return false;
        for (int k = 0; k < N; ++k) o.c[k] = x[k];
        for (int j = 0; j < TAPS; ++j) o.d[j] = x[N + j];
        return true;
    }

    // Per 2 x 2 block, the rotation T = [t0 t0perp] with t0 = h / |h|: h -> (|h|, 0), so only the block's first
    // row takes the input. Orthogonal, so the conditioning of V and W is unchanged (scaling the output weights to
    // 1 as well is ill-conditioned for a block that barely reaches the output, such as a near-DC mode).
    // Lambda, V, W, g1, c and the continuous form follow (s = T s'); the filter and its physical states are unchanged.
    static void normalise(ModalCoeffs<N, BS, TAPS>& o) noexcept
    {
        for (int b = 0; b < kBlocks; ++b) {
            const int r = 2 * b;
            const double hn = std::hypot(o.h[r], o.h[r + 1]);
            double t0[2] = {1.0, 0.0};
            if (hn > 0.0) t0[0] = o.h[r] / hn, t0[1] = o.h[r + 1] / hn;
            const double T[2][2] = {{t0[0], -t0[1]}, {t0[1], t0[0]}};   // columns t0, t0 rotated by 90 degrees
            auto sim = [&](double (*M)[2]) {   // M <- T' M T on rows r, r + 1
                double MT[2][2];
                for (int i = 0; i < 2; ++i)
                    for (int j = 0; j < 2; ++j) MT[i][j] = M[r + i][0] * T[0][j] + M[r + i][1] * T[1][j];
                for (int i = 0; i < 2; ++i)
                    for (int j = 0; j < 2; ++j) M[r + i][j] = T[0][i] * MT[0][j] + T[1][i] * MT[1][j];
            };
            auto left = [&](double* v) {   // v <- T' v
                const double a0 = T[0][0] * v[r] + T[1][0] * v[r + 1], a1 = T[0][1] * v[r] + T[1][1] * v[r + 1];
                v[r] = a0, v[r + 1] = a1;
            };
            auto right = [&](double* v) {   // v <- v T (row vector)
                const double a0 = v[r] * T[0][0] + v[r + 1] * T[1][0], a1 = v[r] * T[0][1] + v[r + 1] * T[1][1];
                v[r] = a0, v[r + 1] = a1;
            };
            sim(o.lam);
            sim(o.Am);
            left(o.h);
            left(o.g1);
            left(o.bm);
            right(o.c);
            right(o.cm);
            o.h[r + 1] = 0.0;   // exact zero (rounding)
            for (int i = 0; i < N; ++i) {   // V <- V T (columns r, r + 1); W <- T' W (rows r, r + 1)
                const double v0 = o.V[i][r] * T[0][0] + o.V[i][r + 1] * T[1][0], v1 = o.V[i][r] * T[0][1] + o.V[i][r + 1] * T[1][1];
                o.V[i][r] = v0, o.V[i][r + 1] = v1;
                const double w0 = T[0][0] * o.W[r][i] + T[1][0] * o.W[r + 1][i], w1 = T[0][1] * o.W[r][i] + T[1][1] * o.W[r + 1][i];
                o.W[r][i] = w0, o.W[r + 1][i] = w1;
            }
        }
    }

private:
    static constexpr int kMaxDelay = 4;
    double fs_ = 96000.0;
    std::array<double, kPoints> f_{};
    std::array<modal_detail::cx, kPoints> jw_{};
    std::array<std::array<modal_detail::cx, TAPS + kMaxDelay + 1>, kPoints> zp_{};
};

// The realtime part of a modal filter: recursion, output map and states. Small and copyable, so a plugin's
// audio loop can hold the kernels of a whole cascade in locals (registers) for a block and write them back.
template <int N, int BS, int TAPS = kModalTaps>
struct ModalKernel {
    static constexpr int kBlocks = N / BS;
    double lam[N][BS] = {}, h[N] = {}, c[N] = {}, d[TAPS] = {};
    double eta[kBlocks] = {};   // BS = 2: the input weight of each block's first row (the second row has none)
    std::array<d2, N> s{};
    std::array<d2, TAPS> u{};   // u[n-1] .. u[n-TAPS]

    // Output: the newest input enters last and the rest is summed in two partial sums, so the path from one
    // section's output to the next one's is a single FMA and a cascade of kernels stays throughput-bound.
    // Recursion: the input first and each row's own state last, so the loop-carried chain is two FMAs.
    d2 tick(d2 x) noexcept
    {
        static_assert(TAPS >= 2, "at least two output taps");
        d2 p0 = d[1] * u[1], p1 = 0.0;
        if constexpr (TAPS > 2) p1 = d[2] * u[2];
        for (int j = 3; j < TAPS; ++j) p0 += d[j] * u[static_cast<size_t>(j)];
        std::array<d2, N> sn;
        if constexpr (BS == 2) {
            for (int b = 0; b < kBlocks; ++b) {
                const size_t r = static_cast<size_t>(2 * b);
                ((b & 1) ? p0 : p1) += c[r] * s[r];
                ((b & 1) ? p1 : p0) += c[r + 1] * s[r + 1];
                d2 a0 = eta[b] * u[0];
                a0 += lam[r][1] * s[r + 1];
                a0 += lam[r][0] * s[r];
                d2 a1 = lam[r + 1][0] * s[r];
                a1 += lam[r + 1][1] * s[r + 1];
                sn[r] = a0, sn[r + 1] = a1;
            }
        } else {
            for (int i = 0; i < N; ++i) ((i & 1) ? p0 : p1) += c[i] * s[static_cast<size_t>(i)];
            for (int b = 0; b < kBlocks; ++b)
                for (int i = 0; i < BS; ++i) {
                    const int r = b * BS + i;
                    d2 acc = h[r] * u[0];
                    for (int k = 0; k < BS; ++k)
                        if (k != i) acc += lam[r][k] * s[static_cast<size_t>(b * BS + k)];
                    acc += lam[r][i] * s[static_cast<size_t>(r)];
                    sn[static_cast<size_t>(r)] = acc;
                }
        }
        d2 y = p0 + p1;
        y += d[0] * u[0];
        s = sn;
        for (int j = TAPS - 1; j > 0; --j) u[static_cast<size_t>(j)] = u[static_cast<size_t>(j - 1)];
        u[0] = x;
        return y;
    }
};

// Stereo (2-lane) realtime filter.
template <int N, int BS, int TAPS = kModalTaps>
class ModalFilter {
public:
    static constexpr int kBlocks = N / BS;

    // New coefficients; the states go through the physical basis so the network carries on.
    void setCoefficients(const ModalCoeffs<N, BS, TAPS>& c) noexcept
    {
        if (!c.valid) return;
        if (c_.valid) {
            const auto x = physicalState();
            for (int i = 0; i < N; ++i) {
                d2 acc = -c.g1[i] * k_.u[0];
                for (int k = 0; k < N; ++k) acc += c.W[i][k] * x[static_cast<size_t>(k)];
                k_.s[static_cast<size_t>(i)] = acc;
            }
        }
        c_ = c;
        for (int i = 0; i < N; ++i) {
            for (int k = 0; k < BS; ++k) k_.lam[i][k] = c.lam[i][k];
            k_.h[i] = c.h[i];
            k_.c[i] = c.c[i];
        }
        for (int j = 0; j < TAPS; ++j) k_.d[j] = c.d[j];
        if constexpr (BS == 2)
            for (int b = 0; b < kBlocks; ++b) k_.eta[b] = c.h[2 * b];
    }
    const ModalCoeffs<N, BS, TAPS>& coefficients() const noexcept { return c_; }
    ModalKernel<N, BS, TAPS>& kernel() noexcept { return k_; }
    void reset() noexcept
    {
        k_.s.fill(d2{0.0, 0.0});
        k_.u.fill(d2{0.0, 0.0});
    }
    d2 tick(d2 u) noexcept { return k_.tick(u); }
    // Physical states to zero (capacitors discharged, inductors without current); the input history stays.
    void zeroPhysicalState() noexcept
    {
        for (int i = 0; i < N; ++i) k_.s[static_cast<size_t>(i)] = -c_.g1[i] * k_.u[0];
    }
    // Physical states x = V (s + g1 u[n-1]) (tests).
    std::array<d2, N> physicalState() const noexcept
    {
        std::array<d2, N> x{};
        for (int i = 0; i < N; ++i) {
            d2 acc = 0.0;
            for (int k = 0; k < N; ++k) acc += c_.V[i][k] * (k_.s[static_cast<size_t>(k)] + c_.g1[k] * k_.u[0]);
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
        for (auto& v : k_.s) flush(v);
        for (auto& v : k_.u) flush(v);
    }
    double minAbsNonzeroState() const noexcept
    {
        double m = 0.0;
        auto see = [&](double v) {
            v = std::fabs(v);
            if (v > 0.0 && (m == 0.0 || v < m)) m = v;
        };
        for (const auto& v : k_.s) see(v[0]), see(v[1]);
        for (const auto& v : k_.u) see(v[0]), see(v[1]);
        return m;
    }

private:
    ModalCoeffs<N, BS, TAPS> c_{};
    ModalKernel<N, BS, TAPS> k_{};
};

} // namespace lkit
