#pragma once
// Plugin wrapper base (house standard), extracted from the LOMAS Passive EQ wrapper.
//
// Gives every LOMAS plugin the same outer shell:
//   * On/off = true bypass: off passes the input to the output bit for bit, delayed by the reported
//     latency; switching crossfades over 10 ms (lkit::TrueBypass). While fully off the core is not run
//     and is reset once, so it restarts clean.
//   * Static, deterministic Auto Gain: blockGainDb() (cheap, e.g. a drive step) runs on the audio thread every
//     block; computeAutoGainDb() (expensive, e.g. an EQ curve's loudness) runs on the message thread whenever a
//     parameter changes; the audio thread smooths to it (50 ms). No level follower.
//   * Output trim in dB after Auto Gain (20 ms smoothing).
//   * Output peak meters, mono/stereo layouts (matched in/out), chunking to the prepared block size.
//   * State: the APVTS tree as XML with a "version" property; migrateState() for old versions.
// The subclass owns the DSP core and the parameter layout.
#include <juce_audio_processors/juce_audio_processors.h>

#include "lkit/Smooth.h"
#include "lkit/TrueBypass.h"

#include <vector>

namespace lkit {

class ProcessorBase : public juce::AudioProcessor,
                      private juce::AudioProcessorValueTreeState::Listener,
                      private juce::AsyncUpdater {
public:
    struct Config {
        juce::String name;                 // getName()
        juce::Identifier stateType;        // root tag of the saved state
        int stateVersion = 1;
        juce::String onId;                 // bool parameter: module on/off (true bypass); required
        juce::String autoGainId;           // bool parameter: Auto Gain on/off; empty = no Auto Gain
        juce::String outputId;             // float parameter in dB: output trim; empty = none
        double tailSeconds = 0.05;
    };

    ProcessorBase(Config cfg, juce::AudioProcessorValueTreeState::ParameterLayout layout);
    ~ProcessorBase() override;

    // ---- subclass interface -------------------------------------------------------------------
    // May allocate. Returns the core's latency in samples (reported to the host, constant).
    virtual int prepareCore(double sampleRate, int maxBlock, int numChannels) = 0;
    virtual void resetCore() noexcept {}
    // Audio thread, once per host block before processing: read parameters into the core.
    virtual void updateCore() noexcept {}
    // Audio thread: process m <= maxBlock samples in place (the wet path).
    virtual void processCore(float* const* io, int numChannels, int m) noexcept = 0;
    // Message thread: the static Auto Gain for the current parameters, in dB (the expensive part, e.g. an EQ
    // curve's loudness; recomputed asynchronously after a parameter change and at prepare).
    virtual double computeAutoGainDb() const { return 0.0; }
    // Audio thread, once per host block: the cheap part of the static gain, in dB, so it follows automation
    // sample-accurately to the block (also in offline renders, where the message thread may lag). autoGainOn tells
    // which: e.g. a drive step's compensation when on, its uncompensated level when off. Must not allocate or lock.
    virtual double blockGainDb(bool /*autoGainOn*/) const noexcept { return 0.0; }
    // Bring an older saved state up to date (called before it replaces the current state).
    virtual void migrateState(juce::ValueTree& /*state*/, int /*fromVersion*/) {}

    // ---- provided -----------------------------------------------------------------------------
    void prepareToPlay(double sampleRate, int samplesPerBlock) override;
    void releaseResources() override {}
    bool isBusesLayoutSupported(const BusesLayout& layouts) const override;
    void processBlock(juce::AudioBuffer<float>&, juce::MidiBuffer&) override;
    using AudioProcessor::processBlock;
    void reset() override;

    const juce::String getName() const override { return cfg_.name; }
    bool acceptsMidi() const override { return false; }
    bool producesMidi() const override { return false; }
    bool isMidiEffect() const override { return false; }
    double getTailLengthSeconds() const override { return cfg_.tailSeconds; }
    int getNumPrograms() override { return 1; }
    int getCurrentProgram() override { return 0; }
    void setCurrentProgram(int) override {}
    const juce::String getProgramName(int) override { return {}; }
    void changeProgramName(int, const juce::String&) override {}
    void getStateInformation(juce::MemoryBlock&) override;
    void setStateInformation(const void*, int) override;

    juce::AudioProcessorValueTreeState& state() noexcept { return apvts_; }
    const juce::AudioProcessorValueTreeState& state() const noexcept { return apvts_; }
    float raw(const juce::String& id) const noexcept;   // plain value (for message thread code)
    void setPlain(const juce::String& id, float plain);  // notifies the host (tests, tools)

    float takePeak(int ch) noexcept { return meter_.take(ch); }
    // The Auto Gain applied now (0 dB when Auto Gain or the module is off).
    float autoGainDb() const noexcept;
    // The static gain applied now: blockGainDb plus, when Auto Gain is on, the computed Auto Gain (0 dB when the
    // module is off).
    float staticGainDb() const noexcept;
    bool isOn() const noexcept { return pOn_->load() > 0.5f; }

protected:
    std::atomic<float>* param(const char* id) const { return apvts_.getRawParameterValue(id); }
    // Recompute Auto Gain now (message thread); e.g. after the subclass changes derived state.
    void refreshAutoGain();

private:
    void parameterChanged(const juce::String&, float) override { triggerAsyncUpdate(); }
    void handleAsyncUpdate() override { refreshAutoGain(); }

    Config cfg_;
    juce::AudioProcessorValueTreeState apvts_;
    std::atomic<float>* pOn_ = nullptr;
    std::atomic<float>* pAutoGain_ = nullptr;
    std::atomic<float>* pOutput_ = nullptr;
    std::atomic<float> autoGainTargetDb_{0.0f};
    double gainTarget() const noexcept;
    TrueBypass bypass_;
    Smoothed gain_, trim_;
    PeakMeter meter_;
    bool coreIdle_ = false;
    int maxChunk_ = 0;
    juce::StringArray listened_;
};

} // namespace lkit
