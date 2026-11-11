// Plugin-kit core tests (no JUCE): linear algebra, half-band resampler, FOH engine, true bypass,
// smoothing, golden files.
#include "lkit/Circuit.h"
#include "lkit/Foh.h"
#include "lkit/Golden.h"
#include "lkit/Halfband.h"
#include "lkit/LinAlg.h"
#include "lkit/Modal.h"
#include "lkit/Smooth.h"
#include "lkit/TrueBypass.h"

#include <cmath>
#include <complex>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <random>
#include <vector>

static int failures = 0;
#define CHECK(cond)                                                                       \
    do {                                                                                  \
        if (!(cond)) {                                                                    \
            std::fprintf(stderr, "%s:%d: CHECK failed: %s\n", __FILE__, __LINE__, #cond); \
            ++failures;                                                                   \
        }                                                                                 \
    } while (0)

constexpr double kPi = 3.14159265358979323846;
using cd = std::complex<double>;

static void testExpm()
{
    // exp([[0, -w], [w, 0]]) = rotation by w
    const double w = 1.3;
    double A[2][2] = {{0, -w}, {w, 0}}, E[2][2];
    CHECK(lkit::linalg::expm<2>(A, E));
    CHECK(std::fabs(E[0][0] - std::cos(w)) < 1e-13 && std::fabs(E[1][0] - std::sin(w)) < 1e-13);
    // large norm (scaling and squaring)
    double B[2][2] = {{-50, 0}, {0, 3}}, F[2][2];
    CHECK(lkit::linalg::expm<2>(B, F));
    CHECK(std::fabs(F[0][0] / std::exp(-50.0) - 1.0) < 1e-10 && std::fabs(F[1][1] / std::exp(3.0) - 1.0) < 1e-12);
}

static void testLstsq()
{
    // fit y = 2 - 3x exactly
    double a[6][2], b[6], x[2];
    for (int i = 0; i < 6; ++i) a[i][0] = 1.0, a[i][1] = i, b[i] = 2.0 - 3.0 * i;
    const bool solved = lkit::linalg::lstsq<6, 2>(a, b, 6, 2, x);
    CHECK(solved);
    CHECK(std::fabs(x[0] - 2.0) < 1e-12 && std::fabs(x[1] + 3.0) < 1e-12);
}

static void testHalfband()
{
    // 2x up then down of a 1 kHz sine at 48 kHz: unity gain, latency = 2 centre - 1 (high rate) / 2
    lkit::HalfbandStereo hb;
    hb.design(96000.0, 20000.0, 100.0);
    const int n = 4800;
    double maxErr = 0.0;
    const double lat = (2.0 * hb.centre() - 1.0) / 2.0;   // base-rate samples
    for (int i = 0; i < n; ++i) {
        const double x = std::sin(2 * kPi * 1000.0 * i / 48000.0);
        lkit::d2 a, b;
        hb.up(lkit::d2{x, -x}, a, b);
        const lkit::d2 y = hb.down(a, b);
        if (i > 400) {
            const double ref = std::sin(2 * kPi * 1000.0 * (i - lat) / 48000.0);
            maxErr = std::max({maxErr, std::fabs(y[0] - ref), std::fabs(y[1] + ref)});
        }
    }
    CHECK(maxErr < 1e-4);
    // stopband: an image at 96k - 25 kHz is attenuated >= ~100 dB by the design
    std::vector<double> h;
    const int c = lkit::designHalfband(96000.0, 20000.0, 100.0, h);
    cd H = 0.0;
    for (size_t k = 0; k < h.size(); ++k) H += h[k] * std::exp(cd(0, -2 * kPi * 28000.0 / 96000.0 * (double(k) - c)));
    CHECK(20 * std::log10(std::abs(H)) < -95.0);
    // block processing = sample processing
    lkit::HalfbandStereo s1, s2;
    s1.design(96000.0, 20000.0, 80.0);
    s2.design(96000.0, 20000.0, 80.0);
    s2.prepareBlocks(64);
    std::vector<lkit::d2> in(64), up1(128), up2(128);
    for (int i = 0; i < 64; ++i) in[static_cast<size_t>(i)] = lkit::d2{std::sin(0.1 * i), std::cos(0.07 * i)};
    for (int i = 0; i < 64; ++i) s1.up(in[static_cast<size_t>(i)], up1[static_cast<size_t>(2 * i)], up1[static_cast<size_t>(2 * i + 1)]);
    s2.upBlock(in.data(), 64, up2.data());
    double d = 0.0;
    for (int i = 0; i < 128; ++i) d = std::max(d, std::fabs(up1[static_cast<size_t>(i)][0] - up2[static_cast<size_t>(i)][0]));
    CHECK(d < 1e-15);
}

// Series RLC band-pass + load: R1 -> L -> C -> out, Rl to ground. States [i_L, v_C, v_Cl].
static lkit::StateSpace<3> rlc()
{
    const double R1 = 600, L = 0.05, C = 100e-9, Rl = 10e3, Cl = 2e-9;
    lkit::StateSpace<3> ss;
    // node out voltage v = v_Cl; i_L flows through C into out; KCL out: i_L = v/Rl + Cl dv/dt
    // L di/dt = u - R1 i - v_C - v
    ss.A[0][0] = -R1 / L, ss.A[0][1] = -1.0 / L, ss.A[0][2] = -1.0 / L;
    ss.A[1][0] = 1.0 / C;
    ss.A[2][0] = 1.0 / Cl, ss.A[2][2] = -1.0 / (Rl * Cl);
    ss.B[0] = 1.0 / L;
    ss.C[2] = 1.0;
    return ss;
}

static void testFoh()
{
    const auto ss = rlc();
    for (double fs : {88200.0, 96000.0}) {
        lkit::FohDesigner<3> d;
        d.prepare(fs, 6, 20.0, 20000.0);
        const double delay = 3.0;   // samples, built into the fit
        lkit::FohCoeffs<3> c;
        CHECK(d.design(ss, [&](double f) { return ss.response(f) * std::exp(cd(0, -2 * kPi * f * delay / fs)); }, c));
        double maxDb = 0.0, maxDeg = 0.0;
        for (int i = 0; i <= 200; ++i) {
            const double f = 20.0 * std::pow(1000.0, i / 200.0);
            const cd ref = ss.response(f) * std::exp(cd(0, -2 * kPi * f * delay / fs));
            const cd got = c.responseAt(f, fs);
            maxDb = std::max(maxDb, std::fabs(20 * std::log10(std::abs(got / ref))));
            maxDeg = std::max(maxDeg, std::fabs(std::arg(got / ref)) * 180 / kPi);
        }
        std::printf("FOH fs %.0f: max %.4f dB, %.3f deg (20 Hz-20 kHz)\n", fs, maxDb, maxDeg);
        CHECK(maxDb < 0.01 && maxDeg < 0.2);

        // realtime filter = designed response (sine through the filter)
        lkit::FohFilter<3> flt;
        flt.setCoefficients(c);
        flt.reset();
        const double f0 = 1000.0;
        double num = 0.0, den = 0.0;
        cd acc = 0.0;
        const int n = static_cast<int>(fs);
        for (int i = 0; i < n; ++i) {
            const double x = std::sin(2 * kPi * f0 * i / fs);
            const double y = flt.tick(lkit::d2{x, x})[0];
            if (i >= n / 2) {
                acc += y * std::exp(cd(0, -2 * kPi * f0 * i / fs));
                num += 1;
            }
        }
        (void)den;
        const cd hm = acc / (num / 2.0) * cd(0, 1);   // sin -> complex amplitude
        const cd hd = c.responseAt(f0, fs);
        CHECK(std::fabs(20 * std::log10(std::abs(hm / hd))) < 1e-3);
    }
}

static void testEigen()
{
    // companion matrix of (s + 1)(s + 2)(s^2 + 2 s + 5): roots -1, -2, -1 +- 2j
    double A[4][4] = {{-5, -13, -19, -10}, {1, 0, 0, 0}, {0, 1, 0, 0}, {0, 0, 1, 0}}, re[4], im[4];
    CHECK(lkit::linalg::eigenvalues<4>(A, re, im));
    int found = 0;
    for (int i = 0; i < 4; ++i) {
        const cd l(re[i], im[i]);
        for (const cd r : {cd(-1, 0), cd(-2, 0), cd(-1, 2), cd(-1, -2)})
            if (std::abs(l - r) < 1e-9) ++found;
    }
    CHECK(found == 4);
    // random 6 x 6: det(A - l I) ~ 0 for every eigenvalue, trace preserved
    std::mt19937 rng(7);
    std::uniform_real_distribution<double> u(-1.0, 1.0);
    for (int t = 0; t < 50; ++t) {
        double B[6][6], r6[6], i6[6], tr = 0.0, sr = 0.0;
        for (int i = 0; i < 6; ++i)
            for (int j = 0; j < 6; ++j) B[i][j] = u(rng) * std::pow(10.0, 3.0 * u(rng));
        for (int i = 0; i < 6; ++i) tr += B[i][i];
        CHECK(lkit::linalg::eigenvalues<6>(B, r6, i6));
        for (int i = 0; i < 6; ++i) sr += r6[i];
        CHECK(std::fabs(tr - sr) < 1e-8 * (1.0 + std::fabs(tr)) * 1e3);
        double nb = 0.0;
        for (auto& r : B)
            for (double v : r) nb = std::max(nb, std::fabs(v));
        for (int i = 0; i < 6; ++i) {   // inverse iteration: (B - l I) x ~ 0 for a unit x
            const cd l(r6[i], i6[i]), sh = l + cd(1e-9, 1e-9) * nb;
            cd M[6][6], R[6][1];
            for (int a = 0; a < 6; ++a) {
                for (int b = 0; b < 6; ++b) M[a][b] = B[a][b] - (a == b ? sh : cd(0.0));
                R[a][0] = 1.0 + 0.1 * a;
            }
            CHECK((lkit::linalg::luSolve<6, 1, cd>(M, R)));
            double xn = 0.0, res = 0.0;
            for (auto& r : R) xn += std::norm(r[0]);
            xn = std::sqrt(xn);
            for (int a = 0; a < 6; ++a) {
                cd acc = 0.0;
                for (int b = 0; b < 6; ++b) acc += (B[a][b] - (a == b ? l : cd(0.0))) * R[b][0] / xn;
                res = std::max(res, std::abs(acc));
            }
            CHECK(res < 1e-7 * nb);
        }
    }
    // null space of a rank-4 6 x 6 matrix
    double P[6][4], Q[4][6], M[6][6], Bn[6][2];
    for (auto& r : P)
        for (double& v : r) v = u(rng);
    for (auto& r : Q)
        for (double& v : r) v = u(rng);
    for (int i = 0; i < 6; ++i)
        for (int j = 0; j < 6; ++j) {
            M[i][j] = 0.0;
            for (int k = 0; k < 4; ++k) M[i][j] += P[i][k] * Q[k][j];
        }
    double M0[6][6];
    std::memcpy(M0, M, sizeof M);
    CHECK((lkit::linalg::nullspace<6, 2>(M, Bn)));
    double worst = 0.0;
    for (int k = 0; k < 2; ++k)
        for (int i = 0; i < 6; ++i) {
            double acc = 0.0;
            for (int j = 0; j < 6; ++j) acc += M0[i][j] * Bn[j][k];
            worst = std::max(worst, std::fabs(acc));
        }
    CHECK(worst < 1e-12);
}

// Six states with a complex pair, a near-repeated real pair and two separate real poles (rad/s), mixed by a
// random well-conditioned basis.
static lkit::StateSpace<6> sixStates()
{
    const double blocks[3][2][2] = {{{-3000, -20000}, {20000, -3000}}, {{-5650, 1}, {0, -5640}}, {{-60, 0}, {0, -4.4e5}}};
    double J[6][6] = {}, P[6][6], Pi[6][6];
    for (int b = 0; b < 3; ++b)
        for (int i = 0; i < 2; ++i)
            for (int k = 0; k < 2; ++k) J[2 * b + i][2 * b + k] = blocks[b][i][k];
    std::mt19937 rng(3);
    std::uniform_real_distribution<double> u(-0.5, 0.5);
    for (int i = 0; i < 6; ++i)
        for (int j = 0; j < 6; ++j) P[i][j] = (i == j ? 1.0 : 0.0) + u(rng), Pi[i][j] = i == j ? 1.0 : 0.0;
    double Pc[6][6];
    std::memcpy(Pc, P, sizeof P);
    lkit::linalg::luSolve<6, 6>(Pc, Pi);
    lkit::StateSpace<6> ss;
    double PJ[6][6];
    lkit::linalg::matmul<6>(P, J, PJ);
    lkit::linalg::matmul<6>(PJ, Pi, ss.A);
    for (int i = 0; i < 6; ++i) ss.B[i] = 1000.0 * (1.0 + u(rng)), ss.C[i] = 1.0 + u(rng);
    ss.D = 0.3;
    return ss;
}

static void testModal()
{
    auto check = [](auto ss, auto designer, auto coeffs, const char* name) {
        for (double fs : {88200.0, 96000.0}) {
            designer.prepare(fs, 20000.0);
            for (int delay : {1, 2}) {
                CHECK(designer.design(ss, delay, coeffs));
                double maxDb = 0.0, maxDeg = 0.0, an = 0.0;
                for (int i = 0; i <= 200; ++i) {
                    const double f = 20.0 * std::pow(1000.0, i / 200.0);
                    const cd ref = ss.response(f) * std::exp(cd(0, -2 * kPi * f * delay / fs));
                    const cd got = coeffs.response(std::exp(cd(0, -2 * kPi * f / fs)));
                    maxDb = std::max(maxDb, std::fabs(20 * std::log10(std::abs(got / ref))));
                    maxDeg = std::max(maxDeg, std::fabs(std::arg(got / ref)) * 180 / kPi);
                    an = std::max(an, std::abs(coeffs.analog(f) / ss.response(f) - 1.0));
                }
                std::printf("Modal %s fs %.0f delay %d: max %.4f dB, %.3f deg; modal analog %.1e\n", name, fs, delay, maxDb, maxDeg, an);
                CHECK(an < 1e-9);
                CHECK(maxDb < (delay == 2 ? 0.02 : 0.1) && maxDeg < (delay == 2 ? 0.2 : 1.0));
            }
        }
    };
    check(sixStates(), lkit::ModalDesigner<6, 2>{}, lkit::ModalCoeffs<6, 2>{}, "6 states, 2x2 blocks");
    check(rlc(), lkit::ModalDesigner<3, 3>{}, lkit::ModalCoeffs<3, 3>{}, "RLC, one block");

    // realtime filter = designed response; a coefficient change keeps the physical states
    const auto ss = sixStates();
    const double fs = 96000.0;
    lkit::ModalDesigner<6, 2> d;
    d.prepare(fs, 20000.0);
    lkit::ModalCoeffs<6, 2> c1, c2;
    CHECK(d.design(ss, 2, c1));
    auto ss2 = ss;
    for (auto& r : ss2.A)
        for (double& v : r) v *= 1.3;   // another setting
    CHECK(d.design(ss2, 2, c2));
    lkit::ModalFilter<6, 2> flt;
    flt.setCoefficients(c1);
    flt.reset();
    const double f0 = 1000.0;
    cd acc = 0.0;
    const int n = static_cast<int>(fs);
    for (int i = 0; i < n; ++i) {
        const double x = std::sin(2 * kPi * f0 * i / fs);
        const double y = flt.tick(lkit::d2{x, -x})[0];
        if (i >= n / 2) acc += y * std::exp(cd(0, -2 * kPi * f0 * i / fs));
    }
    const cd hm = acc / (n / 4.0) * cd(0, 1);
    CHECK(std::fabs(20 * std::log10(std::abs(hm / c1.response(std::exp(cd(0, -2 * kPi * f0 / fs)))))) < 1e-3);
    const auto x1 = flt.physicalState();
    flt.setCoefficients(c2);
    const auto x2 = flt.physicalState();
    double dx = 0.0, xm = 0.0;
    for (int i = 0; i < 6; ++i)
        for (int l = 0; l < 2; ++l) dx = std::max(dx, std::fabs(x1[i][l] - x2[i][l])), xm = std::max(xm, std::fabs(x1[i][l]));
    std::printf("Modal: physical state carried over to %.1e (relative)\n", dx / xm);
    CHECK(dx <= 1e-10 * xm);
    // the physical state equals the FOH state of the dense filter run on the same input
    lkit::FohDesigner<6> fd;
    fd.prepare(fs, 1);
    lkit::FohCoeffs<6> fc;
    CHECK(fd.discretise(ss, fc));
    lkit::FohFilter<6> dense;
    dense.setCoefficients(fc);
    lkit::ModalFilter<6, 2> m2;
    m2.setCoefficients(c1);
    double e = 0.0, s = 0.0;
    for (int i = 0; i < 2000; ++i) {
        const double x = std::sin(2 * kPi * 3000.0 * i / fs) + 0.3 * std::sin(2 * kPi * 70.0 * i / fs);
        dense.tick(lkit::d2{x, x});
        m2.tick(lkit::d2{x, x});
        const auto xd = dense.state(), xmod = m2.physicalState();
        for (int k = 0; k < 6; ++k) e = std::max(e, std::fabs(xd[k][0] - xmod[k][0])), s = std::max(s, std::fabs(xd[k][0]));
    }
    std::printf("Modal: physical states vs dense FOH %.1e (relative)\n", e / s);
    CHECK(e <= 1e-9 * s);
}

static void testCircuit()
{
    // The rlc() network built as a netlist: same response as the hand-written state space; then a VCVS
    // inverting amplifier (gain -Rf/Ri) and a value change.
    lkit::Circuit<8, 12> c;
    const int in = c.node(), n1 = c.node(), n2 = c.node(), out = c.node();
    c.R(in, n1, 600);
    c.L(n1, n2, 0.05);
    c.C(n2, out, 100e-9);
    c.R(out, 0, 10e3);
    c.C(out, 0, 2e-9);
    c.setInput(in);
    c.setOutput(out);
    CHECK(c.numStates() == 3);
    lkit::StateSpace<3> ss;
    CHECK(c.stateSpace(ss));
    const auto ref = rlc();
    double worst = 0.0;
    for (double f : {20.0, 200.0, 711.0, 2000.0, 20000.0})
        worst = std::max(worst, std::abs(ss.response(f) / ref.response(f) - 1.0));
    CHECK(worst < 1e-9);

    lkit::Circuit<6, 8> a;
    const int ai = a.node(), x = a.node(), ao = a.node();
    const int ri = a.R(ai, x, 10e3);
    a.R(ao, x, 22e3);
    a.C(ao, x, 1e-9);
    a.E(ao, 0, 0, x, 1e7);
    a.setInput(ai);
    a.setOutput(ao);
    lkit::StateSpace<1> s1;
    CHECK(a.stateSpace(s1));
    CHECK(std::fabs(std::abs(s1.response(10.0)) - 2.2) < 1e-5);
    a.setValue(ri, 22e3);
    CHECK(a.stateSpace(s1));
    CHECK(std::fabs(std::abs(s1.response(10.0)) - 1.0) < 1e-5);
    const double fc = 1.0 / (2 * kPi * 22e3 * 1e-9);   // pole of Rf || Cf
    CHECK(std::fabs(20 * std::log10(std::abs(s1.response(fc))) + 3.0103) < 1e-3);
}

static void testTrueBypass()
{
    for (int latency : {0, 1, 37, 600})
        for (int block : {16, 333, 512}) {
            lkit::TrueBypass bp;
            bp.prepare(48000.0, block, latency, false);
            std::mt19937 rng(7);
            std::normal_distribution<float> nd(0.0f, 0.3f);
            const int total = 6000;
            std::vector<float> in(2 * total), out(2 * total);
            for (auto& v : in) v = nd(rng);
            std::vector<float> l(static_cast<size_t>(block)), r(static_cast<size_t>(block));
            for (int pos = 0; pos < total; pos += block) {
                const int m = std::min(block, total - pos);
                for (int i = 0; i < m; ++i) l[static_cast<size_t>(i)] = in[static_cast<size_t>(pos + i)], r[static_cast<size_t>(i)] = in[static_cast<size_t>(total + pos + i)];
                float* io[2] = {l.data(), r.data()};
                bp.capture(io, 2, m);
                for (int i = 0; i < m; ++i) l[static_cast<size_t>(i)] *= 0.5f, r[static_cast<size_t>(i)] = 9.0f;   // "wet"
                bp.render(io, 2, m, false);
                for (int i = 0; i < m; ++i) out[static_cast<size_t>(pos + i)] = l[static_cast<size_t>(i)], out[static_cast<size_t>(total + pos + i)] = r[static_cast<size_t>(i)];
            }
            bool exact = true;
            for (int c = 0; c < 2; ++c)
                for (int i = 0; i < total; ++i) {
                    const float want = i < latency ? 0.0f : in[static_cast<size_t>(c * total + i - latency)];
                    const float got = out[static_cast<size_t>(c * total + i)];
                    exact = exact && std::memcmp(&want, &got, sizeof(float)) == 0;
                }
            CHECK(exact);
        }
    // crossfade length: on -> off takes 10 ms
    lkit::TrueBypass bp;
    bp.prepare(48000.0, 64, 5, true);
    std::vector<float> l(64, 1.0f), r(64, 1.0f);
    float* io[2] = {l.data(), r.data()};
    int blocks = 0;
    while (!bp.fullyOff() && blocks < 100) {
        std::fill(l.begin(), l.end(), 1.0f);
        bp.capture(io, 2, 64);
        bp.render(io, 2, 64, false);
        ++blocks;
    }
    CHECK(blocks == 8);   // 480 samples / 64 = 7.5 -> 8 blocks
}

static void testSmoothed()
{
    lkit::Smoothed s;
    s.prepare(48000.0, 0.02, 0.0);
    s.setTarget(1.0);
    for (int i = 0; i < 48000; ++i) s.next();
    s.settle();
    CHECK(s.settled() && s.value() == 1.0);
}

static void testGolden()
{
    namespace fs = std::filesystem;
    const auto dir = fs::temp_directory_path() / "lkit_golden_test";
    fs::create_directories(dir);
    lkit::golden::Store g(dir.string());
    std::vector<float> a = {0.1f, -0.2f, 0.3f, 0.4f};
    setenv("LKIT_UPDATE_GOLDEN", "1", 1);
    CHECK(g.check("t", a.data(), 2, 2, 0.0).ok);
    unsetenv("LKIT_UPDATE_GOLDEN");
    CHECK(g.check("t", a.data(), 2, 2, 0.0).ok);
    a[3] += 1e-3f;
    const auto r = g.check("t", a.data(), 2, 2, 1e-4);
    CHECK(!r.ok && r.maxDiff > 9e-4);
    CHECK(!g.check("missing", a.data(), 2, 2, 1.0).ok);
    fs::remove_all(dir);
}

int main()
{
    testExpm();
    testLstsq();
    testHalfband();
    testFoh();
    testEigen();
    testModal();
    testCircuit();
    testTrueBypass();
    testSmoothed();
    testGolden();
    if (failures == 0) std::puts("all kit tests passed");
    return failures == 0 ? 0 : 1;
}
