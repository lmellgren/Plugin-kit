#pragma once
// One-pole parameter smoothing that snaps to its target (so steady-state fast paths can engage),
// and a lock-free peak meter. Realtime safe.

#include <algorithm>
#include <array>
#include <atomic>
#include <cmath>

namespace lkit {

class Smoothed {
public:
    void prepare(double sampleRate, double timeSeconds, double initial) noexcept
    {
        coef_ = 1.0 - std::exp(-1.0 / std::max(1e-9, timeSeconds * sampleRate));
        value_ = target_ = initial;
    }
    void setTarget(double t) noexcept { target_ = t; }
    void snap() noexcept { value_ = target_; }
    bool settled() const noexcept { return value_ == target_; }
    double value() const noexcept { return value_; }
    double target() const noexcept { return target_; }
    double next() noexcept
    {
        value_ += coef_ * (target_ - value_);
        return value_;
    }
    // Call after a run of next(): a value within 1e-9 (relative) of the target snaps to it.
    void settle() noexcept
    {
        if (std::fabs(value_ - target_) <= 1e-9 * std::max(1e-30, std::fabs(target_))) value_ = target_;
    }

private:
    double value_ = 0.0, target_ = 0.0, coef_ = 1.0;
};

inline double dbToGain(double db) noexcept { return std::pow(10.0, db / 20.0); }
inline double gainToDb(double g) noexcept { return 20.0 * std::log10(std::max(g, 1e-30)); }

// Peak per channel since the last read (audio thread writes, message thread reads).
class PeakMeter {
public:
    void push(int ch, float peak) noexcept
    {
        auto& p = peaks_[static_cast<size_t>(ch & 1)];
        float prev = p.load(std::memory_order_relaxed);
        while (peak > prev && !p.compare_exchange_weak(prev, peak)) {}
    }
    float take(int ch) noexcept { return peaks_[static_cast<size_t>(ch & 1)].exchange(0.0f); }

private:
    std::array<std::atomic<float>, 2> peaks_{};
};

} // namespace lkit
