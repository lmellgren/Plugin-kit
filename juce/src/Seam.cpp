#include "lkit/juce/Seam.h"

#include <cmath>
#include <map>

namespace lkit::seamui {
namespace {
constexpr float kPi = 3.14159265358979f;
using namespace seam;

juce::Point<float> at(float cx, float cy, float deg, float rad)
{
    const float a = deg * kPi / 180.0f;
    return {cx + rad * std::sin(a), cy - rad * std::cos(a)};
}

juce::Path arc(float cx, float cy, float rad, float fromDeg, float toDeg)
{
    juce::Path p;
    p.addCentredArc(cx, cy, rad, rad, 0.0f, fromDeg * kPi / 180.0f, toDeg * kPi / 180.0f, true);
    return p;
}

juce::String typeface()
{
    static const juce::String name = [] {
        const auto all = juce::Font::findAllTypefaceNames();
        return all.contains(type::family) ? juce::String(type::family) : juce::Font::getDefaultSansSerifFontName();
    }();
    return name;
}
} // namespace

juce::String minusSign(juce::String s) { return s.replace("-", juce::String::charToString(0x2212)); }

float textWidth(const juce::Font& f, const juce::String& t)
{
    juce::GlyphArrangement g;
    g.addLineOfText(f, t, 0.0f, 0.0f);
    return g.getBoundingBox(0, -1, true).getWidth();
}

juce::Colour col(std::uint32_t argb) { return juce::Colour(argb); }

juce::Font font(float sizePx, int weight)
{
    // CSS px sizes as point heights; 600 = semibold (bold where no semibold face exists).
    auto opts = juce::FontOptions().withName(typeface()).withPointHeight(sizePx);
    if (weight >= 600) opts = opts.withStyle("Semibold");
    else if (weight >= 500) opts = opts.withStyle("Medium");
    juce::Font f(opts);
    if (weight >= 600 && f.getTypefaceStyle() != "Semibold") f = juce::Font(opts.withStyle("Bold"));
    return f;
}

juce::Font font(const seam::TextStyle& st) { return font(st.size, st.weight); }

// ---- ValueTip --------------------------------------------------------------------------------
void ValueTip::show(juce::Component& anchor, const juce::String& text, float y)
{
    text_ = text;
    if (text.isEmpty()) {
        setVisible(false);
        return;
    }
    auto* parent = getParentComponent();
    if (parent == nullptr) return;
    const auto a = parent->getLocalArea(&anchor, anchor.getLocalBounds()).toFloat();
    const float w = textWidth(font(type::micro), text) + 12.0f, h = 16.0f;
    setBounds(juce::Rectangle<float>(a.getCentreX() - w / 2, a.getY() + y, w, h).toNearestInt());
    setVisible(true);
    toFront(false);
    repaint();
}

void ValueTip::paint(juce::Graphics& g)
{
    auto r = getLocalBounds().toFloat().reduced(0.5f);
    g.setColour(col(colour::bg500));
    g.fillRoundedRectangle(r, radius::radiusSm);
    g.setColour(col(colour::lineStrong));
    g.drawRoundedRectangle(r, radius::radiusSm, 1.0f);
    g.setColour(col(colour::ink));
    g.setFont(font(type::micro));
    g.drawText(text_, getLocalBounds(), juce::Justification::centred);
}

// ---- Encoder ---------------------------------------------------------------------------------
Encoder::Encoder(juce::RangedAudioParameter& p, juce::String label, juce::String lo, juce::String hi, float cellW, ValueTip& tip,
                 bool bipolar)
    : Encoder(p, std::move(label), std::move(lo), std::move(hi), cellW, tip, bipolar, Options{})
{
}

Encoder::Encoder(juce::RangedAudioParameter& p, juce::String label, juce::String lo, juce::String hi, float cellW, ValueTip& tip,
                 bool bipolar, Options opt)
    : param_(p), att_(p, [this](float v) { norm_ = param_.convertTo0to1(v); repaint(); }), label_(std::move(label)),
      lo_(std::move(lo)), hi_(std::move(hi)), cellW_(cellW), bipolar_(bipolar), opt_(std::move(opt)), tip_(tip)
{
    att_.sendInitialUpdate();
    setTitle(label_);
    setDescription("Adjustable");
    setWantsKeyboardFocus(true);
}

bool Encoder::keyPressed(const juce::KeyPress& k)
{
    const int dir = (k.isKeyCode(juce::KeyPress::upKey) || k.isKeyCode(juce::KeyPress::rightKey)) ? 1
                    : (k.isKeyCode(juce::KeyPress::downKey) || k.isKeyCode(juce::KeyPress::leftKey)) ? -1 : 0;
    if (dir == 0) return false;
    const int s = steps();
    setNorm(norm_ + static_cast<float>(dir) * (s > 0 ? 1.0f / static_cast<float>(s) : (k.getModifiers().isAltDown() ? 0.001f : 0.01f)), false);
    return true;
}

int Encoder::steps() const
{
    const int n = param_.getNumSteps();
    return (n > 1 && n < 200) ? n - 1 : 0;
}

juce::String Encoder::valueText() const { return minusSign(param_.getText(norm_, 16)); }

void Encoder::setNorm(float v, bool gesture)
{
    v = juce::jlimit(0.0f, 1.0f, v);
    if (const int s = steps()) v = std::round(v * static_cast<float>(s)) / static_cast<float>(s);
    const float plain = param_.convertFrom0to1(v);
    if (gesture) att_.setValueAsPartOfGesture(plain);
    else att_.setValueAsCompleteGesture(plain);
    tip_.show(*this, valueText());
}

void Encoder::paint(juce::Graphics& g)
{
    const float W = cellW_, cx = W / 2.0f, cy = 16.0f + 50.0f, r = 22.0f;
    g.setColour(col(colour::ink2));
    g.setFont(font(11.0f, 400));
    g.drawText(label_, juce::Rectangle<float>(0, 0, W, 14), juce::Justification::centred);

    const int nLab = opt_.stepLabels.size();
    if (nLab > 1) {   // a stepped scale: a dot and a label per position, the selected one in ink / 600
        const int sel = juce::roundToInt(norm_ * static_cast<float>(nLab - 1));
        for (int i = 0; i < nLab; ++i) {
            const float a = -135.0f + 270.0f * static_cast<float>(i) / static_cast<float>(nLab - 1);
            const bool on = i == sel;
            const float rd = on ? 1.4f : 1.1f;
            const auto pt = at(cx, cy, a, 32.0f);
            g.setColour(col(on ? colour::ink : colour::inkMuted));
            g.fillEllipse(pt.x - rd, pt.y - rd, 2 * rd, 2 * rd);
            g.setFont(font(10.0f, on ? 600 : 400));
            const auto lp = at(cx, cy, a, 41.0f);
            const auto just = std::fabs(a) < 20.0f ? juce::Justification::horizontallyCentred : a < 0.0f ? juce::Justification::right : juce::Justification::left;
            g.drawSingleLineText(minusSign(opt_.stepLabels[i]), juce::roundToInt(lp.x), juce::roundToInt(lp.y + 3.5f), just);
        }
    } else {
        g.setColour(col(colour::inkMuted));
        const int ticks = steps() > 0 && steps() <= 12 ? steps() + 1 : 11;
        for (int i = 0; i < ticks; ++i) {
            const auto pt = at(cx, cy, -135.0f + 270.0f * static_cast<float>(i) / static_cast<float>(ticks - 1), 32.0f);
            g.fillEllipse(pt.x - 1.1f, pt.y - 1.1f, 2.2f, 2.2f);
        }
        g.setFont(font(10.0f, 400));
        const auto l = at(cx, cy, -135.0f, 41.0f), h = at(cx, cy, 135.0f, 41.0f);
        g.drawSingleLineText(minusSign(lo_), juce::roundToInt(l.x), juce::roundToInt(l.y + 3.5f), juce::Justification::right);
        g.drawSingleLineText(minusSign(hi_), juce::roundToInt(h.x), juce::roundToInt(h.y + 3.5f), juce::Justification::left);
    }

    const float ang = -135.0f + 270.0f * norm_;
    const float from = bipolar_ ? 0.0f : -135.0f;
    if (nLab <= 1 && std::fabs(ang - from) > 0.5f) {   // (a stepped scale has no arc)
        g.setColour(col(colour::ink2));
        g.strokePath(arc(cx, cy, 26.0f, std::min(from, ang), std::max(from, ang)), juce::PathStrokeType(2.5f));
    }
    g.setColour(col(colour::bg500));
    g.fillEllipse(cx - r, cy - r, 2 * r, 2 * r);
    g.setColour(col(colour::inkMuted));
    g.drawEllipse(cx - r, cy - r, 2 * r, 2 * r, 1.5f);
    const auto p = at(cx, cy, ang, r - 5.0f);
    g.setColour(col(colour::ink));
    g.drawLine(cx, cy, p.x, p.y, 2.0f);
    g.fillEllipse(p.x - 1.0f, p.y - 1.0f, 2.0f, 2.0f);

    g.setColour(col(colour::ink));
    g.setFont(font(10.0f, 400));
    g.drawText(valueText(), juce::Rectangle<float>(0, 16 + opt_.knobH + 2, W, 12), juce::Justification::centred);
}

void Encoder::mouseDown(const juce::MouseEvent&)
{
    dragStart_ = norm_;
    dragging_ = true;
    att_.beginGesture();
    tip_.show(*this, valueText());
}

void Encoder::mouseDrag(const juce::MouseEvent& e)
{
    const float dy = -static_cast<float>(e.getDistanceFromDragStartY());
    if (const int s = steps()) {
        setNorm(dragStart_ + std::trunc(dy / 16.0f) / static_cast<float>(s), true);   // one step per 16 pt
        return;
    }
    const float perPx = (e.mods.isAltDown() ? 0.1f : 1.0f) / 200.0f;   // Option: fine
    setNorm(dragStart_ + dy * perPx, true);
}

void Encoder::mouseUp(const juce::MouseEvent&)
{
    if (dragging_) att_.endGesture();
    dragging_ = false;
    if (!isMouseOver()) tip_.show(*this, {});
}

void Encoder::mouseDoubleClick(const juce::MouseEvent&) { setNorm(param_.getDefaultValue(), false); }

void Encoder::mouseWheelMove(const juce::MouseEvent& e, const juce::MouseWheelDetails& w)
{
    if (const int s = steps()) {
        setNorm(norm_ + (w.deltaY > 0 ? 1.0f : -1.0f) / static_cast<float>(s), false);
        return;
    }
    setNorm(norm_ + w.deltaY * (e.mods.isAltDown() ? 0.05f : 0.25f), false);
}

void Encoder::mouseEnter(const juce::MouseEvent&) { tip_.show(*this, valueText()); }
void Encoder::mouseExit(const juce::MouseEvent&)
{
    if (!dragging_) tip_.show(*this, {});
}

// ---- Segmented --------------------------------------------------------------------------------
Segmented::Segmented(juce::RangedAudioParameter& p, juce::StringArray labels, juce::String unit, juce::String a11y)
    : att_(p, [this](float v) { index_ = juce::roundToInt(v); repaint(); }), labels_(std::move(labels)), unit_(std::move(unit))
{
    att_.sendInitialUpdate();
    setTitle(a11y);
}

std::vector<juce::Rectangle<float>> Segmented::boxes() const
{
    std::vector<juce::Rectangle<float>> b;
    float x = 0.0f;
    const auto f = font(11.0f, 600);
    for (const auto& t : labels_) {
        const float w = std::max(26.0f, textWidth(f, t) + 12.0f);
        b.emplace_back(x, 0.0f, w, 22.0f);
        x += w + 1.0f;
    }
    return b;
}

float Segmented::preferredWidth() const
{
    const auto b = boxes();
    const float unitW = unit_.isEmpty() ? 0.0f : 6.0f + textWidth(font(10.0f, 400), unit_) + 2.0f;
    return b.back().getRight() + unitW;
}

void Segmented::paint(juce::Graphics& g)
{
    const auto b = boxes();
    juce::Path clip;
    clip.addRoundedRectangle(0.0f, 0.0f, b.back().getRight(), 22.0f, radius::radiusMd);
    {
        juce::Graphics::ScopedSaveState s(g);
        g.reduceClipRegion(clip);
        for (size_t i = 0; i < b.size(); ++i) {
            const bool on = static_cast<int>(i) == index_;
            g.setColour(col(on ? colour::accent : colour::bg600));
            g.fillRect(b[i]);
            g.setColour(col(on ? colour::accentInk : colour::ink2));
            g.setFont(font(11.0f, 600));
            g.drawText(labels_[static_cast<int>(i)], b[i], juce::Justification::centred);
        }
    }
    if (unit_.isNotEmpty()) {
        g.setColour(col(colour::inkMuted));
        g.setFont(font(10.0f, 400));
        g.drawText(unit_, juce::Rectangle<float>(b.back().getRight() + 6.0f, 0.0f, 40.0f, 22.0f), juce::Justification::centredLeft);
    }
}

void Segmented::mouseDown(const juce::MouseEvent& e)
{
    const auto b = boxes();
    for (size_t i = 0; i < b.size(); ++i)
        if (b[i].contains(e.position)) att_.setValueAsCompleteGesture(static_cast<float>(i));
}

// ---- Toggle -----------------------------------------------------------------------------------
Toggle::Toggle(juce::String label, juce::RangedAudioParameter* p) : label_(std::move(label)), param_(p)
{
    if (param_ != nullptr) {
        att_ = std::make_unique<juce::ParameterAttachment>(*param_, [this](float v) { on_ = v > 0.5f; repaint(); });
        att_->sendInitialUpdate();
    } else {
        disabled_ = true;
        setEnabled(false);
    }
    setTitle(label_);
}

Toggle::Toggle(juce::String label, std::function<bool()> get, std::function<void(bool)> set)
    : label_(std::move(label)), param_(nullptr), get_(std::move(get)), set_(std::move(set))
{
    on_ = get_();
    setTitle(label_);
}

float Toggle::preferredWidth() const { return textWidth(font(11.0f, 600), label_) + 20.0f; }

void Toggle::paint(juce::Graphics& g)
{
    if (disabled_) g.beginTransparencyLayer(0.4f);
    const auto r = getLocalBounds().toFloat();
    g.setColour(col(on_ ? colour::activator : colour::bg600));
    g.fillRoundedRectangle(r, radius::radiusMd);
    g.setColour(col(on_ ? colour::accentInk : colour::ink2));
    g.setFont(font(11.0f, 600));
    g.drawText(label_, r, juce::Justification::centred);
    if (disabled_) g.endTransparencyLayer();
}

void Toggle::mouseDown(const juce::MouseEvent&)
{
    if (att_) att_->setValueAsCompleteGesture(on_ ? 0.0f : 1.0f);
    else if (set_) {
        on_ = !on_;
        set_(on_);
        repaint();
    }
}

// ---- Trim ------------------------------------------------------------------------------------
Trim::Trim(juce::RangedAudioParameter& p, juce::String label)
    : param_(p), att_(p, [this](float v) { db_ = v; repaint(); }), label_(std::move(label))
{
    att_.sendInitialUpdate();
    setTitle(label_);
}

void Trim::setDb(float v, bool gesture)
{
    const auto r = param_.getNormalisableRange();
    v = juce::jlimit(r.start, r.end, v);
    if (gesture) att_.setValueAsPartOfGesture(v);
    else att_.setValueAsCompleteGesture(v);
}

void Trim::paint(juce::Graphics& g)
{
    const float H = static_cast<float>(getHeight());
    const float span = std::max(std::fabs(param_.getNormalisableRange().start), std::fabs(param_.getNormalisableRange().end));
    g.setColour(col(colour::ink2));
    g.setFont(font(11.0f, 400));
    g.drawText(label_, juce::Rectangle<float>(0, 0, 30, H), juce::Justification::centredRight);
    const float kx = 36.0f, c = 11.0f, r = c - 3.0f, cx = kx + c, cy = H / 2.0f;
    g.setColour(col(colour::inkMuted));
    g.drawEllipse(cx - r, cy - r, 2 * r, 2 * r, 1.5f);
    const float v = db_ / span, ang = v * 135.0f;
    if (std::fabs(v) > 1e-6f) {
        g.setColour(col(colour::ink2));
        g.strokePath(arc(cx, cy, r, std::min(0.0f, ang), std::max(0.0f, ang)), juce::PathStrokeType(2.5f));
    }
    const auto p = at(cx, cy, ang, r - 4.0f);
    g.setColour(col(colour::ink));
    g.drawLine(cx, cy, p.x, p.y, 1.5f);
    const juce::Rectangle<float> field(kx + 22.0f + 6.0f, cy - 11.0f, 46.0f, 22.0f);
    g.setColour(col(colour::bg000));
    g.fillRoundedRectangle(field, radius::radiusSm);
    g.setColour(col(colour::ink2));
    g.setFont(font(11.0f, 600));
    g.drawText(minusSign((db_ > 0.04f ? "+" : "") + juce::String(db_, 1) + " dB"), field, juce::Justification::centred);
}

void Trim::mouseDown(const juce::MouseEvent&)
{
    dragStart_ = db_;
    dragging_ = true;
    att_.beginGesture();
}
void Trim::mouseDrag(const juce::MouseEvent& e)
{
    const auto r = param_.getNormalisableRange();
    const float perPx = (e.mods.isAltDown() ? 0.1f : 1.0f) * (r.end - r.start) / 200.0f;
    setDb(dragStart_ - static_cast<float>(e.getDistanceFromDragStartY()) * perPx, true);
}
void Trim::mouseUp(const juce::MouseEvent&)
{
    if (dragging_) att_.endGesture();
    dragging_ = false;
}
void Trim::mouseDoubleClick(const juce::MouseEvent&) { setDb(param_.convertFrom0to1(param_.getDefaultValue()), false); }
void Trim::mouseWheelMove(const juce::MouseEvent& e, const juce::MouseWheelDetails& w)
{
    setDb(db_ + w.deltaY * (e.mods.isAltDown() ? 1.0f : 6.0f), false);
}

// ---- Section ---------------------------------------------------------------------------------
void Section::paint(juce::Graphics& g)
{
    const auto r = getLocalBounds().toFloat().reduced(0.5f);
    g.setColour(col(colour::bg300));
    g.fillRoundedRectangle(r, radius::radiusMd);
    g.setColour(col(colour::line));
    g.drawRoundedRectangle(r, radius::radiusMd, 1.0f);
    g.drawHorizontalLine(21, 1.0f, static_cast<float>(getWidth()) - 1.0f);
    g.setColour(col(colour::inkMuted));
    g.setFont(font(type::micro));
    g.drawText(title_, juce::Rectangle<float>(9.0f, 1.0f, r.getWidth(), 20.0f), juce::Justification::centredLeft);
    if (dividerX_ >= 0.0f) {
        const auto c = content();
        g.setColour(col(colour::line));
        g.fillRect(dividerX_, c.getY(), 1.0f, c.getHeight());
    }
}

juce::Rectangle<float> Section::content() const
{
    return getLocalBounds().toFloat().withTrimmedTop(22.0f + 4.0f).withTrimmedBottom(1.0f + 8.0f).reduced(11.0f, 0.0f);
}

// ---- Meter -----------------------------------------------------------------------------------
Meter::Meter(PeakSource src) : src_(std::move(src))
{
    setTitle("Output level");
    startTimerHz(30);
}

void Meter::timerCallback()
{
    for (int ch = 0; ch < 2; ++ch) {
        const float pk = src_ ? src_(ch) : 0.0f;
        const float db = pk > 0.0f ? 20.0f * std::log10(pk) : -120.0f;
        if (pk > 1.0f) clip_ = true;
        level_[ch] = std::max(db, level_[ch] - 26.0f / 30.0f);   // instant attack, 26 dB/s release
        if (db >= hold_[ch]) {
            hold_[ch] = db;
            holdTicks_[ch] = 45;   // 1.5 s
        } else if (--holdTicks_[ch] <= 0) {
            hold_[ch] = std::max(level_[ch], hold_[ch] - 26.0f / 30.0f);
        }
    }
    repaint();
}

void Meter::paint(juce::Graphics& g)
{
    constexpr float lo = -60.0f, hi = 6.0f;
    auto frac = [](float db) { return juce::jlimit(0.0f, 1.0f, (db - lo) / (hi - lo)); };
    const float x0 = 2.0f;
    const float barsTop = 4.0f, h = static_cast<float>(getHeight()) - barsTop, z = (1.0f - frac(0.0f)) * h;
    g.setColour(col(clip_ && !dark_ ? colour::meterOver : colour::bg000));
    g.fillRoundedRectangle(x0, 0.0f, 7.0f, 3.0f, 1.0f);
    for (int ch = 0; ch < 2; ++ch) {
        const float bx = x0 + static_cast<float>(ch) * 4.0f;
        g.setColour(col(colour::bg000));
        g.fillRect(bx, barsTop, 3.0f, h);
        if (dark_) continue;
        const float db = level_[ch], top = (1.0f - frac(db)) * h;
        const float greenTop = std::max(top, z);
        g.setColour(col(colour::ok));
        g.fillRect(bx, barsTop + greenTop, 3.0f, h - greenTop);
        if (db > 0.0f) {
            g.setColour(col(colour::meterOver));
            g.fillRect(bx, barsTop + top, 3.0f, z - top);
        }
        if (hold_[ch] > lo) {
            g.setColour(col(hold_[ch] > 0.0f ? colour::meterOver : colour::activator));
            g.fillRect(bx, barsTop + (1.0f - frac(hold_[ch])) * h, 3.0f, 1.0f);
        }
    }
    g.setColour(col(colour::ink));
    g.fillRect(0.0f, barsTop + z, 11.0f, 1.0f);
}

// ---- GrMeter ---------------------------------------------------------------------------------
GrMeter::GrMeter(Source src) : src_(std::move(src))
{
    setTitle("Gain reduction");
    startTimerHz(30);
}

void GrMeter::timerCallback()
{
    const float v = src_ ? src_() : 0.0f;
    const float g = std::isfinite(v) ? juce::jlimit(0.0f, 60.0f, v) : 0.0f;
    if (std::fabs(g - gr_) < 0.005f) return;
    gr_ = g;
    repaint();
}

float GrMeter::needleAngle(float grDb) { return 38.0f - std::min(20.0f, std::max(0.0f, grDb)) / 20.0f * 76.0f; }

juce::String GrMeter::text() const
{
    const float r = std::round(gr_ * 10.0f) / 10.0f;
    return r <= 0.0f ? juce::String("0.0 dB") : juce::String::charToString(0x2212) + juce::String(r, 1) + " dB";
}

void GrMeter::paint(juce::Graphics& g)
{
    const float w = static_cast<float>(getWidth()), h = static_cast<float>(getHeight());
    const float cx = w / 2.0f, cy = h + 30.0f, r = 74.0f;   // the pivot is below the meter (seamgen gr_meter)
    g.setColour(col(colour::bg000));
    g.fillRoundedRectangle(getLocalBounds().toFloat(), radius::radiusMd);
    juce::Graphics::ScopedSaveState s(g);
    g.reduceClipRegion(getLocalBounds());
    g.setColour(col(colour::inkMuted));
    g.strokePath(arc(cx, cy, r, -38.0f, 38.0f), juce::PathStrokeType(1.0f));
    for (int d : {20, 10, 7, 5, 3, 1, 0}) {
        const float a = needleAngle(static_cast<float>(d));
        const bool major = d == 20 || d == 10 || d == 0;
        const auto p0 = at(cx, cy, a, r), p1 = at(cx, cy, a, r + (major ? 7.0f : 4.0f));
        g.setColour(col(colour::ink2));
        g.drawLine(p0.x, p0.y, p1.x, p1.y, 1.2f);
        if (major) {
            const auto lp = at(cx, cy, a, r + 15.0f);
            g.setFont(font(10.0f, 400));
            g.drawSingleLineText(juce::String(d), juce::roundToInt(lp.x), juce::roundToInt(lp.y + 3.0f), juce::Justification::horizontallyCentred);
        }
    }
    const auto n = at(cx, cy, needleAngle(gr_), r - 2.0f);
    g.setColour(col(colour::accent));
    g.strokePath([&] { juce::Path p; p.startNewSubPath(cx, cy); p.lineTo(n.x, n.y); return p; }(),
                 juce::PathStrokeType(1.8f, juce::PathStrokeType::curved, juce::PathStrokeType::rounded));
    g.setFont(font(10.0f, 600));
    g.setColour(col(colour::inkMuted));
    g.drawSingleLineText("GR", 6, juce::roundToInt(h - 5.0f), juce::Justification::left);
    g.setColour(col(colour::ink));
    g.drawSingleLineText(text(), juce::roundToInt(w - 6.0f), juce::roundToInt(h - 5.0f), juce::Justification::right);
}

// ---- Header ----------------------------------------------------------------------------------
Header::Header(juce::RangedAudioParameter& on, juce::String title, bool titleLeft)
    : att_(on, [this](float v) {
          on_ = v > 0.5f;
          repaint();
          if (onChange) onChange();
      }),
      title_(std::move(title)), titleLeft_(titleLeft)
{
    setTitle(title_);
    att_.sendInitialUpdate();
}

void Header::paint(juce::Graphics& g)
{
    const float H = static_cast<float>(getHeight());
    const juce::Rectangle<float> sq(0.5f, (H - 12.0f) / 2.0f + 0.5f, 11.0f, 11.0f);
    g.setColour(col(on_ ? colour::inkMuted : colour::bg600));
    g.fillRoundedRectangle(sq, radius::radiusMd);
    g.setColour(col(colour::lineStrong));
    g.drawRoundedRectangle(sq, radius::radiusMd, 1.0f);
    g.setFont(font(type::heading));
    if (titleLeft_) {   // seamgen plugin_window: the name after the square, in ink
        g.setColour(col(colour::ink));
        g.drawText(title_, getLocalBounds().toFloat().withTrimmedLeft(20.0f), juce::Justification::centredLeft);
        return;
    }
    g.setColour(col(on_ ? colour::ink2 : colour::inkMuted).withMultipliedAlpha(on_ ? 1.0f : 0.4f));
    g.drawText(title_, getLocalBounds().toFloat(), juce::Justification::centred);
}

void Header::mouseDown(const juce::MouseEvent& e)
{
    if (e.position.x <= 16.0f) att_.setValueAsCompleteGesture(on_ ? 0.0f : 1.0f);
}

// ---- 126 pt encoder cells ------------------------------------------------------------------------------------------
namespace {
constexpr float kCx = 63.0f, kTop = 20.0f, kCy = kTop + 50.0f;   // knob drawing: 104 tall from y 20, centre 50 below
constexpr float kValY = kTop + 104.0f + 6.0f;                    // value lines: 12 pt, 2 pt apart

float posAngle(int i, int n) { return -135.0f + 270.0f * static_cast<float>(i) / static_cast<float>(std::max(1, n - 1)); }

// A scale label at angle a, radius r (seamgen: centred within 20 deg of the top, else ending / starting there).
juce::Rectangle<float> labelBox(const juce::String& t, float a, float r, bool bold)
{
    const auto p = at(kCx, kCy, a, r);
    const float w = textWidth(font(10.0f, bold ? 600 : 400), t);
    const float x = std::fabs(a) < 20.0f ? p.x - w / 2.0f : (a < 0.0f ? p.x - w : p.x);
    return {x, p.y + 3.5f - 9.5f, w, 12.0f};
}

void drawLabel(juce::Graphics& g, const juce::String& t, float a, float r, bool sel)
{
    g.setColour(col(sel ? colour::ink : colour::inkMuted));
    g.setFont(font(10.0f, sel ? 600 : 400));
    const auto p = at(kCx, kCy, a, r);
    const auto just = std::fabs(a) < 20.0f ? juce::Justification::horizontallyCentred
                                           : (a < 0.0f ? juce::Justification::right : juce::Justification::left);
    g.drawSingleLineText(t, juce::roundToInt(p.x), juce::roundToInt(p.y + 3.5f), just);
}

void dot(juce::Graphics& g, float a, float r, bool sel)
{
    const auto p = at(kCx, kCy, a, r);
    const float d = sel ? 1.4f : 1.1f;
    g.setColour(col(sel ? colour::ink : colour::inkMuted));
    g.fillEllipse(p.x - d, p.y - d, 2 * d, 2 * d);
}

void pointer(juce::Graphics& g, float a, float r, float width)
{
    const auto p = at(kCx, kCy, a, r);
    g.setColour(col(colour::ink));
    juce::Path l;
    l.startNewSubPath(kCx, kCy);
    l.lineTo(p);
    g.strokePath(l, juce::PathStrokeType(width, juce::PathStrokeType::curved, juce::PathStrokeType::rounded));
}

void drawHead(juce::Graphics& g, const CellHead& h, float w)
{
    if (h.glyph.isNotEmpty()) {
        auto path = eqGlyph(h.glyph);
        path.applyTransform(juce::AffineTransform::translation((w - 24.0f) / 2.0f, 1.0f));
        g.setColour(col(colour::ink2));
        g.strokePath(path, juce::PathStrokeType(1.5f, juce::PathStrokeType::curved, juce::PathStrokeType::rounded));
        return;
    }
    g.setColour(col(colour::inkMuted));
    g.setFont(font(type::micro));
    g.drawText(h.text, juce::Rectangle<float>(0.0f, 0.0f, w, 14.0f), juce::Justification::centred);
}

void valueLine(juce::Graphics& g, const juce::String& t, int line, bool dim, float w)
{
    g.setColour(col(dim ? colour::inkMuted : colour::ink2));
    g.setFont(font(10.0f, 400));
    g.drawText(t, juce::Rectangle<float>(0.0f, kValY + 14.0f * static_cast<float>(line), w, 12.0f), juce::Justification::centred);
}

void unitText(juce::Graphics& g, const juce::String& u)
{
    if (u.isEmpty()) return;
    g.setColour(col(colour::inkMuted));
    g.setFont(font(10.0f, 400));
    g.drawSingleLineText(u, juce::roundToInt(kCx), juce::roundToInt(kTop + 102.0f), juce::Justification::horizontallyCentred);
}

// Accessible value of a parameter-backed control (normalised range, the parameter's text).
class ParamValue final : public juce::AccessibilityValueInterface {
public:
    ParamValue(ParamControl& c, std::function<bool()> inactive = {}) : c_(c), inactive_(std::move(inactive)) {}
    bool isReadOnly() const override { return false; }
    double getCurrentValue() const override { return c_.norm(); }
    void setValue(double v) override { c_.set(static_cast<float>(v), false); }
    juce::String getCurrentValueAsString() const override { return inactive_ && inactive_() ? juce::String("inactive") : c_.text(); }
    void setValueAsString(const juce::String& t) override { c_.set(c_.param.getValueForText(t), false); }
    AccessibleValueRange getRange() const override
    {
        const int s = c_.steps();
        return {{0.0, 1.0}, s > 0 ? 1.0 / s : 0.01};
    }

private:
    ParamControl& c_;
    std::function<bool()> inactive_;
};

int stepsFor(const juce::RangedAudioParameter& p)
{
    const int n = p.getNumSteps();
    return (n > 1 && n < 200) ? n - 1 : 0;
}
} // namespace

juce::Path eqGlyph(const juce::String& name)
{
    static const std::map<juce::String, const char*> paths{
        {"high", "M1 6 H11 M11 6 L23 1.5 M11 6 L23 10.5"},
        {"mid", "M1 6 H7 M17 6 H23 M7 6 L10 2.5 H14 L17 6 L14 9.5 H10 Z"},
        {"low", "M1 1.5 L13 6 M1 10.5 L13 6 M13 6 H23"},
        {"hpf", "M2 11 C4 3 6 2 9 2 H23"}};
    const auto it = paths.find(name);
    return it == paths.end() ? juce::Path{} : juce::Drawable::parseSVGPath(it->second);
}

// ---- ParamControl ---------------------------------------------------------------------------------------------
ParamControl::ParamControl(juce::RangedAudioParameter& p, std::function<void()> changed)
    : param(p), att_(p, [this, cb = std::move(changed)](float v) {
          norm_ = param.convertTo0to1(v);
          if (cb) cb();
      })
{
    att_.sendInitialUpdate();
}

int ParamControl::steps() const { return stepsFor(param); }
int ParamControl::index() const { return juce::roundToInt(norm_ * static_cast<float>(std::max(1, steps()))); }
juce::String ParamControl::text() const { return minusSign(param.getText(norm_, 32)); }

void ParamControl::set(float v, bool gesture)
{
    v = juce::jlimit(0.0f, 1.0f, v);
    if (const int s = steps()) v = std::round(v * static_cast<float>(s)) / static_cast<float>(s);
    const float plain = param.convertFrom0to1(v);
    if (gesture) att_.setValueAsPartOfGesture(plain);
    else att_.setValueAsCompleteGesture(plain);
}

void ParamControl::setIndex(int i)
{
    const int s = std::max(1, steps());
    const float v = static_cast<float>(juce::jlimit(0, s, i)) / static_cast<float>(s);
    set(v, dragging_);
    if (dragging_) start_ = v;   // a drag that began on a label continues from it
}

void ParamControl::begin()
{
    start_ = norm_;
    dragging_ = true;
    att_.beginGesture();
}

void ParamControl::drag(float dyUp, bool fine)
{
    if (const int s = steps()) {
        set(start_ + std::trunc(dyUp / 16.0f) / static_cast<float>(s), true);   // one step per 16 pt, no fine mode
        return;
    }
    set(start_ + dyUp * (fine ? 0.1f : 1.0f) / 200.0f, true);
}

void ParamControl::end()
{
    if (dragging_) att_.endGesture();
    dragging_ = false;
}

void ParamControl::wheel(float deltaY, bool smooth, bool fine)
{
    if (const int s = steps()) {   // one step per notch, or per 16 pt of trackpad scroll
        if (!smooth) {
            set(norm_ + (deltaY > 0 ? 1.0f : -1.0f) / static_cast<float>(s), false);
            return;
        }
        wheelAcc_ += deltaY;
        const float q = 16.0f / 256.0f;
        while (std::fabs(wheelAcc_) >= q) {
            const float d = wheelAcc_ > 0 ? 1.0f : -1.0f;
            set(norm_ + d / static_cast<float>(s), false);
            wheelAcc_ -= d * q;
        }
        return;
    }
    set(norm_ + deltaY * (fine ? 0.05f : 0.25f), false);
}

void ParamControl::step(int dir, bool fine)
{
    if (const int s = steps()) {
        set(norm_ + static_cast<float>(dir) / static_cast<float>(s), false);
        return;
    }
    const float plain = param.convertFrom0to1(norm_) + static_cast<float>(dir) * (fine ? 0.1f : 0.5f);
    set(param.convertTo0to1(plain), false);
}

void ParamControl::reset() { set(param.getDefaultValue(), false); }

// ---- ScaleKnob ------------------------------------------------------------------------------------------------
ScaleKnob::ScaleKnob(juce::RangedAudioParameter& p, CellHead head, Style st, ValueTip& tip)
    : ctl_(p, [this] { repaint(); if (ctl_.dragging()) showTip(); }), head_(std::move(head)), st_(std::move(st)), tip_(tip)
{
    setTitle(st_.name);
    setWantsKeyboardFocus(true);
}

void ScaleKnob::showTip() { tip_.show(*this, st_.name + " " + ctl_.text(), -14.0f); }

void ScaleKnob::paint(juce::Graphics& g)
{
    drawHead(g, head_, kW);
    float ang = 0.0f;
    if (st_.kind == Kind::stepped) {
        const int n = st_.labels.size(), idx = ctl_.index();
        for (int i = 0; i < n; ++i) {
            const float a = posAngle(i, n);
            dot(g, a, 32.0f, i == idx);
            if (i % std::max(1, st_.every) == 0) drawLabel(g, st_.labels[i], a, 41.0f, i == idx);
        }
        ang = posAngle(idx, n);
    } else {
        for (int i = 0; i < 11; ++i) dot(g, -135.0f + 27.0f * static_cast<float>(i), 32.0f, false);
        const float v = ctl_.norm();
        if (st_.kind == Kind::bipolar) {
            for (auto [a, t] : {std::pair{-135.0f, "-"}, std::pair{0.0f, "0"}, std::pair{135.0f, "+"}}) {
                g.setColour(col(colour::inkMuted));
                g.setFont(font(10.0f, 400));
                const auto pt = at(kCx, kCy, a, 41.0f);
                g.drawSingleLineText(minusSign(t), juce::roundToInt(pt.x), juce::roundToInt(pt.y + 3.5f), juce::Justification::horizontallyCentred);
            }
            ang = (2.0f * v - 1.0f) * 135.0f;
            if (std::fabs(ang) > 0.2f) {
                g.setColour(col(colour::ink2));
                g.strokePath(arc(kCx, kCy, 26.0f, std::min(0.0f, ang), std::max(0.0f, ang)), juce::PathStrokeType(2.5f));
            }
        } else {
            drawLabel(g, minusSign(st_.lo), -135.0f, 41.0f, false);
            drawLabel(g, minusSign(st_.hi), 135.0f, 41.0f, false);
            ang = -135.0f + 270.0f * v;
            if (v > 0.002f) {
                g.setColour(col(colour::ink2));
                g.strokePath(arc(kCx, kCy, 26.0f, -135.0f, ang), juce::PathStrokeType(2.5f));
            }
        }
    }
    const float r = 22.0f;
    g.setColour(col(colour::bg500));
    g.fillEllipse(kCx - r, kCy - r, 2 * r, 2 * r);
    g.setColour(col(colour::inkMuted));
    g.drawEllipse(kCx - r, kCy - r, 2 * r, 2 * r, 1.5f);
    pointer(g, ang, r - 5.0f, 2.0f);
    unitText(g, st_.unit);
    valueLine(g, ctl_.text(), 0, false, kW);
    if (hasKeyboardFocus(false)) {
        g.setColour(col(colour::ink2).withAlpha(0.6f));
        g.drawEllipse(kCx - r - 3, kCy - r - 3, 2 * r + 6, 2 * r + 6, 1.0f);
    }
}

int ScaleKnob::labelAt(juce::Point<float> p) const
{
    if (st_.kind != Kind::stepped) return -1;
    const int n = st_.labels.size(), idx = ctl_.index();
    for (int i = 0; i < n; ++i) {
        const float a = posAngle(i, n);
        if (i % std::max(1, st_.every) == 0 && labelBox(st_.labels[i], a, 41.0f, i == idx).expanded(2.0f).contains(p)) return i;
        if (at(kCx, kCy, a, 32.0f).getDistanceFrom(p) <= 5.0f) return i;
    }
    return -1;
}

void ScaleKnob::mouseDown(const juce::MouseEvent& e)
{
    ctl_.begin();
    if (const int i = labelAt(e.position); i >= 0) ctl_.setIndex(i);   // click a label or dot: jump there
    showTip();
}
void ScaleKnob::mouseDrag(const juce::MouseEvent& e) { ctl_.drag(-static_cast<float>(e.getDistanceFromDragStartY()), e.mods.isAltDown()); }
void ScaleKnob::mouseUp(const juce::MouseEvent&)
{
    ctl_.end();
    if (!isMouseOver()) tip_.show(*this, {});
}
void ScaleKnob::mouseDoubleClick(const juce::MouseEvent& e)
{
    if (labelAt(e.position) < 0) ctl_.reset();
}
void ScaleKnob::mouseWheelMove(const juce::MouseEvent& e, const juce::MouseWheelDetails& w)
{
    ctl_.wheel(w.deltaY, w.isSmooth, e.mods.isAltDown());
    showTip();
}
void ScaleKnob::mouseEnter(const juce::MouseEvent&) { showTip(); }
void ScaleKnob::mouseExit(const juce::MouseEvent&)
{
    if (!ctl_.dragging()) tip_.show(*this, {});
}
bool ScaleKnob::keyPressed(const juce::KeyPress& k)
{
    const int c = k.getKeyCode();
    const int dir = (c == juce::KeyPress::upKey || c == juce::KeyPress::rightKey) ? 1 : (c == juce::KeyPress::downKey || c == juce::KeyPress::leftKey) ? -1 : 0;
    if (dir == 0) return false;
    ctl_.step(dir, k.getModifiers().isAltDown());
    return true;
}
std::unique_ptr<juce::AccessibilityHandler> ScaleKnob::createAccessibilityHandler()
{
    return std::make_unique<juce::AccessibilityHandler>(*this, juce::AccessibilityRole::slider, juce::AccessibilityActions{},
                                                        juce::AccessibilityHandler::Interfaces{std::make_unique<ParamValue>(ctl_)});
}

// ---- DualKnob -------------------------------------------------------------------------------------------------
// A focusable, accessible part of the dual encoder (keyboard and screen readers); the parent paints and takes the
// mouse.
class DualKnob::Focus final : public juce::Component {
public:
    Focus(DualKnob& o, Part part, ParamControl& c, juce::String name, std::function<bool()> inactive)
        : owner_(o), part_(part), ctl_(c), inactive_(std::move(inactive))
    {
        setTitle(std::move(name));
        setWantsKeyboardFocus(true);
        setInterceptsMouseClicks(false, false);
    }
    bool keyPressed(const juce::KeyPress& k) override
    {
        const int c = k.getKeyCode();
        const int dir = (c == juce::KeyPress::upKey || c == juce::KeyPress::rightKey) ? 1 : (c == juce::KeyPress::downKey || c == juce::KeyPress::leftKey) ? -1 : 0;
        if (dir == 0) return false;
        owner_.keyPart(part_, dir, k.getModifiers().isAltDown());
        return true;
    }
    void focusGained(FocusChangeType) override { owner_.repaint(); }
    void focusLost(FocusChangeType) override { owner_.repaint(); }
    std::unique_ptr<juce::AccessibilityHandler> createAccessibilityHandler() override
    {
        return std::make_unique<juce::AccessibilityHandler>(*this, juce::AccessibilityRole::slider, juce::AccessibilityActions{},
                                                            juce::AccessibilityHandler::Interfaces{std::make_unique<ParamValue>(ctl_, inactive_)});
    }

private:
    DualKnob& owner_;
    Part part_;
    ParamControl& ctl_;
    std::function<bool()> inactive_;
};

DualKnob::DualKnob(juce::RangedAudioParameter& freq, juce::RangedAudioParameter& gain, CellHead head, juce::StringArray freqLabels,
                   juce::String unit, juce::String freqName, juce::String gainName, ValueTip& tip)
    : freq_(freq, [this] { repaint(); if (freq_.dragging()) showTip(); }), gain_(gain, [this] { repaint(); if (gain_.dragging()) showTip(); }),
      head_(std::move(head)), labels_(std::move(freqLabels)), unit_(std::move(unit)), freqName_(std::move(freqName)),
      gainName_(std::move(gainName)), tip_(tip)
{
    setTitle(head_.a11y);
    focusRing_ = std::make_unique<Focus>(*this, Part::ring, freq_, freqName_, nullptr);
    focusKnob_ = std::make_unique<Focus>(*this, Part::knob, gain_, gainName_, [this] { return off(); });
    addAndMakeVisible(*focusRing_);
    addAndMakeVisible(*focusKnob_);
    setFocusContainerType(FocusContainerType::focusContainer);
}

DualKnob::~DualKnob() = default;

bool DualKnob::off() const { return labels_.size() > 0 && labels_[0] == "Off" && freq_.index() == 0; }

void DualKnob::resized()
{
    focusRing_->setBounds(juce::Rectangle<float>(kCx - 28.0f, kCy - 28.0f, 56.0f, 56.0f).toNearestInt());
    focusKnob_->setBounds(juce::Rectangle<float>(kCx - 16.0f, kCy - 16.0f, 32.0f, 32.0f).toNearestInt());
}

void DualKnob::paint(juce::Graphics& g)
{
    constexpr float ro = 28.0f, rb = 22.0f, ri = 16.0f;
    drawHead(g, head_, kW);
    const bool ringOn = active_ == Part::ring || focusRing_->hasKeyboardFocus(false);
    const bool knobOn = active_ == Part::knob || focusKnob_->hasKeyboardFocus(false);
    g.setColour(col(colour::bg600));
    g.drawEllipse(kCx - (ro + rb) / 2, kCy - (ro + rb) / 2, ro + rb, ro + rb, ro - rb);
    g.setColour(col(ringOn ? colour::ink2 : colour::inkMuted));
    g.drawEllipse(kCx - ro, kCy - ro, 2 * ro, 2 * ro, ringOn ? 1.5f : 1.0f);
    const int n = labels_.size(), idx = freq_.index();
    for (int i = 0; i < n; ++i) {
        const float a = posAngle(i, n);
        dot(g, a, ro + 4.0f, i == idx);
        drawLabel(g, labels_[i], a, ro + 13.0f, i == idx);
    }
    {   // the frequency notch across the ring
        const float fa = posAngle(idx, n);
        const auto p1 = at(kCx, kCy, fa, rb), p2 = at(kCx, kCy, fa, ro);
        juce::Path l;
        l.startNewSubPath(p1);
        l.lineTo(p2);
        g.setColour(col(colour::ink));
        g.strokePath(l, juce::PathStrokeType(2.0f, juce::PathStrokeType::curved, juce::PathStrokeType::rounded));
    }
    const bool dim = off();
    if (dim) g.beginTransparencyLayer(0.4f);
    g.setColour(col(colour::bg500));
    g.fillEllipse(kCx - ri, kCy - ri, 2 * ri, 2 * ri);
    g.setColour(col(knobOn ? colour::ink : colour::inkMuted));
    g.drawEllipse(kCx - ri, kCy - ri, 2 * ri, 2 * ri, 1.5f);
    const float ga = (2.0f * gain_.norm() - 1.0f) * 135.0f, rg = (ri + rb) / 2.0f;
    if (std::fabs(ga) > 0.2f) {
        g.setColour(col(colour::ink2));
        g.strokePath(arc(kCx, kCy, rg, std::min(0.0f, ga), std::max(0.0f, ga)), juce::PathStrokeType(2.5f));
    }
    pointer(g, ga, ri - 4.0f, 2.0f);
    if (dim) g.endTransparencyLayer();
    unitText(g, unit_);
    valueLine(g, freq_.text(), 0, active_ == Part::knob, kW);
    valueLine(g, gain_.text(), 1, active_ == Part::ring || dim, kW);
}

DualKnob::Part DualKnob::partAt(juce::Point<float> p) const
{
    const float d = p.getDistanceFrom({kCx, kCy});
    if (d < 19.0f) return Part::knob;                               // the inner knob and half the gap
    if (d <= 50.0f || labelAt(p) >= 0) return Part::ring;            // the ring with its printed scale
    return Part::none;
}

int DualKnob::labelAt(juce::Point<float> p) const
{
    const int n = labels_.size(), idx = freq_.index();
    for (int i = 0; i < n; ++i) {
        const float a = posAngle(i, n);
        if (labelBox(labels_[i], a, 41.0f, i == idx).expanded(2.0f).contains(p)) return i;
        if (at(kCx, kCy, a, 32.0f).getDistanceFrom(p) <= 4.0f) return i;
    }
    return -1;
}

void DualKnob::setActive(Part p)
{
    if (p == active_) return;
    active_ = p;
    repaint();
}

void DualKnob::showTip()
{
    if (active_ == Part::none) {
        tip_.show(*this, {});
        return;
    }
    tip_.show(*this, active_ == Part::ring ? freqName_ + " " + freq_.text() : gainName_ + " " + gain_.text(), -14.0f);
}

void DualKnob::keyPart(Part part, int dir, bool fine) { (part == Part::ring ? freq_ : gain_).step(dir, fine); }

void DualKnob::mouseMove(const juce::MouseEvent& e)
{
    if (dragPart_ != Part::none) return;
    setActive(partAt(e.position));
    showTip();
}
void DualKnob::mouseEnter(const juce::MouseEvent& e) { mouseMove(e); }
void DualKnob::mouseExit(const juce::MouseEvent&)
{
    if (dragPart_ != Part::none) return;
    setActive(Part::none);
    tip_.show(*this, {});
}
void DualKnob::mouseDown(const juce::MouseEvent& e)
{
    dragPart_ = partAt(e.position);
    setActive(dragPart_);
    if (dragPart_ == Part::none) return;
    auto& c = dragPart_ == Part::ring ? freq_ : gain_;
    c.begin();
    if (dragPart_ == Part::ring)
        if (const int i = labelAt(e.position); i >= 0) c.setIndex(i);
    showTip();
}
void DualKnob::mouseDrag(const juce::MouseEvent& e)
{
    if (dragPart_ == Part::none) return;
    (dragPart_ == Part::ring ? freq_ : gain_).drag(-static_cast<float>(e.getDistanceFromDragStartY()), e.mods.isAltDown());
}
void DualKnob::mouseUp(const juce::MouseEvent& e)
{
    if (dragPart_ != Part::none) (dragPart_ == Part::ring ? freq_ : gain_).end();
    dragPart_ = Part::none;
    if (!isMouseOver()) {
        setActive(Part::none);
        tip_.show(*this, {});
    } else {
        setActive(partAt(e.position));
    }
}
void DualKnob::mouseDoubleClick(const juce::MouseEvent& e)
{
    const Part p = partAt(e.position);
    if (p == Part::knob) gain_.reset();
    else if (p == Part::ring && labelAt(e.position) < 0) freq_.reset();
}
void DualKnob::mouseWheelMove(const juce::MouseEvent& e, const juce::MouseWheelDetails& w)
{
    const Part p = partAt(e.position);
    if (p == Part::none) return;
    setActive(p);
    (p == Part::ring ? freq_ : gain_).wheel(w.deltaY, w.isSmooth, e.mods.isAltDown());
    showTip();
}

} // namespace lkit::seamui
