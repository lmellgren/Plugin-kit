#pragma once
// Small dense linear algebra on fixed-size arrays (no allocation, realtime safe):
// LU solve (real/complex), least squares (Householder QR), matrix exponential (Pade 6,6 + squaring).
// Extracted from LOMAS Passive EQ M7b (Network.cpp, Matched.cpp) and made size-generic.

#include <algorithm>
#include <cmath>
#include <complex>

namespace lkit::linalg {

// Solve M X = R in place (R holds NR right-hand sides; on return R = X). Partial pivoting.
// Works for double and std::complex<double>. Returns false for a singular matrix.
template <int N, int NR, typename T = double>
bool luSolve(T (&M)[N][N], T (&R)[N][NR]) noexcept
{
    for (int k = 0; k < N; ++k) {
        int p = k;
        double best = std::abs(M[k][k]);
        for (int i = k + 1; i < N; ++i)
            if (std::abs(M[i][k]) > best) best = std::abs(M[i][k]), p = i;
        if (best == 0.0) return false;
        if (p != k) {
            for (int j = 0; j < N; ++j) std::swap(M[k][j], M[p][j]);
            for (int j = 0; j < NR; ++j) std::swap(R[k][j], R[p][j]);
        }
        for (int i = k + 1; i < N; ++i) {
            const T f = M[i][k] / M[k][k];
            if (f == T(0.0)) continue;
            for (int j = k; j < N; ++j) M[i][j] -= f * M[k][j];
            for (int j = 0; j < NR; ++j) R[i][j] -= f * R[k][j];
        }
    }
    for (int k = N - 1; k >= 0; --k)
        for (int j = 0; j < NR; ++j) {
            T acc = R[k][j];
            for (int c = k + 1; c < N; ++c) acc -= M[k][c] * R[c][j];
            R[k][j] = acc / M[k][k];
        }
    return true;
}

template <int N>
void matmul(const double (&a)[N][N], const double (&b)[N][N], double (&c)[N][N]) noexcept
{
    for (int i = 0; i < N; ++i)
        for (int j = 0; j < N; ++j) {
            double acc = 0.0;
            for (int k = 0; k < N; ++k) acc += a[i][k] * b[k][j];
            c[i][j] = acc;
        }
}

// exp(A) by scaling and squaring with a (6,6) Pade approximant.
template <int N>
bool expm(const double (&A)[N][N], double (&E)[N][N]) noexcept
{
    double norm = 0.0;
    for (int i = 0; i < N; ++i) {
        double r = 0.0;
        for (int j = 0; j < N; ++j) r += std::fabs(A[i][j]);
        norm = std::max(norm, r);
    }
    int sq = 0;
    if (norm > 0.5) sq = static_cast<int>(std::ceil(std::log2(norm / 0.5)));
    const double scale = std::ldexp(1.0, -sq);
    double X[N][N], Xp[N][N], T[N][N], Nm[N][N], Dm[N][N];
    for (int i = 0; i < N; ++i)
        for (int j = 0; j < N; ++j) {
            X[i][j] = A[i][j] * scale;
            Xp[i][j] = X[i][j];
            Nm[i][j] = Dm[i][j] = i == j ? 1.0 : 0.0;
        }
    constexpr double c[7] = {1.0, 0.5, 5.0 / 44.0, 1.0 / 66.0, 1.0 / 792.0, 1.0 / 15840.0, 1.0 / 665280.0};
    for (int k = 1; k <= 6; ++k) {
        if (k > 1) {
            matmul<N>(Xp, X, T);
            for (int i = 0; i < N; ++i)
                for (int j = 0; j < N; ++j) Xp[i][j] = T[i][j];
        }
        const double sgn = (k % 2) ? -1.0 : 1.0;
        for (int i = 0; i < N; ++i)
            for (int j = 0; j < N; ++j) {
                Nm[i][j] += c[k] * Xp[i][j];
                Dm[i][j] += sgn * c[k] * Xp[i][j];
            }
    }
    if (!luSolve<N, N>(Dm, Nm)) return false;   // Nm = Dm^-1 Nm
    for (int s = 0; s < sq; ++s) {
        matmul<N>(Nm, Nm, T);
        for (int i = 0; i < N; ++i)
            for (int j = 0; j < N; ++j) Nm[i][j] = T[i][j];
    }
    for (int i = 0; i < N; ++i)
        for (int j = 0; j < N; ++j) E[i][j] = Nm[i][j];
    return true;
}

// Least squares min |A x - b| (rows x cols, row-major), Householder QR with column scaling.
// a and b are destroyed. Returns false if A is rank deficient.
template <int MaxRows, int MaxCols>
bool lstsq(double (&a)[MaxRows][MaxCols], double (&b)[MaxRows], int rows, int cols, double* x) noexcept
{
    double scale[MaxCols];
    for (int j = 0; j < cols; ++j) {
        double n = 0.0;
        for (int i = 0; i < rows; ++i) n += a[i][j] * a[i][j];
        scale[j] = n > 0.0 ? 1.0 / std::sqrt(n) : 1.0;
        for (int i = 0; i < rows; ++i) a[i][j] *= scale[j];
    }
    double diag[MaxCols];
    for (int k = 0; k < cols; ++k) {
        double norm = 0.0;
        for (int i = k; i < rows; ++i) norm += a[i][k] * a[i][k];
        norm = std::sqrt(norm);
        if (norm == 0.0) return false;
        const double alpha = a[k][k] > 0.0 ? -norm : norm;
        a[k][k] -= alpha;
        double vnorm2 = 0.0;
        for (int i = k; i < rows; ++i) vnorm2 += a[i][k] * a[i][k];
        if (vnorm2 == 0.0) return false;
        for (int j = k + 1; j < cols; ++j) {
            double dot = 0.0;
            for (int i = k; i < rows; ++i) dot += a[i][k] * a[i][j];
            const double f = 2.0 * dot / vnorm2;
            for (int i = k; i < rows; ++i) a[i][j] -= f * a[i][k];
        }
        double dot = 0.0;
        for (int i = k; i < rows; ++i) dot += a[i][k] * b[i];
        const double f = 2.0 * dot / vnorm2;
        for (int i = k; i < rows; ++i) b[i] -= f * a[i][k];
        diag[k] = alpha;
    }
    for (int k = cols - 1; k >= 0; --k) {
        double acc = b[k];
        for (int j = k + 1; j < cols; ++j) acc -= a[k][j] * x[j];
        x[k] = acc / diag[k];
    }
    for (int j = 0; j < cols; ++j) x[j] *= scale[j];
    return true;
}

} // namespace lkit::linalg
