#pragma once
// Seam design system components for LOMAS plugin windows, extracted from the LOMAS Passive EQ editor
// and made generic (ranges come from the parameter; the meter reads a peak source).
// Colours, sizes and type come from Tokens.h, generated at build time from the read-only Seam design
// system (~/Apps/Seam/design/tokens.json) by juce/gen_tokens.py. No design file lives in any repo but Seam.
#include <juce_audio_processors/juce_audio_processors.h>
#include <juce_gui_basics/juce_gui_basics.h>

#include "Tokens.h"

#include <functional>
#include <memory>
#include <vector>

namespace lkit::seamui {

juce::Colour col(std::uint32_t argb);
juce::Font font(float sizePx, int weight);
juce::Font font(const seam::TextStyle& st);
float textWidth(const juce::Font& f, const juce::String& t);
juce::String minusSign(juce::String s);   // typographic minus

// Value tooltip above a control (Knob.md): bg-500, line-strong border, micro.
class ValueTip final : public juce::Component {
public:
    // Centred above the anchor, its top at y (anchor coordinates; 22 for the 44 pt Encoder, -14 for the 126 pt
    // encoder cells, as seamgen knob_tip). An empty text hides it.
    void show(juce::Component& anchor, const juce::String& text, float y = 22.0f);
    void paint(juce::Graphics&) override;

private:
    juce::String text_;
};

// 44 pt encoder in a cellW x 90 cell, label above, value below (seamgen enc_c + single_knob).
// Unipolar draws the arc from the minimum, bipolar from the centre. Range and value text come from
// the parameter; choice/stepped parameters snap (one step per 16 pt of drag).
//   knobH: the knob drawing's height (seamgen single_knob h; 90 in Passive EQ, 84 in the Vari-mu rows).
//   stepLabels: one label per position of a stepped parameter, printed around the scale with the selected one in
//   ink / 600 (seamgen single_knob(labels, idx)); lo / hi are then not drawn.
class Encoder final : public juce::Component {
public:
    struct Options {
        float knobH = 90.0f;
        juce::StringArray stepLabels;
    };
    Encoder(juce::RangedAudioParameter& p, juce::String label, juce::String lo, juce::String hi, float cellW, ValueTip& tip,
            bool bipolar = false);
    Encoder(juce::RangedAudioParameter& p, juce::String label, juce::String lo, juce::String hi, float cellW, ValueTip& tip,
            bool bipolar, Options opt);
    float cellHeight() const { return 14.0f + 2.0f + opt_.knobH + 2.0f + 12.0f; }
    juce::String text() const { return valueText(); }   // the value line (tests: QA G4)
    bool keyPressed(const juce::KeyPress&) override;     // arrows: one step (stepped) or 1 % (Option: 0.1 %)
    // Assistive technology: a slider whose value is the value line, plus a note after a comma when one is set
    // ("3.0, follows A" for a control that shows another one's value). Read-only while the component is disabled.
    std::unique_ptr<juce::AccessibilityHandler> createAccessibilityHandler() override;
    void setAccessibleNote(juce::String note) { note_ = std::move(note); }
    juce::String accessibleText() const { return note_.isEmpty() ? valueText() : valueText() + ", " + note_; }
    float norm() const { return norm_; }
    void setNormalised(float v) { setNorm(v, false); }   // a complete gesture (assistive technology)
    void paint(juce::Graphics&) override;
    void mouseDown(const juce::MouseEvent&) override;
    void mouseDrag(const juce::MouseEvent&) override;
    void mouseUp(const juce::MouseEvent&) override;
    void mouseDoubleClick(const juce::MouseEvent&) override;
    void mouseWheelMove(const juce::MouseEvent&, const juce::MouseWheelDetails&) override;
    void mouseEnter(const juce::MouseEvent&) override;
    void mouseExit(const juce::MouseEvent&) override;
    static constexpr float kHeight = 14 + 2 + 90 + 2 + 12;

private:
    void setNorm(float v, bool gesture);
    int steps() const;   // > 0 for stepped parameters
    juce::String valueText() const;
    juce::RangedAudioParameter& param_;
    juce::ParameterAttachment att_;
    juce::String label_, lo_, hi_, note_;
    float cellW_;
    bool bipolar_;
    Options opt_;
    float norm_ = 0.0f, dragStart_ = 0.0f;
    bool dragging_ = false;
    ValueTip& tip_;
};

// Segmented control at chain size (seamgen seg_s), with an optional unit after it.
class Segmented final : public juce::Component {
public:
    Segmented(juce::RangedAudioParameter& p, juce::StringArray labels, juce::String unit, juce::String a11y);
    void paint(juce::Graphics&) override;
    void mouseDown(const juce::MouseEvent&) override;
    float preferredWidth() const;

private:
    std::vector<juce::Rectangle<float>> boxes() const;
    juce::ParameterAttachment att_;
    juce::StringArray labels_;
    juce::String unit_;
    int index_ = 0;
};

// Footer toggle (seamgen toggle_btn): activator when on, bg-600 when off, 40 % when disabled.
class Toggle final : public juce::Component {
public:
    Toggle(juce::String label, juce::RangedAudioParameter* p);   // p == nullptr: disabled
    Toggle(juce::String label, std::function<bool()> get, std::function<void(bool)> set);
    void paint(juce::Graphics&) override;
    void mouseDown(const juce::MouseEvent&) override;
    float preferredWidth() const;

private:
    juce::String label_;
    juce::RangedAudioParameter* param_;
    std::unique_ptr<juce::ParameterAttachment> att_;
    std::function<bool()> get_;
    std::function<void(bool)> set_;
    bool on_ = false;
    bool disabled_ = false;
};

// Trim: label, 22 pt bipolar knob (knob_bi), 46 x 22 value field; range from the parameter (dB).
class Trim final : public juce::Component {
public:
    explicit Trim(juce::RangedAudioParameter& p, juce::String label = "Trim");
    void paint(juce::Graphics&) override;
    void mouseDown(const juce::MouseEvent&) override;
    void mouseDrag(const juce::MouseEvent&) override;
    void mouseUp(const juce::MouseEvent&) override;
    void mouseDoubleClick(const juce::MouseEvent&) override;
    void mouseWheelMove(const juce::MouseEvent&, const juce::MouseWheelDetails&) override;
    static constexpr float kWidth = 30 + 6 + 22 + 6 + 46;

private:
    void setDb(float v, bool gesture);
    juce::RangedAudioParameter& param_;
    juce::ParameterAttachment att_;
    juce::String label_;
    float db_ = 0.0f, dragStart_ = 0.0f;
    bool dragging_ = false;
};

// Section frame (seamgen section): 1 pt line border, bg-300, 20 pt title bar in micro ink-muted.
class Section final : public juce::Component {
public:
    explicit Section(juce::String title) : title_(std::move(title)) {}
    void paint(juce::Graphics&) override;
    void setHeading(juce::String t)   // the title bar's text (e.g. a channel row that changes name with the mode)
    {
        if (t == title_) return;
        title_ = std::move(t);
        repaint();
    }
    const juce::String& heading() const { return title_; }
    juce::Rectangle<float> content() const;   // inside the padding (4 10 8 10)
    void setDivider(float x) { dividerX_ = x; repaint(); }

private:
    juce::String title_;
    float dividerX_ = -1.0f;
};

// Stereo slot meter (seamgen slot_meter): -60..+6 dBFS, 0 dB line, peak hold, clip lamp.
class Meter final : public juce::Component, private juce::Timer {
public:
    using PeakSource = std::function<float(int channel)>;   // linear peak since the last call
    explicit Meter(PeakSource src);
    void paint(juce::Graphics&) override;
    void mouseDown(const juce::MouseEvent&) override { clip_ = false; repaint(); }
    void setDark(bool d) { dark_ = d; }
    void poll() { timerCallback(); }                         // one meter tick now (tests)
    float levelDb(int ch) const { return level_[ch & 1]; }   // the bar's level (tests: QA G4)
    float holdDb(int ch) const { return hold_[ch & 1]; }

private:
    void timerCallback() override;
    PeakSource src_;
    float level_[2] = {-120, -120}, hold_[2] = {-120, -120};
    int holdTicks_[2] = {0, 0};
    bool clip_ = false, dark_ = false;
};

// Gain-reduction meter (seamgen gr_meter): a needle in accent over a 20 -> 0 dB scale, 0 at the right; "GR" at the
// bottom left and the value at the bottom right. The source gives the gain reduction in dB (>= 0) to show now; the
// needle shows it as it comes (the plugin's own release is the ballistics). Any size; 176 x 92 in the Vari-mu rows.
class GrMeter final : public juce::Component, private juce::Timer {
public:
    using Source = std::function<float()>;
    explicit GrMeter(Source src);
    void paint(juce::Graphics&) override;
    void poll() { timerCallback(); }             // one meter tick now (tests)
    float grDb() const { return gr_; }           // what the needle shows (tests: QA G4)
    juce::String text() const;                   // the value text: "0.0 dB", "\u22123.0 dB"
    static float needleAngle(float grDb);        // degrees from 12 o'clock: 0 dB -> +38, 20 dB -> -38

private:
    void timerCallback() override;
    Source src_;
    float gr_ = 0.0f;
};

// Header: the on/off square, then the name in heading style. Centred over its bounds (Passive EQ), or left after
// the square with an 8 pt gap in ink (seamgen plugin_window, titleLeft).
class Header final : public juce::Component {
public:
    Header(juce::RangedAudioParameter& on, juce::String title, bool titleLeft = false);
    void paint(juce::Graphics&) override;
    void mouseDown(const juce::MouseEvent&) override;
    bool isOn() const { return on_; }
    std::function<void()> onChange;

private:
    juce::ParameterAttachment att_;
    juce::String title_;
    bool on_ = true, titleLeft_ = false;
};

// ---- 126 pt encoder cells (Knob.md; seamgen enc + single_knob / dual_knob) --------------------------------------
// EQ section glyphs as on the hardware front panel (seamgen ICON, 24 x 12): "high", "mid", "low", "hpf".
juce::Path eqGlyph(const juce::String& name);

// One parameter's value logic, shared by the knobs: stepped (choice / few steps) or continuous; drag (stepped: one
// step per 16 pt; continuous: full range per 200 pt, Option x 0.1), wheel, arrow keys (stepped: one step;
// continuous: 0.5, Option 0.1, in plain units), default on double-click. Message thread.
class ParamControl {
public:
    ParamControl(juce::RangedAudioParameter& p, std::function<void()> changed);
    int steps() const;   // > 0: stepped, number of intervals
    int index() const;   // the step (stepped)
    float norm() const { return norm_; }
    void set(float norm, bool gesture);
    void setIndex(int i);
    void begin();
    void drag(float dyUp, bool fine);
    void end();
    bool dragging() const { return dragging_; }
    void wheel(float deltaY, bool smooth, bool fine);
    void step(int dir, bool fine);
    void reset();
    juce::String text() const;   // the parameter's text, typographic minus
    juce::RangedAudioParameter& param;

private:
    juce::ParameterAttachment att_;
    float norm_ = 0.0f, start_ = 0.0f, wheelAcc_ = 0.0f;
    bool dragging_ = false;
};

// Head of an encoder cell: an EQ glyph (glyph name) or a text label (micro, ink-muted).
struct CellHead {
    juce::String glyph, text, a11y;
};

// The 126 x 156 encoder cell: head (14), gap 6, the 104 pt knob drawing (centre 50 below its top), gap 6, the
// value line(s). Stepped: a dot per position and labels (every n-th on dense scales; dots and labels are click
// targets); bipolar: 11 dots, − 0 +, arc from 12 o'clock; unipolar: 11 dots, end labels, arc from the minimum.
class ScaleKnob final : public juce::Component {
public:
    enum class Kind { stepped, bipolar, unipolar };
    struct Style {
        Kind kind = Kind::bipolar;
        juce::StringArray labels;   // stepped: one per position
        int every = 1;              // stepped: a label on every n-th position
        juce::String lo, hi;        // unipolar end labels
        juce::String unit;          // under the knob
        juce::String name;          // tooltip and accessibility ("Input")
    };
    ScaleKnob(juce::RangedAudioParameter& p, CellHead head, Style st, ValueTip& tip);
    static constexpr float kW = 126.0f, kH = 156.0f;
    void paint(juce::Graphics&) override;
    void mouseDown(const juce::MouseEvent&) override;
    void mouseDrag(const juce::MouseEvent&) override;
    void mouseUp(const juce::MouseEvent&) override;
    void mouseDoubleClick(const juce::MouseEvent&) override;
    void mouseWheelMove(const juce::MouseEvent&, const juce::MouseWheelDetails&) override;
    void mouseEnter(const juce::MouseEvent&) override;
    void mouseExit(const juce::MouseEvent&) override;
    bool keyPressed(const juce::KeyPress&) override;
    std::unique_ptr<juce::AccessibilityHandler> createAccessibilityHandler() override;
    int labelAt(juce::Point<float> p) const;   // stepped: the position whose label or dot is at p, else -1
    ParamControl& control() { return ctl_; }

private:
    void showTip();
    ParamControl ctl_;
    CellHead head_;
    Style st_;
    ValueTip& tip_;
};

// Dual encoder (Knob.md): outer ring = stepped frequency (scale printed around), inner knob = bipolar gain. Two hit
// zones and two focusable parts (frequency first); the part under the pointer, or being dragged, is outlined. Both
// values under it, the inactive one in ink-muted. An "Off" first position dims the inner knob to 40 %.
class DualKnob final : public juce::Component {
public:
    DualKnob(juce::RangedAudioParameter& freq, juce::RangedAudioParameter& gain, CellHead head, juce::StringArray freqLabels,
             juce::String unit, juce::String freqName, juce::String gainName, ValueTip& tip);
    ~DualKnob() override;
    static constexpr float kW = 126.0f, kH = 156.0f;
    enum class Part { none, ring, knob };
    void paint(juce::Graphics&) override;
    void resized() override;
    void mouseMove(const juce::MouseEvent&) override;
    void mouseDown(const juce::MouseEvent&) override;
    void mouseDrag(const juce::MouseEvent&) override;
    void mouseUp(const juce::MouseEvent&) override;
    void mouseDoubleClick(const juce::MouseEvent&) override;
    void mouseWheelMove(const juce::MouseEvent&, const juce::MouseWheelDetails&) override;
    void mouseEnter(const juce::MouseEvent&) override;
    void mouseExit(const juce::MouseEvent&) override;
    Part partAt(juce::Point<float> p) const;
    int labelAt(juce::Point<float> p) const;   // the frequency position whose label or dot is at p, else -1
    ParamControl& freq() { return freq_; }
    ParamControl& gain() { return gain_; }
    Part active() const { return active_; }
    void keyPart(Part part, int dir, bool fine);   // arrow keys on a focused part

private:
    class Focus;
    void setActive(Part p);
    void showTip();
    bool off() const;
    ParamControl freq_, gain_;
    CellHead head_;
    juce::StringArray labels_;
    juce::String unit_, freqName_, gainName_;
    ValueTip& tip_;
    Part active_ = Part::none, dragPart_ = Part::none;
    std::unique_ptr<Focus> focusRing_, focusKnob_;
};

} // namespace lkit::seamui
