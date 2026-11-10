// Plugin-kit JUCE tests: the wrapper base on a toy core (gain x 0.5, latency 7).
// True bypass bit-exact (mono/stereo, block sizes, rates), switching to bypass, Auto Gain, Output trim,
// state round-trip and version migration, and the Seam components build and paint.
#include "lkit/juce/ProcessorBase.h"
#include "lkit/juce/Seam.h"

#include <cstdio>
#include <cstring>
#include <random>

static int failures = 0;
#define CHECK(cond)                                                                       \
    do {                                                                                  \
        if (!(cond)) {                                                                    \
            std::fprintf(stderr, "%s:%d: CHECK failed: %s\n", __FILE__, __LINE__, #cond); \
            ++failures;                                                                   \
        }                                                                                 \
    } while (0)

namespace {
juce::AudioProcessorValueTreeState::ParameterLayout layout()
{
    juce::AudioProcessorValueTreeState::ParameterLayout l;
    l.add(std::make_unique<juce::AudioParameterBool>(juce::ParameterID{"on", 1}, "On", true));
    l.add(std::make_unique<juce::AudioParameterBool>(juce::ParameterID{"autoGain", 1}, "Auto Gain", true));
    l.add(std::make_unique<juce::AudioParameterFloat>(juce::ParameterID{"drive", 1}, "Drive", juce::NormalisableRange<float>(0.0f, 12.0f, 0.1f), 0.0f));
    l.add(std::make_unique<juce::AudioParameterFloat>(juce::ParameterID{"output", 1}, "Output", juce::NormalisableRange<float>(-24.0f, 24.0f, 0.1f), 0.0f));
    l.add(std::make_unique<juce::AudioParameterChoice>(juce::ParameterID{"step", 1}, "Step", juce::StringArray{"a", "b", "c"}, 1));
    return l;
}

class Toy final : public lkit::ProcessorBase {
public:
    Toy() : ProcessorBase({"Toy", "TOY", 2, "on", "autoGain", "output", 0.0}, layout()) {}
    int prepareCore(double, int, int) override
    {
        for (auto& d : line_) d.fill(0.0f);
        return 7;
    }
    void resetCore() noexcept override
    {
        for (auto& d : line_) d.fill(0.0f);
        pos_ = 0;
    }
    void processCore(float* const* io, int nch, int m) noexcept override
    {
        for (int i = 0; i < m; ++i) {
            for (int ch = 0; ch < nch; ++ch) {
                const float x = io[ch][i];
                io[ch][i] = 0.5f * line_[static_cast<size_t>(ch)][static_cast<size_t>(pos_)];
                line_[static_cast<size_t>(ch)][static_cast<size_t>(pos_)] = x;
            }
            pos_ = (pos_ + 1) % 7;
        }
    }
    double computeAutoGainDb() const override { return -raw("drive"); }   // compensate the drive
    void migrateState(juce::ValueTree& st, int from) override { migratedFrom = from; (void)st; }
    juce::AudioProcessorEditor* createEditor() override { return nullptr; }
    bool hasEditor() const override { return false; }
    int migratedFrom = 0;

private:
    std::array<std::array<float, 7>, 2> line_{};
    int pos_ = 0;
};

bool sameBits(float a, float b) { return std::memcmp(&a, &b, sizeof(float)) == 0; }

void run(Toy& p, int nch, int block, int total, std::vector<std::vector<float>>& in, std::vector<std::vector<float>>& out,
         int switchOffAt = -1)
{
    std::mt19937 rng(42);
    std::normal_distribution<float> n(0.0f, 0.25f);
    in.assign(static_cast<size_t>(nch), std::vector<float>(static_cast<size_t>(total)));
    out = in;
    for (auto& c : in)
        for (auto& v : c) v = n(rng);
    juce::MidiBuffer midi;
    for (int pos = 0; pos < total; pos += block) {
        if (switchOffAt >= 0 && pos >= switchOffAt) p.setPlain("on", 0.0f);
        const int m = std::min(block, total - pos);
        juce::AudioBuffer<float> buf(nch, m);
        for (int c = 0; c < nch; ++c) buf.copyFrom(c, 0, in[static_cast<size_t>(c)].data() + pos, m);
        p.processBlock(buf, midi);
        for (int c = 0; c < nch; ++c) std::copy_n(buf.getReadPointer(c), m, out[static_cast<size_t>(c)].data() + pos);
    }
}

void setMono(Toy& p)
{
    juce::AudioProcessor::BusesLayout l;
    l.inputBuses.add(juce::AudioChannelSet::mono());
    l.outputBuses.add(juce::AudioChannelSet::mono());
    CHECK(p.setBusesLayout(l));
}
} // namespace

static void testBypassBitExact()
{
    for (double fs : {44100.0, 48000.0, 96000.0})
        for (int block : {16, 333, 512})
            for (int nch : {1, 2}) {
                Toy p;
                if (nch == 1) setMono(p);
                p.setPlain("on", 0.0f);
                p.setPlain("drive", 6.0f);
                p.setPlain("output", 6.0f);
                p.prepareToPlay(fs, block);
                CHECK(p.getLatencySamples() == 7);
                std::vector<std::vector<float>> in, out;
                const int total = 4000;
                run(p, nch, block, total, in, out);
                bool exact = true;
                for (int c = 0; c < nch; ++c)
                    for (int i = 0; i < total; ++i)
                        exact = exact && sameBits(out[static_cast<size_t>(c)][static_cast<size_t>(i)],
                                                  i < 7 ? 0.0f : in[static_cast<size_t>(c)][static_cast<size_t>(i - 7)]);
                CHECK(exact);
                CHECK(p.autoGainDb() == 0.0f);
            }
    // host blocks larger than prepared
    Toy p;
    p.setPlain("on", 0.0f);
    p.prepareToPlay(48000.0, 64);
    std::vector<std::vector<float>> in, out;
    run(p, 2, 1000, 5000, in, out);
    bool exact = true;
    for (int i = 7; i < 5000; ++i) exact = exact && sameBits(out[1][static_cast<size_t>(i)], in[1][static_cast<size_t>(i - 7)]);
    CHECK(exact);
}

static void testSwitchToBypass()
{
    Toy p;
    p.prepareToPlay(48000.0, 256);
    std::vector<std::vector<float>> in, out;
    const int total = 48000, off = 24064;
    run(p, 2, 256, total, in, out, off);
    const int settled = off + 480 + 1;
    bool exact = true, processedBefore = true;
    for (int c = 0; c < 2; ++c)
        for (int i = 7; i < total; ++i) {
            const float x = in[static_cast<size_t>(c)][static_cast<size_t>(i - 7)];
            const float y = out[static_cast<size_t>(c)][static_cast<size_t>(i)];
            if (i >= settled) exact = exact && sameBits(y, x);
            else if (i < off) processedBefore = processedBefore && std::fabs(y - 0.5f * x) < 1e-6f;
        }
    CHECK(exact);
    CHECK(processedBefore);
}

static void testGains()
{
    // Auto Gain -6 dB (drive 6) and Output +6 dB cancel: out = 0.5 x (the core) after smoothing
    Toy p;
    p.setPlain("drive", 6.0f);
    p.setPlain("output", 6.0f);
    p.prepareToPlay(48000.0, 512);
    CHECK(std::fabs(p.autoGainDb() + 6.0f) < 1e-4f);
    std::vector<std::vector<float>> in, out;
    run(p, 2, 512, 4096, in, out);
    double err = 0.0;
    for (int i = 100; i < 4096; ++i) err = std::max(err, std::fabs(out[0][static_cast<size_t>(i)] - 0.5 * in[0][static_cast<size_t>(i - 7)]));
    CHECK(err < 1e-5);
    // Auto Gain off: +6 dB trim only
    p.setPlain("autoGain", 0.0f);
    CHECK(p.autoGainDb() == 0.0f);
}

static void testState()
{
    Toy a;
    a.setPlain("drive", 3.3f);
    a.setPlain("step", 2.0f);
    a.setPlain("on", 0.0f);
    juce::MemoryBlock mb;
    a.getStateInformation(mb);
    Toy b;
    b.setStateInformation(mb.getData(), static_cast<int>(mb.getSize()));
    CHECK(std::fabs(b.raw("drive") - 3.3f) < 1e-4f);
    CHECK(b.raw("step") == 2.0f && b.raw("on") == 0.0f);
    CHECK(b.migratedFrom == 0);   // same version: no migration
    // an older version is migrated
    auto st = a.state().copyState();
    st.setProperty("version", 1, nullptr);
    juce::MemoryBlock old;
    juce::AudioProcessor::copyXmlToBinary(*st.createXml(), old);
    Toy c;
    c.setStateInformation(old.getData(), static_cast<int>(old.getSize()));
    CHECK(c.migratedFrom == 1);
    // garbage is ignored
    Toy d;
    const char junk[] = "not a state";
    d.setStateInformation(junk, sizeof(junk));
    CHECK(d.raw("drive") == 0.0f);
}

static void testSeamComponents()
{
    Toy p;
    lkit::seamui::ValueTip tip;
    juce::Component host;
    host.setSize(400, 300);
    host.addChildComponent(tip);
    auto& st = p.state();
    lkit::seamui::Encoder enc(*st.getParameter("drive"), "Drive", "0", "12", 126.0f, tip);
    lkit::seamui::Encoder stepped(*st.getParameter("step"), "Step", "a", "c", 126.0f, tip);
    lkit::seamui::Segmented seg(*st.getParameter("step"), {"a", "b", "c"}, "", "Step");
    lkit::seamui::Toggle tog("Auto Gain", st.getParameter("autoGain"));
    lkit::seamui::Trim trim(*st.getParameter("output"), "Output");
    lkit::seamui::Meter meter([&p](int ch) { return p.takePeak(ch); });
    lkit::seamui::Header head(*st.getParameter("on"), "Toy");
    for (juce::Component* c : std::initializer_list<juce::Component*>{&enc, &stepped, &seg, &tog, &trim, &meter, &head}) {
        host.addAndMakeVisible(c);
        c->setBounds(0, 0, 140, 130);
    }
    auto img = host.createComponentSnapshot(host.getLocalBounds());
    CHECK(img.isValid());
    CHECK(seg.preferredWidth() > 70.0f);
}

int main()
{
    juce::ScopedJuceInitialiser_GUI gui;
    testBypassBitExact();
    testSwitchToBypass();
    testGains();
    testState();
    testSeamComponents();
    if (failures == 0) std::puts("all kit JUCE tests passed");
    return failures == 0 ? 0 : 1;
}
