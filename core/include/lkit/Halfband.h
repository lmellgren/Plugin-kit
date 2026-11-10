#pragma once
// Linear-phase half-band FIR stages (Kaiser window) for 2x up/down-sampling, stereo (2 lanes).
// Designed in prepare() (may allocate); processing is allocation-free.

#include "lkit/Simd.h"

#include <vector>

namespace lkit {


// Half-band taps: passband to passHz, stopband from fsHigh/2 - passHz, attenuation attDb.
// Length 4m+3 (outermost taps nonzero), unity DC gain. Returns the centre index.
int designHalfband(double fsHigh, double passHz, double attDb, std::vector<double>& h);

class HalfbandStereo {
public:
    void design(double fsHigh, double passHz, double attDb);
    void reset();
    int centre() const { return centre_; }   // group delay in samples at fsHigh

    // Upsampling: one input sample -> two output samples (gain 2 applied).
    void up(d2 x, d2& y0, d2& y1) noexcept
    {
        // zero-stuffed input: y[2n] = 2 sum_k h[2k] x[n-k]; y[2n+1] = 2 h[c] x[n-(c-1)/2]
        posUp_ = (posUp_ == 0 ? nUp_ : posUp_) - 1;
        bufUp_[static_cast<size_t>(posUp_)] = bufUp_[static_cast<size_t>(posUp_ + nUp_)] = x;
        const d2* in = bufUp_.data() + posUp_;
        // symmetric taps folded: even[k] = even[nUp-1-k]
        d2 acc = 0.0;
        for (int k = 0; k < nHalf_; ++k) acc += half_[static_cast<size_t>(k)] * (in[k] + in[nUp_ - 1 - k]);
        y0 = acc;
        y1 = 2.0 * hc_ * in[(centre_ - 1) / 2];
    }
    // Downsampling: two input samples (in time order) -> one output sample.
    d2 down(d2 x0, d2 x1) noexcept
    {
        // y[n] = sum_i h[i] x[newest - i]; nonzero taps: even i (odd offsets) and the centre
        posDn_ = (posDn_ == 0 ? ringDn_ : posDn_) - 2;
        bufDn_[static_cast<size_t>(posDn_ + 1)] = bufDn_[static_cast<size_t>(posDn_ + 1 + ringDn_)] = x0;
        bufDn_[static_cast<size_t>(posDn_)] = bufDn_[static_cast<size_t>(posDn_ + ringDn_)] = x1;
        const d2* in = bufDn_.data() + posDn_;
        d2 acc = hc_ * in[centre_];
        for (int k = 0; k < nHalf_; ++k) acc += even_[static_cast<size_t>(k)] * (in[2 * k] + in[2 * (nUp_ - 1 - k)]);
        return acc;
    }

    // Block processing (same arithmetic as up()/down()): allocate for at most maxIn input samples
    // per call in prepareBlocks(); then upBlock: n -> 2n samples, downBlock: 2n -> n samples.
    void prepareBlocks(int maxIn);
    void upBlock(const d2* in, int n, d2* out) noexcept
    {
        const int h = nUp_ - 1;
        d2* x = workUp_.data();
        for (int i = 0; i < n; ++i) x[h + i] = in[i];
        switch (nHalf_) {   // short stages with compile-time tap counts
        case 4: upKernel<4>(x + h, n, out); break;
        case 5: upKernel<5>(x + h, n, out); break;
        case 6: upKernel<6>(x + h, n, out); break;
        case 16: upKernel<16>(x + h, n, out); break;
        default: upKernel<0>(x + h, n, out); break;
        }
        for (int i = 0; i < h; ++i) x[i] = x[n + i];
    }
    void downBlock(const d2* in, int n, d2* out) noexcept
    {
        const int h = len_ - 1;
        d2* x = workDn_.data();
        for (int i = 0; i < 2 * n; ++i) x[h + i] = in[i];
        switch (nHalf_) {
        case 4: downKernel<4>(x + h, n, out); break;
        case 5: downKernel<5>(x + h, n, out); break;
        case 6: downKernel<6>(x + h, n, out); break;
        case 16: downKernel<16>(x + h, n, out); break;
        default: downKernel<0>(x + h, n, out); break;
        }
        for (int i = 0; i < h; ++i) x[i] = x[2 * n + i];
    }

private:
    // NH > 0: compile-time number of folded taps; 0: runtime nHalf_.
    template <int NH>
    void upKernel(const d2* x, int n, d2* out) const noexcept
    {
        const int nh = NH > 0 ? NH : nHalf_;
        const int h = 2 * nh - 1;   // nUp - 1
        const int c1 = (centre_ - 1) / 2;
        const double g = 2.0 * hc_;
        const double* hf = half_.data();
        for (int i = 0; i < n; ++i) {
            const d2* p = x + i;   // newest
            d2 acc = 0.0;
            for (int k = 0; k < nh; ++k) acc += hf[k] * (p[-k] + p[-h + k]);
            out[2 * i] = acc;
            out[2 * i + 1] = g * p[-c1];
        }
    }
    template <int NH>
    void downKernel(const d2* x, int n, d2* out) const noexcept
    {
        const int nh = NH > 0 ? NH : nHalf_;
        const int span = 2 * (2 * nh - 1);
        const double* ev = even_.data();
        for (int i = 0; i < n; ++i) {
            const d2* p = x + 2 * i + 1;   // newest of the pair
            d2 acc = hc_ * p[-centre_];
            for (int k = 0; k < nh; ++k) acc += ev[k] * (p[-2 * k] + p[-span + 2 * k]);
            out[i] = acc;
        }
    }

    std::vector<d2> workUp_, workDn_;
    std::vector<double> even_;   // h[2k], k = 0..(len-1)/2
    std::vector<double> half_;   // 2 h[2k], k < nUp/2 (upsampling gain folded in)
    double hc_ = 0.5;
    int centre_ = 0, len_ = 0, nUp_ = 0, nHalf_ = 0, ringDn_ = 0, posUp_ = 0, posDn_ = 0;
    std::vector<d2> bufUp_, bufDn_;
};

// Legacy mono interface kept for tests and tools: a cascade of 2^k stages (as M5).
class Oversampler {
public:
    void prepare(double fs, int factor, double passHz = 20000.0, double attDb = 110.0);
    void reset();
    int factor() const { return factor_; }
    double latencyBase() const { return latencyBase_; }
    void up(double x, double* out) noexcept;
    double down(const double* in) noexcept;

private:
    std::vector<HalfbandStereo> stages_;
    std::vector<d2> tmpA_, tmpB_;
    int factor_ = 1;
    double latencyBase_ = 0.0;
};

} // namespace lkit
