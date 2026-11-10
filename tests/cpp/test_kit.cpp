// Plugin-kit core tests (no JUCE): linear algebra, half-band resampler, FOH engine, true bypass,
// smoothing, golden files.
#include "lkit/Foh.h"
#include "lkit/Golden.h"
#include "lkit/Halfband.h"
#include "lkit/LinAlg.h"
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
    testTrueBypass();
    testSmoothed();
    testGolden();
    if (failures == 0) std::puts("all kit tests passed");
    return failures == 0 ? 0 : 1;
}
