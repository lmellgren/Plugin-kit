#include "lkit/juce/ProcessorBase.h"

#include <algorithm>
#include <cmath>

namespace lkit {

ProcessorBase::ProcessorBase(Config cfg, juce::AudioProcessorValueTreeState::ParameterLayout layout)
    : AudioProcessor(BusesProperties()
                         .withInput("Input", juce::AudioChannelSet::stereo(), true)
                         .withOutput("Output", juce::AudioChannelSet::stereo(), true)),
      cfg_(std::move(cfg)), apvts_(*this, nullptr, cfg_.stateType, std::move(layout))
{
    pOn_ = apvts_.getRawParameterValue(cfg_.onId);
    jassert(pOn_ != nullptr);
    if (cfg_.autoGainId.isNotEmpty()) pAutoGain_ = apvts_.getRawParameterValue(cfg_.autoGainId);
    if (cfg_.outputId.isNotEmpty()) pOutput_ = apvts_.getRawParameterValue(cfg_.outputId);
    for (auto* p : getParameters())
        if (auto* r = dynamic_cast<juce::RangedAudioParameter*>(p)) {
            listened_.add(r->getParameterID());
            apvts_.addParameterListener(r->getParameterID(), this);
        }
    apvts_.state.setProperty("version", cfg_.stateVersion, nullptr);
}

ProcessorBase::~ProcessorBase()
{
    cancelPendingUpdate();
    for (const auto& id : listened_) apvts_.removeParameterListener(id, this);
}

float ProcessorBase::raw(const juce::String& id) const noexcept
{
    auto* p = apvts_.getRawParameterValue(id);
    return p != nullptr ? p->load() : 0.0f;
}

void ProcessorBase::setPlain(const juce::String& id, float plain)
{
    if (auto* p = apvts_.getParameter(id)) p->setValueNotifyingHost(p->convertTo0to1(plain));
}

float ProcessorBase::autoGainDb() const noexcept
{
    if (pAutoGain_ == nullptr || pAutoGain_->load() < 0.5f || !isOn()) return 0.0f;
    return autoGainTargetDb_.load();
}

void ProcessorBase::refreshAutoGain()
{
    autoGainTargetDb_.store(static_cast<float>(computeAutoGainDb()));
}

void ProcessorBase::prepareToPlay(double sampleRate, int samplesPerBlock)
{
    const int nch = std::clamp(getTotalNumOutputChannels(), 1, 2);
    maxChunk_ = std::max(1, samplesPerBlock);
    updateCore();
    const int latency = prepareCore(sampleRate, maxChunk_, nch);
    setLatencySamples(latency);
    bypass_.prepare(sampleRate, maxChunk_, latency, isOn());
    refreshAutoGain();
    const bool ag = pAutoGain_ != nullptr && pAutoGain_->load() > 0.5f;
    gain_.prepare(sampleRate, 0.050, ag ? dbToGain(autoGainTargetDb_.load()) : 1.0);
    trim_.prepare(sampleRate, 0.020, pOutput_ != nullptr ? dbToGain(pOutput_->load()) : 1.0);
    coreIdle_ = false;
}

void ProcessorBase::reset()
{
    resetCore();
    bypass_.reset();
}

bool ProcessorBase::isBusesLayoutSupported(const BusesLayout& layouts) const
{
    const auto& out = layouts.getMainOutputChannelSet();
    if (out != juce::AudioChannelSet::mono() && out != juce::AudioChannelSet::stereo()) return false;
    return out == layouts.getMainInputChannelSet();
}

void ProcessorBase::processBlock(juce::AudioBuffer<float>& buffer, juce::MidiBuffer&)
{
    juce::ScopedNoDenormals noDenormals;
    const int n = buffer.getNumSamples();
    for (int ch = getTotalNumInputChannels(); ch < getTotalNumOutputChannels(); ++ch) buffer.clear(ch, 0, n);
    const int nch = std::min(buffer.getNumChannels(), 2);
    if (n == 0 || nch == 0 || maxChunk_ == 0) return;

    const bool on = isOn();
    const bool ag = pAutoGain_ != nullptr && pAutoGain_->load() > 0.5f;
    gain_.setTarget(ag ? dbToGain(autoGainTargetDb_.load()) : 1.0);
    trim_.setTarget(pOutput_ != nullptr ? dbToGain(pOutput_->load()) : 1.0);
    updateCore();

    float* io[2] = {buffer.getWritePointer(0), buffer.getWritePointer(nch > 1 ? 1 : 0)};
    float peak[2] = {0.0f, 0.0f};
    for (int start = 0; start < n; start += maxChunk_) {
        const int m = std::min(maxChunk_, n - start);
        float* chunk[2] = {io[0] + start, io[1] + start};
        bypass_.capture(chunk, nch, m);
        if (!on && bypass_.fullyOff()) {
            if (!coreIdle_) {   // fully bypassed: stop the core, restart it clean when switched on
                resetCore();
                coreIdle_ = true;
            }
        } else {
            coreIdle_ = false;
            processCore(chunk, nch, m);
            if (gain_.settled() && trim_.settled()) {
                const float g = static_cast<float>(gain_.value() * trim_.value());
                if (g != 1.0f)
                    for (int ch = 0; ch < nch; ++ch) juce::FloatVectorOperations::multiply(chunk[ch], g, m);
            } else {
                for (int i = 0; i < m; ++i) {
                    const double g = gain_.next() * trim_.next();
                    for (int ch = 0; ch < nch; ++ch) chunk[ch][i] = static_cast<float>(g * chunk[ch][i]);
                }
                gain_.settle();
                trim_.settle();
            }
        }
        bypass_.render(chunk, nch, m, on);
        for (int ch = 0; ch < nch; ++ch) {
            const auto r = juce::FloatVectorOperations::findMinAndMax(chunk[ch], m);
            peak[ch] = std::max(peak[ch], std::max(-r.getStart(), r.getEnd()));
        }
    }
    meter_.push(0, peak[0]);
    meter_.push(1, nch > 1 ? peak[1] : peak[0]);
}

void ProcessorBase::getStateInformation(juce::MemoryBlock& dest)
{
    auto st = apvts_.copyState();
    st.setProperty("version", cfg_.stateVersion, nullptr);
    if (auto xml = st.createXml()) copyXmlToBinary(*xml, dest);
}

void ProcessorBase::setStateInformation(const void* data, int size)
{
    auto xml = getXmlFromBinary(data, size);
    if (xml == nullptr || !xml->hasTagName(apvts_.state.getType())) return;
    auto tree = juce::ValueTree::fromXml(*xml);
    const int from = tree.getProperty("version", 1);
    if (from != cfg_.stateVersion) migrateState(tree, from);
    tree.setProperty("version", cfg_.stateVersion, nullptr);
    apvts_.replaceState(tree);
    triggerAsyncUpdate();
}

} // namespace lkit
