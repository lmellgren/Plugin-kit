#pragma once
// Small dense linear algebra on fixed-size arrays (no allocation, realtime safe):
// LU solve (real/complex), least squares (Householder QR), matrix exponential (Pade 6,6 + squaring),
// real eigenvalues (balance + Hessenberg + shifted QR) and null spaces (complete pivoting).
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

// Eigenvalues of a real N x N matrix: balancing, reduction to Hessenberg form by stabilised elementary
// similarity transforms, then the Francis double-shift QR iteration (the classic EISPACK balanc / elmhes / hqr).
// re[i] + j im[i]; a complex pair comes out as two adjacent entries with im of opposite sign.
// dscale (optional) receives the balancing scale D (balanced = D^-1 A D). Returns false if QR fails to converge.
template <int N>
bool eigenvalues(const double (&A)[N][N], double (&re)[N], double (&im)[N], double* dscale = nullptr) noexcept
{
    double a[N + 1][N + 1] = {};   // 1-based, as the reference algorithms
    for (int i = 0; i < N; ++i)
        for (int j = 0; j < N; ++j) a[i + 1][j + 1] = A[i][j];
    double d[N + 1];
    for (int i = 0; i <= N; ++i) d[i] = 1.0;
    // balance
    for (bool done = false; !done;) {
        done = true;
        for (int i = 1; i <= N; ++i) {
            double r = 0.0, c = 0.0;
            for (int j = 1; j <= N; ++j)
                if (j != i) c += std::fabs(a[j][i]), r += std::fabs(a[i][j]);
            if (c == 0.0 || r == 0.0) continue;
            double g = r / 2.0, f = 1.0;
            const double s = c + r;
            while (c < g) f *= 2.0, c *= 4.0;
            g = r * 2.0;
            while (c > g) f /= 2.0, c /= 4.0;
            if ((c + r) / f < 0.95 * s) {
                done = false;
                g = 1.0 / f;
                for (int j = 1; j <= N; ++j) a[i][j] *= g;
                for (int j = 1; j <= N; ++j) a[j][i] *= f;
                d[i] *= f;
            }
        }
    }
    if (dscale)
        for (int i = 0; i < N; ++i) dscale[i] = d[i + 1];
    // Hessenberg
    for (int m = 2; m < N; ++m) {
        double x = 0.0;
        int i = m;
        for (int j = m; j <= N; ++j)
            if (std::fabs(a[j][m - 1]) > std::fabs(x)) x = a[j][m - 1], i = j;
        if (i != m) {
            for (int j = m - 1; j <= N; ++j) std::swap(a[i][j], a[m][j]);
            for (int j = 1; j <= N; ++j) std::swap(a[j][i], a[j][m]);
        }
        if (x != 0.0)
            for (i = m + 1; i <= N; ++i) {
                double y = a[i][m - 1];
                if (y == 0.0) continue;
                y /= x;
                a[i][m - 1] = y;
                for (int j = m; j <= N; ++j) a[i][j] -= y * a[m][j];
                for (int j = 1; j <= N; ++j) a[j][m] += y * a[j][i];
            }
    }
    for (int i = 3; i <= N; ++i)
        for (int j = 1; j < i - 1; ++j) a[i][j] = 0.0;
    // shifted QR
    double anorm = 0.0;
    for (int i = 1; i <= N; ++i)
        for (int j = std::max(i - 1, 1); j <= N; ++j) anorm += std::fabs(a[i][j]);
    auto sign = [](double x, double y) { return y >= 0.0 ? std::fabs(x) : -std::fabs(x); };
    int nn = N, l = 1;
    double t = 0.0, p = 0.0, q = 0.0, r = 0.0, s = 0.0, w = 0.0, x = 0.0, y = 0.0, z = 0.0;
    double wr[N + 1] = {}, wi[N + 1] = {};
    while (nn >= 1) {
        int its = 0;
        do {
            for (l = nn; l >= 2; --l) {
                s = std::fabs(a[l - 1][l - 1]) + std::fabs(a[l][l]);
                if (s == 0.0) s = anorm;
                if (std::fabs(a[l][l - 1]) + s == s) {
                    a[l][l - 1] = 0.0;
                    break;
                }
            }
            x = a[nn][nn];
            if (l == nn) {
                wr[nn] = x + t;
                wi[nn--] = 0.0;
            } else {
                y = a[nn - 1][nn - 1];
                w = a[nn][nn - 1] * a[nn - 1][nn];
                if (l == nn - 1) {
                    p = 0.5 * (y - x);
                    q = p * p + w;
                    z = std::sqrt(std::fabs(q));
                    x += t;
                    if (q >= 0.0) {
                        z = p + sign(z, p);
                        wr[nn - 1] = wr[nn] = x + z;
                        if (z != 0.0) wr[nn] = x - w / z;
                        wi[nn - 1] = wi[nn] = 0.0;
                    } else {
                        wr[nn - 1] = wr[nn] = x + p;
                        wi[nn - 1] = -(wi[nn] = z);
                    }
                    nn -= 2;
                } else {
                    if (its == 60) return false;
                    if (its == 10 || its == 20 || its == 40) {
                        t += x;
                        for (int i = 1; i <= nn; ++i) a[i][i] -= x;
                        s = std::fabs(a[nn][nn - 1]) + std::fabs(a[nn - 1][nn - 2]);
                        y = x = 0.75 * s;
                        w = -0.4375 * s * s;
                    }
                    ++its;
                    int m = nn - 2;
                    for (; m >= l; --m) {
                        z = a[m][m];
                        r = x - z;
                        s = y - z;
                        p = (r * s - w) / a[m + 1][m] + a[m][m + 1];
                        q = a[m + 1][m + 1] - z - r - s;
                        r = a[m + 2][m + 1];
                        s = std::fabs(p) + std::fabs(q) + std::fabs(r);
                        p /= s, q /= s, r /= s;
                        if (m == l) break;
                        const double u = std::fabs(a[m][m - 1]) * (std::fabs(q) + std::fabs(r));
                        const double v = std::fabs(p) * (std::fabs(a[m - 1][m - 1]) + std::fabs(z) + std::fabs(a[m + 1][m + 1]));
                        if (u + v == v) break;
                    }
                    for (int i = m + 2; i <= nn; ++i) {
                        a[i][i - 2] = 0.0;
                        if (i != m + 2) a[i][i - 3] = 0.0;
                    }
                    for (int k = m; k <= nn - 1; ++k) {
                        if (k != m) {
                            p = a[k][k - 1];
                            q = a[k + 1][k - 1];
                            r = 0.0;
                            if (k != nn - 1) r = a[k + 2][k - 1];
                            if ((x = std::fabs(p) + std::fabs(q) + std::fabs(r)) != 0.0) p /= x, q /= x, r /= x;
                        }
                        if ((s = sign(std::sqrt(p * p + q * q + r * r), p)) != 0.0) {
                            if (k == m) {
                                if (l != m) a[k][k - 1] = -a[k][k - 1];
                            } else {
                                a[k][k - 1] = -s * x;
                            }
                            p += s;
                            x = p / s;
                            y = q / s;
                            z = r / s;
                            q /= p;
                            r /= p;
                            for (int j = k; j <= nn; ++j) {
                                p = a[k][j] + q * a[k + 1][j];
                                if (k != nn - 1) {
                                    p += r * a[k + 2][j];
                                    a[k + 2][j] -= p * z;
                                }
                                a[k + 1][j] -= p * y;
                                a[k][j] -= p * x;
                            }
                            const int mmin = nn < k + 3 ? nn : k + 3;
                            for (int i = l; i <= mmin; ++i) {
                                p = x * a[i][k] + y * a[i][k + 1];
                                if (k != nn - 1) {
                                    p += z * a[i][k + 2];
                                    a[i][k + 2] -= p * r;
                                }
                                a[i][k + 1] -= p * q;
                                a[i][k] -= p;
                            }
                        }
                    }
                }
            }
        } while (l < nn - 1);
    }
    for (int i = 0; i < N; ++i) re[i] = wr[i + 1], im[i] = wi[i + 1];
    for (int i = 0; i + 1 < N; ++i)   // a complex pair: positive imaginary part first
        if (im[i] < 0.0 && im[i + 1] > 0.0) std::swap(im[i], im[i + 1]), std::swap(re[i], re[i + 1]);
    return true;
}

// An orthonormal basis (columns of B) of the K-dimensional null space of M (rank N - K), by Gaussian
// elimination with complete pivoting. M is destroyed. Returns false if M has fewer than N - K usable pivots.
template <int N, int K>
bool nullspace(double (&M)[N][N], double (&B)[N][K]) noexcept
{
    constexpr int R = N - K;
    int perm[N];
    for (int j = 0; j < N; ++j) perm[j] = j;
    for (int r = 0; r < R; ++r) {
        int pi = r, pj = r;
        double best = 0.0;
        for (int i = r; i < N; ++i)
            for (int j = r; j < N; ++j)
                if (std::fabs(M[i][perm[j]]) > best) best = std::fabs(M[i][perm[j]]), pi = i, pj = j;
        if (best == 0.0) return false;
        for (int j = 0; j < N; ++j) std::swap(M[r][j], M[pi][j]);
        std::swap(perm[r], perm[pj]);
        for (int i = r + 1; i < N; ++i) {
            const double f = M[i][perm[r]] / M[r][perm[r]];
            for (int j = r; j < N; ++j) M[i][perm[j]] -= f * M[r][perm[j]];
        }
    }
    for (int k = 0; k < K; ++k) {
        double x[N] = {};
        x[perm[R + k]] = 1.0;
        for (int r = R - 1; r >= 0; --r) {
            double acc = 0.0;
            for (int c = r + 1; c < N; ++c) acc += M[r][perm[c]] * x[perm[c]];
            x[perm[r]] = -acc / M[r][perm[r]];
        }
        for (int kk = 0; kk < k; ++kk) {   // Gram-Schmidt (twice) against the previous vectors
            for (int pass = 0; pass < 2; ++pass) {
                double dot = 0.0;
                for (int i = 0; i < N; ++i) dot += B[i][kk] * x[i];
                for (int i = 0; i < N; ++i) x[i] -= dot * B[i][kk];
            }
        }
        double n2 = 0.0;
        for (int i = 0; i < N; ++i) n2 += x[i] * x[i];
        if (!(n2 > 0.0)) return false;
        const double inv = 1.0 / std::sqrt(n2);
        for (int i = 0; i < N; ++i) B[i][k] = x[i] * inv;
    }
    return true;
}

} // namespace lkit::linalg
