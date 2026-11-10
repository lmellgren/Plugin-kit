#pragma once
// True bypass (house standard): when off, the output is the input delayed by the plugin's reported
// latency, bit for bit; switching crossfades linearly over 10 ms. Extracted from the LOMAS Passive EQ
// wrapper (EQ In) and made independent of JUCE.
//
// Per chunk (m <= maxBlock samples):
//   bypass.capture(input, nch, m);           // before the wet path overwrites the input
//   ... wet path writes the processed signal into `io` ...
//   bypass.render(io, nch, m, on);           // io = mix * wet + (1 - mix) * dry, or exactly dry
// No allocation after prepare().

#include <algorithm>
#include <cstring>
#include <vector>

namespace lkit {

class TrueBypass {
public:
    static constexpr int kMaxChannels = 2;

    void prepare(double sampleRate, int maxBlock, int latency, bool on, double fadeSeconds = 0.010)
    {
        latency_ = std::max(0, latency);
        maxBlock_ = std::max(1, maxBlock);
        for (auto& d : dry_) d.assign(static_cast<size_t>(latency_ + maxBlock_), 0.0f);
        mix_ = on ? 1.0 : 0.0;
        step_ = 1.0 / std::max(1.0, fadeSeconds * sampleRate);
    }
    void reset() noexcept
    {
        for (auto& d : dry_) std::fill(d.begin(), d.end(), 0.0f);
    }
    int latency() const noexcept { return latency_; }
    int maxBlock() const noexcept { return maxBlock_; }
    bool fullyOn() const noexcept { return mix_ >= 1.0; }
    bool fullyOff() const noexcept { return mix_ <= 0.0; }
    double mix() const noexcept { return mix_; }

    // Store the unprocessed input of this chunk (m <= maxBlock).
    void capture(const float* const* in, int nch, int m) noexcept
    {
        for (int ch = 0; ch < std::min(nch, kMaxChannels); ++ch)
            std::memcpy(dry_[static_cast<size_t>(ch)].data() + latency_, in[ch], sizeof(float) * static_cast<size_t>(m));
    }
    // Delayed dry sample i of this chunk (valid between capture() and render()).
    float dry(int ch, int i) const noexcept { return dry_[static_cast<size_t>(ch)][static_cast<size_t>(i)]; }

    // io holds the wet signal; on return it holds the output. Then advances the delay line by m.
    void render(float* const* io, int nch, int m, bool on) noexcept
    {
        nch = std::min(nch, kMaxChannels);
        const double target = on ? 1.0 : 0.0;
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wfloat-equal"
        const bool steady = mix_ == target;   // exact: the ramp clamps to 0 or 1
#pragma clang diagnostic pop
        if (steady) {
            if (!on)
                for (int ch = 0; ch < nch; ++ch)
                    std::memcpy(io[ch], dry_[static_cast<size_t>(ch)].data(), sizeof(float) * static_cast<size_t>(m));
        } else {
            double mix = mix_;
            for (int i = 0; i < m; ++i) {
                mix = mix < target ? std::min(target, mix + step_) : std::max(target, mix - step_);
                for (int ch = 0; ch < nch; ++ch) {
                    const float d = dry_[static_cast<size_t>(ch)][static_cast<size_t>(i)];
                    io[ch][i] = mix <= 0.0 ? d : (mix >= 1.0 ? io[ch][i] : static_cast<float>(mix * io[ch][i] + (1.0 - mix) * d));
                }
            }
            mix_ = mix;
        }
        for (int ch = 0; ch < nch; ++ch) {
            float* d = dry_[static_cast<size_t>(ch)].data();
            std::memmove(d, d + m, sizeof(float) * static_cast<size_t>(latency_));
        }
    }

private:
    std::vector<float> dry_[kMaxChannels];
    int latency_ = 0, maxBlock_ = 1;
    double mix_ = 1.0, step_ = 1.0 / 480.0;
};

} // namespace lkit
