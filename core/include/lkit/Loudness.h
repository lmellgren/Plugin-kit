#pragma once
// Static loudness of a filter's response, for a static Auto Gain that compensates an EQ curve (QA X3): the
// K-weighted (ITU-R BS.1770) power of pink noise through the response, relative to no filter. Extracted from
// LOMAS Passive EQ (PassiveEq::kWeightingPower / autoGainDb, PassiveEQ@ec407da), unchanged in substance.
// Message thread only (not for the audio thread's inner loop).

#include <cmath>
#include <complex>

namespace lkit::loudness {

// |K(j 2 pi f)|^2: the BS.1770 K-weighting as analog prototypes, the high shelf (+4 dB above about 1.7 kHz) and
// the RLB high-pass (about 38 Hz); the analog forms of the standard 48 kHz biquads.
inline double kWeightingPower(double f) noexcept
{
    using cd = std::complex<double>;
    constexpr double pi = 3.14159265358979323846;
    const cd s(0.0, 2.0 * pi * f);
    const double w0 = 2.0 * pi * 1681.974450955533, q0 = 0.7071752369554193;
    const double vh = std::pow(10.0, 3.99984385397 / 20.0), vb = std::pow(vh, 0.4996667741545416);
    const cd shelf = (vh * s * s + vb * (w0 / q0) * s + w0 * w0) / (s * s + (w0 / q0) * s + w0 * w0);
    const double w1 = 2.0 * pi * 38.13547087602444, q1 = 0.5003270373238773;
    const cd hp = s * s / (s * s + (w1 / q1) * s + w1 * w1);
    return std::norm(shelf * hp);
}

// Loudness change (dB) of pink noise through a response: equal power per octave (a log grid, 1/12 octave from
// 20 Hz to 20 kHz), each point weighted by K-weighting. powerAt(f) returns |H(f)|^2. The compensating Auto
// Gain is the negative of this.
template <typename PowerAt>
double pinkKWeightedDb(PowerAt&& powerAt) noexcept
{
    constexpr int n = 120;
    double acc = 0.0, wsum = 0.0;
    for (int i = 0; i < n; ++i) {
        const double f = 20.0 * std::pow(1000.0, i / static_cast<double>(n - 1));
        const double w = kWeightingPower(f);
        acc += w * powerAt(f);
        wsum += w;
    }
    return 10.0 * std::log10(acc / wsum);
}

} // namespace lkit::loudness
