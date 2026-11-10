#include "lkit/Halfband.h"

#include <algorithm>
#include <cmath>

namespace lkit {

namespace {
constexpr double kPi = 3.14159265358979323846;

double besselI0(double x)
{
    double sum = 1.0, term = 1.0;
    for (int k = 1; k < 200; ++k) {
        term *= (x / (2.0 * k)) * (x / (2.0 * k));
        sum += term;
        if (term < 1e-17 * sum) break;
    }
    return sum;
}
} // namespace

int designHalfband(double fsHigh, double passHz, double attDb, std::vector<double>& h)
{
    const double stopHz = fsHigh / 2.0 - passHz;
    const double dw = 2.0 * kPi * (stopHz - passHz) / fsHigh;
    const double beta = attDb > 50 ? 0.1102 * (attDb - 8.7) : 0.5842 * std::pow(attDb - 21, 0.4) + 0.07886 * (attDb - 21);
    const int n = static_cast<int>(std::ceil((attDb - 7.95) / (2.285 * dw))) + 1;
    int centre = (n - 1) / 2;
    if (centre % 2 == 0) ++centre;   // length 4m+3: outermost taps nonzero
    const int len = 2 * centre + 1;
    h.assign(static_cast<size_t>(len), 0.0);
    const double i0b = besselI0(beta);
    for (int i = 0; i < len; ++i) {
        const int d = i - centre;
        const double r = static_cast<double>(d) / centre;
        const double win = besselI0(beta * std::sqrt(std::max(0.0, 1.0 - r * r))) / i0b;
        const double sinc = d == 0 ? 0.5 : std::sin(kPi * d / 2.0) / (kPi * d);
        h[static_cast<size_t>(i)] = (d != 0 && d % 2 == 0) ? 0.0 : sinc * win;
    }
    double sum = 0.0;
    for (double v : h) sum += v;
    for (double& v : h) v /= sum;
    return centre;
}

void HalfbandStereo::design(double fsHigh, double passHz, double attDb)
{
    std::vector<double> h;
    centre_ = designHalfband(fsHigh, passHz, attDb, h);
    len_ = static_cast<int>(h.size());
    hc_ = h[static_cast<size_t>(centre_)];
    nUp_ = (len_ + 1) / 2;   // even indices 0, 2, ..., len-1
    even_.assign(static_cast<size_t>(nUp_), 0.0);
    for (int k = 0; k < nUp_; ++k) even_[static_cast<size_t>(k)] = h[static_cast<size_t>(2 * k)];
    nHalf_ = nUp_ / 2;   // nUp = 2m+2 is even for length 4m+3
    half_.assign(static_cast<size_t>(nHalf_), 0.0);
    for (int k = 0; k < nHalf_; ++k) half_[static_cast<size_t>(k)] = 2.0 * even_[static_cast<size_t>(k)];
    ringDn_ = len_ + 1;   // even ring length for 2-sample steps
    bufUp_.assign(static_cast<size_t>(2 * nUp_), d2{0.0, 0.0});
    bufDn_.assign(static_cast<size_t>(2 * ringDn_), d2{0.0, 0.0});
    reset();
}

void HalfbandStereo::prepareBlocks(int maxIn)
{
    workUp_.assign(static_cast<size_t>(nUp_ - 1 + maxIn), d2{0.0, 0.0});
    workDn_.assign(static_cast<size_t>(len_ - 1 + 2 * maxIn), d2{0.0, 0.0});
}

void HalfbandStereo::reset()
{
    std::fill(workUp_.begin(), workUp_.end(), d2{0.0, 0.0});
    std::fill(workDn_.begin(), workDn_.end(), d2{0.0, 0.0});
    std::fill(bufUp_.begin(), bufUp_.end(), d2{0.0, 0.0});
    std::fill(bufDn_.begin(), bufDn_.end(), d2{0.0, 0.0});
    posUp_ = posDn_ = 0;
}

void Oversampler::prepare(double fs, int factor, double passHz, double attDb)
{
    factor_ = 1;
    int stages = 0;
    while (factor_ < factor) factor_ *= 2, ++stages;
    stages_.assign(static_cast<size_t>(stages), HalfbandStereo{});
    latencyBase_ = 0.0;
    double fh = fs;
    for (int i = 0; i < stages; ++i) {
        fh *= 2.0;
        stages_[static_cast<size_t>(i)].design(fh, std::min(passHz, 0.45 * fs), attDb);
        // up: centre samples at fh; down: centre - 1 (it reads the newest high sample)
        latencyBase_ += (2.0 * stages_[static_cast<size_t>(i)].centre() - 1.0) * fs / fh;
    }
    tmpA_.assign(static_cast<size_t>(factor_), d2{0.0, 0.0});
    tmpB_.assign(static_cast<size_t>(factor_), d2{0.0, 0.0});
}

void Oversampler::reset()
{
    for (auto& s : stages_) s.reset();
}

void Oversampler::up(double x, double* out) noexcept
{
    d2* cur = tmpA_.data();
    d2* nxt = tmpB_.data();
    cur[0] = d2{x, 0.0};
    int n = 1;
    for (auto& st : stages_) {
        for (int i = 0; i < n; ++i) st.up(cur[i], nxt[2 * i], nxt[2 * i + 1]);
        n *= 2;
        std::swap(cur, nxt);
    }
    for (int i = 0; i < n; ++i) out[i] = cur[i].x;
}

double Oversampler::down(const double* in) noexcept
{
    d2* cur = tmpA_.data();
    d2* nxt = tmpB_.data();
    int n = factor_;
    for (int i = 0; i < n; ++i) cur[i] = d2{in[i], 0.0};
    for (int s = static_cast<int>(stages_.size()) - 1; s >= 0; --s) {
        for (int i = 0; i < n / 2; ++i) nxt[i] = stages_[static_cast<size_t>(s)].down(cur[2 * i], cur[2 * i + 1]);
        n /= 2;
        std::swap(cur, nxt);
    }
    return cur[0].x;
}

} // namespace lkit
