#pragma once
// Generic linear circuit -> continuous state space with PHYSICAL states (capacitor voltages, inductor
// currents), allocation-free, for realtime coefficient updates (the C++ twin of python/lkit/mna.py
// Netlist.state_space()). A plugin builds its netlist once (fixed topology, fixed state order), then changes
// element values (pots, switched parts) and calls stateSpace() on the control thread or at control rate.
//
//   lkit::Circuit<16, 24> c;                 // max nodes, max elements
//   int in = c.node(), out = c.node();       // node 0 is ground; the input source drives node `in`
//   int r1 = c.R(in, out, 1e3); c.C(out, 0, 1e-6);
//   c.setInput(in); c.setOutput(out);
//   lkit::StateSpace<1> ss; c.stateSpace(ss);
//   c.setValue(r1, 2e3);                     // e.g. a pot moved
//
// Elements: R, C, L (a, b, value), E (VCVS: out+, out-, ctrl+, ctrl-, gain). States: capacitors in the order
// added, then inductors in the order added. The resistive network is solved with each capacitor replaced by a
// voltage source (its state) and each inductor by a current source (its state), once per state and once for
// the input: C dv/dt = i_C, L di/dt = v_L, y = V(out).

#include "lkit/Foh.h"

#include <array>
#include <cmath>

namespace lkit {

template <int MaxNodes, int MaxElems>
class Circuit {
public:
    enum Kind { kR, kC, kL, kE };
    struct Element {
        Kind kind;
        int a, b, c, d;   // E: out+, out-, ctrl+, ctrl-
        double value;
    };

    int node() noexcept { return ++nodes_; }   // returns 1..; 0 is ground
    int R(int a, int b, double v) noexcept { return add({kR, a, b, 0, 0, v}); }
    int C(int a, int b, double v) noexcept { return add({kC, a, b, 0, 0, v}); }
    int L(int a, int b, double v) noexcept { return add({kL, a, b, 0, 0, v}); }
    int E(int op, int om, int cp, int cm, double gain) noexcept { return add({kE, op, om, cp, cm, gain}); }
    void setInput(int n) noexcept { in_ = n; }
    void setOutput(int n) noexcept { out_ = n; }
    void setValue(int element, double v) noexcept { el_[static_cast<size_t>(element)].value = v; }
    double value(int element) const noexcept { return el_[static_cast<size_t>(element)].value; }

    int numStates() const noexcept
    {
        int n = 0;
        for (int i = 0; i < n_; ++i) n += (el_[static_cast<size_t>(i)].kind == kC || el_[static_cast<size_t>(i)].kind == kL);
        return n;
    }

    // N must equal numStates(). Returns false if the network is singular.
    template <int N>
    bool stateSpace(StateSpace<N>& out) const noexcept
    {
        constexpr int MaxM = MaxNodes + 1 + MaxElems;   // nodes + input source + caps/VCVS branch currents
        constexpr int MaxR = N + 1;
        static_assert(N <= MaxElems, "too many states");
        double G[MaxM][MaxM];
        double Rhs[MaxM][MaxR];
        int capRow[MaxElems], capEl[N], indEl[N];
        int nc = 0, ni = 0;
        const int nn = nodes_;
        int m = nn;   // next branch row
        const int vin = m++;
        for (int i = 0; i < n_; ++i) {
            const auto& e = el_[static_cast<size_t>(i)];
            if (e.kind == kC) capEl[nc] = i, capRow[nc++] = m++;
            else if (e.kind == kL) indEl[ni++] = i;
        }
        int eRow[MaxElems];
        int ne = 0;
        for (int i = 0; i < n_; ++i)
            if (el_[static_cast<size_t>(i)].kind == kE) eRow[ne++] = m++;
        if (nc + ni != N) return false;
        for (int r = 0; r < m; ++r) {
            for (int k = 0; k < m; ++k) G[r][k] = 0.0;
            for (int k = 0; k <= N; ++k) Rhs[r][k] = 0.0;
        }
        auto idx = [](int nd) { return nd - 1; };   // ground (0) -> -1
        auto stamp = [&](int a, int b, double y) {
            const int ia = idx(a), ib = idx(b);
            if (ia >= 0) G[ia][ia] += y;
            if (ib >= 0) G[ib][ib] += y;
            if (ia >= 0 && ib >= 0) G[ia][ib] -= y, G[ib][ia] -= y;
        };
        auto vsrc = [&](int row, int a, int b) {
            const int ia = idx(a), ib = idx(b);
            if (ia >= 0) G[ia][row] += 1.0, G[row][ia] += 1.0;
            if (ib >= 0) G[ib][row] -= 1.0, G[row][ib] -= 1.0;
        };
        vsrc(vin, in_, 0);
        Rhs[vin][N] = 1.0;
        int ci = 0, ei = 0;
        for (int i = 0; i < n_; ++i) {
            const auto& e = el_[static_cast<size_t>(i)];
            switch (e.kind) {
            case kR: stamp(e.a, e.b, 1.0 / e.value); break;
            case kC: vsrc(capRow[ci], e.a, e.b); Rhs[capRow[ci]][ci] = 1.0; ++ci; break;
            case kL: break;
            case kE: {
                const int row = eRow[ei++];
                vsrc(row, e.a, e.b);
                if (idx(e.c) >= 0) G[row][idx(e.c)] -= e.value;
                if (idx(e.d) >= 0) G[row][idx(e.d)] += e.value;
                break;
            }
            }
        }
        for (int k = 0; k < ni; ++k) {   // inductor current a -> b leaves node a
            const auto& e = el_[static_cast<size_t>(indEl[k])];
            if (idx(e.a) >= 0) Rhs[idx(e.a)][nc + k] -= 1.0;
            if (idx(e.b) >= 0) Rhs[idx(e.b)][nc + k] += 1.0;
        }
        if (!solve<MaxM, MaxR>(G, Rhs, m, N + 1)) return false;
        for (int k = 0; k < nc; ++k) {
            const double cval = el_[static_cast<size_t>(capEl[k])].value;
            for (int j = 0; j < N; ++j) out.A[k][j] = Rhs[capRow[k]][j] / cval;
            out.B[k] = Rhs[capRow[k]][N] / cval;
        }
        if (ni > 0) {   // L di/dt = v_L (no mutual inductance in this C++ twin)
            for (int k = 0; k < ni; ++k) {
                const auto& e = el_[static_cast<size_t>(indEl[k])];
                for (int j = 0; j <= N; ++j) {
                    const double va = idx(e.a) >= 0 ? Rhs[idx(e.a)][j] : 0.0;
                    const double vb = idx(e.b) >= 0 ? Rhs[idx(e.b)][j] : 0.0;
                    const double di = (va - vb) / e.value;
                    if (j < N) out.A[nc + k][j] = di;
                    else out.B[nc + k] = di;
                }
            }
        }
        for (int j = 0; j < N; ++j) out.C[j] = Rhs[idx(out_)][j];
        out.D = Rhs[idx(out_)][N];
        return true;
    }

private:
    int add(Element e) noexcept
    {
        el_[static_cast<size_t>(n_)] = e;
        return n_++;
    }

    // Dense LU with partial pivoting on the leading m x m block, nr right-hand sides.
    template <int MM, int MR>
    static bool solve(double (&M)[MM][MM], double (&R)[MM][MR], int m, int nr) noexcept
    {
        for (int k = 0; k < m; ++k) {
            int p = k;
            double best = std::fabs(M[k][k]);
            for (int i = k + 1; i < m; ++i)
                if (std::fabs(M[i][k]) > best) best = std::fabs(M[i][k]), p = i;
            if (best == 0.0) return false;
            if (p != k) {
                for (int j = 0; j < m; ++j) std::swap(M[k][j], M[p][j]);
                for (int j = 0; j < nr; ++j) std::swap(R[k][j], R[p][j]);
            }
            const double inv = 1.0 / M[k][k];
            for (int i = k + 1; i < m; ++i) {
                const double f = M[i][k] * inv;
                if (f == 0.0) continue;
                for (int j = k; j < m; ++j) M[i][j] -= f * M[k][j];
                for (int j = 0; j < nr; ++j) R[i][j] -= f * R[k][j];
            }
        }
        for (int k = m - 1; k >= 0; --k)
            for (int j = 0; j < nr; ++j) {
                double acc = R[k][j];
                for (int c = k + 1; c < m; ++c) acc -= M[k][c] * R[c][j];
                R[k][j] = acc / M[k][k];
            }
        return true;
    }

    std::array<Element, MaxElems> el_{};
    int n_ = 0, nodes_ = 0, in_ = 1, out_ = 1;
};

} // namespace lkit
