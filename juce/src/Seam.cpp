#include "lkit/juce/Seam.h"

#include <cmath>

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
void ValueTip::show(juce::Component& anchor, const juce::String& text)
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
    setBounds(juce::Rectangle<float>(a.getCentreX() - w / 2, a.getY() + 22.0f, w, h).toNearestInt());
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
    : param_(p), att_(p, [this](float v) { norm_ = param_.convertTo0to1(v); repaint(); }), label_(std::move(label)),
      lo_(std::move(lo)), hi_(std::move(hi)), cellW_(cellW), bipolar_(bipolar), tip_(tip)
{
    att_.sendInitialUpdate();
    setTitle(label_);
    setDescription("Adjustable");
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

    const float ang = -135.0f + 270.0f * norm_;
    const float from = bipolar_ ? 0.0f : -135.0f;
    if (std::fabs(ang - from) > 0.5f) {
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
    g.drawText(valueText(), juce::Rectangle<float>(0, 16 + 90 + 2, W, 12), juce::Justification::centred);
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

// ---- Header ----------------------------------------------------------------------------------
Header::Header(juce::RangedAudioParameter& on, juce::String title)
    : att_(on, [this](float v) {
          on_ = v > 0.5f;
          repaint();
          if (onChange) onChange();
      }),
      title_(std::move(title))
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
    g.setColour(col(on_ ? colour::ink2 : colour::inkMuted).withMultipliedAlpha(on_ ? 1.0f : 0.4f));
    g.setFont(font(type::heading));
    g.drawText(title_, getLocalBounds().toFloat(), juce::Justification::centred);
}

void Header::mouseDown(const juce::MouseEvent& e)
{
    if (e.position.x <= 16.0f) att_.setValueAsCompleteGesture(on_ ? 0.0f : 1.0f);
}

} // namespace lkit::seamui
