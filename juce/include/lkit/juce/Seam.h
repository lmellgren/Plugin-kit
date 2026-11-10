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
    void show(juce::Component& anchor, const juce::String& text);
    void paint(juce::Graphics&) override;

private:
    juce::String text_;
};

// 44 pt encoder in a cellW x 90 cell, label above, value below (seamgen enc_c + single_knob).
// Unipolar draws the arc from the minimum, bipolar from the centre. Range and value text come from
// the parameter; choice/stepped parameters snap (one step per 16 pt of drag).
class Encoder final : public juce::Component {
public:
    Encoder(juce::RangedAudioParameter& p, juce::String label, juce::String lo, juce::String hi, float cellW, ValueTip& tip,
            bool bipolar = false);
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
    juce::String label_, lo_, hi_;
    float cellW_;
    bool bipolar_;
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

private:
    void timerCallback() override;
    PeakSource src_;
    float level_[2] = {-120, -120}, hold_[2] = {-120, -120};
    int holdTicks_[2] = {0, 0};
    bool clip_ = false, dark_ = false;
};

// Header (ChainSlot): the on/off square, then the name in heading style.
class Header final : public juce::Component {
public:
    Header(juce::RangedAudioParameter& on, juce::String title);
    void paint(juce::Graphics&) override;
    void mouseDown(const juce::MouseEvent&) override;
    bool isOn() const { return on_; }
    std::function<void()> onChange;

private:
    juce::ParameterAttachment att_;
    juce::String title_;
    bool on_ = true;
};

} // namespace lkit::seamui
