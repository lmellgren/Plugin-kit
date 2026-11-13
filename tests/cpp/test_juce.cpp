// Plugin-kit JUCE tests: the wrapper base on a toy core (gain x 0.5, latency 7).
// True bypass bit-exact (mono/stereo, block sizes, rates), switching to bypass, Auto Gain, Output trim,
// state round-trip and version migration, and the Seam components build and paint.
#include "lkit/juce/ProcessorBase.h"
#include "lkit/juce/Seam.h"
#include "lkit/juce/Snapshot.h"

#include <cmath>
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
    double blockGainDb(bool on) const noexcept override { return on ? 0.0 : gainOff; }
    void migrateState(juce::ValueTree& st, int from) override { migratedFrom = from; (void)st; }
    juce::AudioProcessorEditor* createEditor() override { return nullptr; }
    bool hasEditor() const override { return false; }
    int migratedFrom = 0;
    double gainOff = 0.0;

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
    CHECK(p.staticGainDb() == 0.0f);
    // Auto Gain off with a gain-off value (an uncompensated drive): -12 dB, +6 dB trim -> out = 0.5 x 0.5 x (in)
    Toy q;
    q.gainOff = -12.0;
    q.setPlain("output", 6.0f);
    q.setPlain("autoGain", 0.0f);
    q.prepareToPlay(48000.0, 512);
    CHECK(std::fabs(q.staticGainDb() + 12.0f) < 1e-4f);
    run(q, 2, 512, 4096, in, out);
    err = 0.0;
    const double g = std::pow(10.0, -6.0 / 20.0);
    for (int i = 100; i < 4096; ++i) err = std::max(err, std::fabs(out[0][static_cast<size_t>(i)] - 0.5 * g * in[0][static_cast<size_t>(i - 7)]));
    CHECK(err < 1e-5);
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
    {   // Vari-mu P7: the 84 pt encoder with a labelled stepped scale, the GR meter, a section heading that changes
        lkit::seamui::Encoder::Options o;
        o.knobH = 84.0f;
        o.stepLabels = {"a", "b", "c"};
        lkit::seamui::Encoder e84(*st.getParameter("step"), "Step", "", "", 126.0f, tip, false, o);
        CHECK(std::fabs(e84.cellHeight() - 114.0f) < 1e-6f && std::fabs(enc.cellHeight() - 120.0f) < 1e-6f);
        CHECK(e84.text() == lkit::seamui::minusSign(st.getParameter("step")->getCurrentValueAsText()));
        CHECK(e84.keyPressed(juce::KeyPress(juce::KeyPress::upKey)) && !e84.keyPressed(juce::KeyPress(juce::KeyPress::spaceKey)));
        float gr = 3.04f;
        lkit::seamui::GrMeter m([&gr] { return gr; });
        m.poll();
        CHECK(std::fabs(m.grDb() - 3.04f) < 1e-6f && m.text() == juce::String::charToString(0x2212) + "3.0 dB");
        gr = 0.0f;
        m.poll();
        CHECK(m.text() == "0.0 dB");
        gr = 35.0f;   // beyond the scale: the needle stops at 20
        m.poll();
        CHECK(std::fabs(lkit::seamui::GrMeter::needleAngle(0.0f) - 38.0f) < 1e-6f && std::fabs(lkit::seamui::GrMeter::needleAngle(20.0f) + 38.0f) < 1e-6f);
        CHECK(std::fabs(lkit::seamui::GrMeter::needleAngle(35.0f) + 38.0f) < 1e-6f && std::fabs(lkit::seamui::GrMeter::needleAngle(10.0f)) < 1e-6f);
        lkit::seamui::Section sec("Channel A");
        sec.setHeading("Channel A, Mid");
        CHECK(sec.heading() == "Channel A, Mid");
        for (juce::Component* c : std::initializer_list<juce::Component*>{&e84, &m, &sec}) host.addAndMakeVisible(c);
        e84.setBounds(0, 150, 126, 114);
        m.setBounds(140, 150, 176, 92);
        sec.setBounds(0, 0, 400, 141);
        CHECK(host.createComponentSnapshot(host.getLocalBounds()).isValid());
    }
}

static void testEncoderCells()
{
    using namespace lkit::seamui;
    Toy p;
    ValueTip tip;
    juce::Component host;
    host.setSize(500, 400);
    host.addChildComponent(tip);
    auto& st = p.state();
    auto& step = *st.getParameter("step");    // 3 positions, default b
    auto& drive = *st.getParameter("drive");  // continuous 0..12
    ScaleKnob::Style ss;
    ss.kind = ScaleKnob::Kind::stepped;
    ss.labels = {"a", "b", "c"};
    ss.name = "Step";
    ScaleKnob stepped(step, {"", "Step", "Step"}, ss, tip);
    ScaleKnob::Style bs;
    bs.name = "Drive";
    bs.unit = "dB";
    ScaleKnob bi(drive, {"high", "", "High"}, bs, tip);
    DualKnob dual(step, drive, {"mid", "", "Mid"}, {"Off", "x", "y"}, "kHz", "Mid frequency", "Mid gain", tip);
    for (juce::Component* c : std::initializer_list<juce::Component*>{&stepped, &bi, &dual}) {
        host.addAndMakeVisible(c);
        c->setBounds(0, 0, 126, 156);
    }
    CHECK(host.createComponentSnapshot(host.getLocalBounds()).isValid());
    CHECK(!eqGlyph("mid").isEmpty() && eqGlyph("nope").isEmpty());
    // stepped: clicking a printed label jumps to it; one step per 16 pt of drag; arrow keys one step
    CHECK(stepped.control().index() == 1);
    const juce::Point<float> labelC = [&] {
        for (int y = 0; y < 156; ++y)
            for (int x = 0; x < 126; ++x)
                if (stepped.labelAt({float(x), float(y)}) == 2) return juce::Point<float>(float(x), float(y));
        return juce::Point<float>(-1, -1);
    }();
    CHECK(labelC.x >= 0.0f);
    stepped.control().begin();
    stepped.control().setIndex(stepped.labelAt(labelC));
    stepped.control().end();
    CHECK(juce::roundToInt(p.raw("step")) == 2);
    stepped.control().begin();
    stepped.control().drag(-33.0f, false);   // two steps down
    stepped.control().end();
    CHECK(juce::roundToInt(p.raw("step")) == 0);
    stepped.keyPressed(juce::KeyPress(juce::KeyPress::upKey));
    CHECK(juce::roundToInt(p.raw("step")) == 1);
    // continuous: arrow 0.5, Option 0.1; double-click default
    bi.keyPressed(juce::KeyPress(juce::KeyPress::upKey));
    CHECK(std::fabs(p.raw("drive") - 0.5f) < 1e-4f);
    bi.keyPressed(juce::KeyPress(juce::KeyPress::upKey, juce::ModifierKeys::altModifier, 0));
    CHECK(std::fabs(p.raw("drive") - 0.6f) < 1e-4f);
    bi.control().reset();
    CHECK(p.raw("drive") == 0.0f);
    // dual: hit zones (inner knob = gain, ring and scale = frequency, outside = none)
    CHECK(dual.partAt({63.0f, 70.0f}) == DualKnob::Part::knob);
    CHECK(dual.partAt({63.0f, 70.0f - 25.0f}) == DualKnob::Part::ring);
    CHECK(dual.partAt({2.0f, 150.0f}) == DualKnob::Part::none);
    dual.keyPart(DualKnob::Part::knob, 1, false);
    CHECK(std::fabs(p.raw("drive") - 0.5f) < 1e-4f);
    dual.keyPart(DualKnob::Part::ring, 1, false);
    CHECK(juce::roundToInt(p.raw("step")) == 2);
    CHECK(stepped.control().text() == "c");
}

// Snapshot helpers (lkit/juce/Snapshot.h): set a plain value, feed a tone, write the editor to a PNG.
class ToyWithEditor final : public lkit::ProcessorBase {
public:
    ToyWithEditor() : ProcessorBase({"ToyE", "TOYE", 1, "on", "", "output", 0.0}, layout()) {}
    int prepareCore(double, int, int) override { return 0; }
    void processCore(float* const*, int, int) noexcept override {}
    juce::AudioProcessorEditor* createEditor() override { return new juce::GenericAudioProcessorEditor(*this); }
    bool hasEditor() const override { return true; }
};

static void testSnapshot()
{
    ToyWithEditor p;
    CHECK(lkit::setPlain(p, "drive", 6.0f));
    CHECK(std::abs(p.raw("drive") - 6.0f) < 1e-4f);
    CHECK(!lkit::setPlain(p, "nope", 1.0f));
    lkit::feedTone(p, 48000.0, 256, 0.2f, 0.5f, 4);
    const auto f = juce::File::getSpecialLocation(juce::File::tempDirectory).getChildFile("lkit_snapshot_test.png");
    CHECK(lkit::writeEditorSnapshot(p, f.getFullPathName(), 1.0f, 20));
    CHECK(f.existsAsFile() && f.getSize() > 100);
    auto img = juce::ImageFileFormat::loadFrom(f);
    CHECK(img.isValid() && img.getWidth() > 10);
    f.deleteFile();
}

int main()
{
    juce::ScopedJuceInitialiser_GUI gui;
    testBypassBitExact();
    testSwitchToBypass();
    testGains();
    testState();
    testSeamComponents();
    testEncoderCells();
    testSnapshot();
    if (failures == 0) std::puts("all kit JUCE tests passed");
    return failures == 0 ? 0 : 1;
}
