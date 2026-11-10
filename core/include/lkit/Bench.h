#pragma once
// Benchmark helpers for tools/bench (QA R5): warm-up, best-of-N timing, per-stage breakdown, and the
// budget unit "% of one core per stereo instance". Not for realtime code.
//
// Per-stage breakdown: a plugin's bench builds its core with a stage hook, or times each stage in
// isolation with timeStage(); report one CSV row per stage with row().

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <functional>

namespace lkit::bench {

using clk = std::chrono::steady_clock;

// Keep the thread busy for `seconds` so the scheduler moves it to a performance core at full clock.
inline void warmUp(double seconds, const std::function<void()>& work)
{
    const auto t0 = clk::now();
    while (std::chrono::duration<double>(clk::now() - t0).count() < seconds) work();
}

// Microseconds per call of `block` over `calls` calls, best of `rounds` (noise is one-sided).
inline double usPerCall(int calls, int rounds, const std::function<void()>& block,
                        const std::function<void()>& prepareRound = {})
{
    double best = 1e300;
    for (int r = 0; r < rounds; ++r) {
        if (prepareRound) prepareRound();
        const auto t0 = clk::now();
        for (int i = 0; i < calls; ++i) block();
        best = std::min(best, std::chrono::duration<double>(clk::now() - t0).count() / calls * 1e6);
    }
    return best;
}

// Share of one core for a block of `blockSize` samples at `fs` taking `us` microseconds.
inline double percentCore(double us, double fs, int blockSize) { return 100.0 * us * 1e-6 * fs / blockSize; }

inline void header() { std::printf("scope,fs,block,mode,stage,us_per_block,percent_core_per_stereo_instance\n"); }
inline void row(const char* scope, double fs, int block, const char* mode, const char* stage, double us)
{
    std::printf("%s,%.0f,%d,%s,%s,%.3f,%.4f\n", scope, fs, block, mode, stage, us, percentCore(us, fs, block));
}

} // namespace lkit::bench
