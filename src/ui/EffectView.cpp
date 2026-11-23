#include "ui/EffectView.h"

#include "engine/Effects.h"
#include "ui/Theme.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <cwchar>

namespace ui {

using namespace theme;
using fx::CompressorEffect;
using fx::DelayEffect;
using fx::ChorusEffect;
using fx::EqEffect;
using fx::GateEffect;
using fx::ReverbEffect;

namespace {

constexpr float kMargin = 12.0f;
constexpr float kHeaderH = 46.0f;
constexpr float kRowH = 46.0f;
constexpr float kCheckH = 24.0f;
constexpr float kGap = 8.0f;
constexpr double kMinHz = 20.0, kMaxHz = 20000.0, kRangeDb = 18.0;
const Color kDisabledText = rgb(0x6C6F76);
const Color kDisabledDim = rgb(0x5C5F66);
const Color kBandColors[] = {rgb(0x8D9099), rgb(0xE8A33D), rgb(0x4CD27A), rgb(0x4C9AFF), rgb(0xB57BE6), rgb(0x8D9099)};

std::wstring number(double value, int decimals) {
    wchar_t buffer[48];
    std::swprintf(buffer, 48, L"%.*f", decimals, value);
    std::wstring text = buffer;
    for (auto& ch : text)
        if (ch == L'.') ch = L',';
    if (!text.empty() && text[0] == L'-') {
        bool zero = true;
        for (wchar_t ch : text)
            if (ch >= L'1' && ch <= L'9') zero = false;
        text = zero ? text.substr(1) : L"−" + text.substr(1);
    }
    return text;
}

std::wstring hzText(double hz) {
    if (hz >= 1000.0) return number(hz / 1000.0, hz >= 10000.0 ? 1 : 2) + L" kHz";
    return number(hz, hz < 10.0 ? 2 : hz < 100.0 ? 1 : 0) + L" Hz";
}

std::wstring valueText(const fx::ParamInfo& p, float value) {
    if (p.toggle) return value >= 0.5f ? L"an" : L"aus";
    if (p.choiceCount > 0) return p.choices[std::clamp(static_cast<int>(value), 0, p.choiceCount - 1)];
    const std::wstring unit = p.unit;
    if (unit == L"Hz") return hzText(value);
    if (unit == L":1") return number(value, value < 10.0f ? 1 : 0) + L" : 1";
    if (unit == L"ms") return number(value, value < 10.0f ? 1 : 0) + L" ms";
    if (unit == L"s") return number(value, 1) + L" s";
    if (unit == L"%") return number(value, 0) + L" %";
    // Plus nur bei Reglern, die in beide Richtungen gehen
    if (unit == L"dB") return (value > 0.04f && p.min < 0.0f ? L"+" : L"") + number(value, 1) + L" dB";
    return number(value, 2);
}

}  // namespace

EffectView::EffectView(fx::BuiltinEffect& effect) : fx_(effect) {
    const std::string id = effect.id();
    float boxW = 160.0f;
    if (id == "EQ") {
        kind_ = Kind::Eq;
        sections_ = {{L"Hochpass", EqEffect::kLowCutOn, {EqEffect::kLowCut}, {}},
                     {L"Tiefen", -1, {EqEffect::kLowFreq, EqEffect::kLowGain}, {}},
                     {L"Mitten 1", -1, {EqEffect::kMid1Freq, EqEffect::kMid1Gain, EqEffect::kMid1Q}, {}},
                     {L"Mitten 2", -1, {EqEffect::kMid2Freq, EqEffect::kMid2Gain, EqEffect::kMid2Q}, {}},
                     {L"Höhen", -1, {EqEffect::kHighFreq, EqEffect::kHighGain}, {}},
                     {L"Tiefpass", EqEffect::kHighCutOn, {EqEffect::kHighCut}, {}},
                     {L"Ausgang", -1, {EqEffect::kOutput}, {}}};
        handles_ = {{EqEffect::kLowCut, -1, -1},
                    {EqEffect::kLowFreq, EqEffect::kLowGain, -1},
                    {EqEffect::kMid1Freq, EqEffect::kMid1Gain, EqEffect::kMid1Q},
                    {EqEffect::kMid2Freq, EqEffect::kMid2Gain, EqEffect::kMid2Q},
                    {EqEffect::kHighFreq, EqEffect::kHighGain, -1},
                    {EqEffect::kHighCut, -1, -1}};
        boxW = 118.0f;
        graphH_ = 230.0f;
    } else if (id == "Kompressor") {
        kind_ = Kind::Compressor;
        sections_ = {{L"Kennlinie", -1, {CompressorEffect::kThreshold, CompressorEffect::kRatio, CompressorEffect::kKnee}, {}},
                     {L"Zeiten", -1, {CompressorEffect::kAttack, CompressorEffect::kRelease}, {}},
                     {L"Pegel", -1, {CompressorEffect::kMakeup, CompressorEffect::kMix}, {CompressorEffect::kAutoMakeup}}};
        boxW = 190.0f;
        graphH_ = 220.0f;
    } else if (id == "Hall") {
        kind_ = Kind::Reverb;
        sections_ = {{L"Raum", -1, {ReverbEffect::kSize, ReverbEffect::kDecay, ReverbEffect::kPredelay}, {}},
                     {L"Klang", -1, {ReverbEffect::kDamping, ReverbEffect::kLowCut}, {}},
                     {L"Ausgabe", -1, {ReverbEffect::kWidth, ReverbEffect::kMix}, {}}};
        boxW = 190.0f;
        graphH_ = 130.0f;
    } else if (id == "Echo") {
        kind_ = Kind::Delay;
        sections_ = {{L"Zeit", -1, {DelayEffect::kNote, DelayEffect::kTime}, {DelayEffect::kSync}},
                     {L"Wiederholungen", -1, {DelayEffect::kFeedback}, {DelayEffect::kPingPong}},
                     {L"Klang", -1, {DelayEffect::kLowCut, DelayEffect::kHighCut}, {}},
                     {L"Ausgabe", -1, {DelayEffect::kMix}, {}}};
        boxW = 150.0f;
        graphH_ = 130.0f;
    } else if (id == "Gate") {
        kind_ = Kind::Gate;
        sections_ = {{L"Schwelle", -1, {GateEffect::kThreshold, GateEffect::kHysteresis}, {}},
                     {L"Zeiten", -1, {GateEffect::kAttack, GateEffect::kHold, GateEffect::kRelease}, {}},
                     {L"Wirkung", -1, {GateEffect::kRange, GateEffect::kDetectLowCut}, {}}};
        boxW = 190.0f;
        graphH_ = 110.0f;
    } else if (id == "Chorus") {
        kind_ = Kind::Chorus;
        sections_ = {{L"Bewegung", -1, {ChorusEffect::kRate, ChorusEffect::kDepth}, {}},
                     {L"Klang", -1, {ChorusEffect::kDelay, ChorusEffect::kHighCut}, {}},
                     {L"Ausgabe", -1, {ChorusEffect::kWidth, ChorusEffect::kMix}, {}}};
        boxW = 190.0f;
        graphH_ = 110.0f;
    } else {
        std::vector<int> all;
        for (int i = 0; i < effect.paramCount(); ++i) all.push_back(i);
        sections_ = {{effect.name(), -1, all, {}}};
        boxW = 260.0f;
    }
    std::size_t rows = 0, checks = 0;
    for (const Section& s : sections_) {
        rows = std::max(rows, s.sliders.size());
        checks = std::max(checks, s.checks.size());
    }
    const auto n = static_cast<float>(sections_.size());
    width_ = 2.0f * kMargin + n * boxW + (n - 1.0f) * kGap;
    height_ = kHeaderH + (graphH_ > 0.0f ? graphH_ + kGap : 0.0f) + 44.0f + static_cast<float>(rows) * kRowH +
              static_cast<float>(checks) * kCheckH + 8.0f + kMargin;
}

void EffectView::set(int param, float value) {
    const float before = fx_.param(param);
    fx_.setParam(param, value);
    if (fx_.param(param) != before && onChange) onChange();
}

// Regler, die gerade nichts bewirken, werden blass gezeichnet.
bool EffectView::enabled(int param) const {
    if (kind_ == Kind::Delay) {
        const bool sync = fx_.param(DelayEffect::kSync) >= 0.5f;
        if (param == DelayEffect::kTime) return !sync;
        if (param == DelayEffect::kNote) return sync;
    }
    if (kind_ == Kind::Compressor && param == CompressorEffect::kMakeup) return true;
    return true;
}

void EffectView::drawSlider(Canvas& c, const Rect& row, int param, bool active) {
    const fx::ParamInfo& p = fx_.info(param);
    const float value = fx_.param(param);
    c.text(p.label, {row.x, row.y, row.w, 18.0f}, active ? kTextDim : kDisabledDim, Font::Small);
    c.text(valueText(p, value), {row.x, row.y, row.w, 18.0f}, active ? kText : kDisabledText, Font::Small, Align::Right);
    const Rect bar{row.x + 2.0f, row.y + 22.0f, row.w - 4.0f, 14.0f};
    float zero = 0.0f;
    if (p.choiceCount == 0 && p.min < 0.0f && p.max > 0.0f) zero = fx::toPosition(p, 0.0f);
    theme::drawSlider(c, bar, fx::toPosition(p, value), zero);
    if (!active) c.fill({bar.x - 4.0f, bar.y, bar.w + 8.0f, bar.h}, rgb(0x2A2C31, 0.55f));
    hits_.push_back({{row.x, row.y, row.w, kRowH - 4.0f}, HitKind::Slider, param, bar.w});
}

void EffectView::drawSection(Canvas& c, const Rect& box, const Section& section) {
    c.fillRounded(box, 6.0f, kPanel);
    const bool on = section.toggle < 0 || fx_.param(section.toggle) >= 0.5f;
    float x = box.x + 12.0f;
    if (section.toggle >= 0) {
        const Rect pill{x, box.y + 12.0f, 30.0f, 16.0f};
        c.fillRounded(pill, 8.0f, on ? kAccent : kButton);
        c.fillRounded({on ? pill.right() - 14.0f : pill.x + 2.0f, pill.y + 2.0f, 12.0f, 12.0f}, 6.0f, on ? kText : kTextDim);
        hits_.push_back({{box.x, box.y, box.w, 38.0f}, HitKind::Toggle, section.toggle, 0.0f});
        x = pill.right() + 10.0f;
    }
    c.text(section.title, {x, box.y + 8.0f, box.right() - x - 8.0f, 24.0f}, on ? kText : kTextDim);

    float y = box.y + 44.0f;
    for (int param : section.sliders) {
        drawSlider(c, {box.x + 12.0f, y, box.w - 24.0f, kRowH}, param, on && enabled(param));
        y += kRowH;
    }
    for (int param : section.checks) {
        const bool checked = fx_.param(param) >= 0.5f;
        const Rect check{box.x + 12.0f, y + 4.0f, 14.0f, 14.0f};
        c.fillRounded(check, 3.0f, checked ? kAccent : kDark);
        c.strokeRounded(check, 3.0f, kLine);
        if (checked) c.text(L"✓", {check.x, check.y - 3.0f, check.w, 18.0f}, kText, Font::Small, Align::Center);
        c.text(fx_.info(param).label, {check.right() + 8.0f, y + 1.0f, box.right() - check.right() - 20.0f, 20.0f}, kText,
               Font::Small);
        hits_.push_back({{box.x + 8.0f, y, box.w - 16.0f, 22.0f}, HitKind::Toggle, param, 0.0f});
        y += kCheckH;
    }
}

// --- EQ-Kurve ------------------------------------------------------------------

float EffectView::freqToX(double hz) const {
    const double t = std::log(std::clamp(hz, kMinHz, kMaxHz) / kMinHz) / std::log(kMaxHz / kMinHz);
    return graph_.x + static_cast<float>(t) * graph_.w;
}

double EffectView::xToFreq(float x) const {
    const double t = std::clamp((x - graph_.x) / graph_.w, 0.0f, 1.0f);
    return kMinHz * std::pow(kMaxHz / kMinHz, t);
}

float EffectView::gainToY(double db) const {
    const double t = std::clamp(db, -kRangeDb, kRangeDb) / kRangeDb;  // -1..1
    return graph_.y + graph_.h * 0.5f - static_cast<float>(t) * graph_.h * 0.5f;
}

double EffectView::yToGain(float y) const {
    return std::clamp(static_cast<double>((graph_.y + graph_.h * 0.5f - y) / (graph_.h * 0.5f)), -1.0, 1.0) * kRangeDb;
}

float EffectView::handleY(const Handle& h) const { return h.gain >= 0 ? gainToY(fx_.param(h.gain)) : gainToY(0.0); }

int EffectView::handleAt(float x, float y) const {
    int best = -1;
    float bestDistance = 12.0f;
    for (std::size_t i = 0; i < handles_.size(); ++i) {
        const float dx = x - freqToX(fx_.param(handles_[i].freq)), dy = y - handleY(handles_[i]);
        const float distance = std::sqrt(dx * dx + dy * dy);
        if (distance < bestDistance) {
            bestDistance = distance;
            best = static_cast<int>(i);
        }
    }
    return best;
}

void EffectView::drawEqGraph(Canvas& c, const Rect& area) {
    graph_ = {area.x + 40.0f, area.y + 10.0f, area.w - 52.0f, area.h - 30.0f};
    c.fillRounded(area, 6.0f, kPanel);
    c.fill(graph_, kDark);
    for (double hz : {50.0, 100.0, 200.0, 500.0, 1000.0, 2000.0, 5000.0, 10000.0}) {
        const float x = freqToX(hz);
        c.line(x, graph_.y, x, graph_.bottom(), kGrid);
        c.text(hz >= 1000.0 ? number(hz / 1000.0, 0) + L"k" : number(hz, 0), {x - 20.0f, graph_.bottom() + 2.0f, 40.0f, 16.0f},
               kTextDim, Font::Small, Align::Center);
    }
    for (double db : {-12.0, -6.0, 0.0, 6.0, 12.0}) {
        const float y = gainToY(db);
        c.line(graph_.x, y, graph_.right(), y, db == 0.0 ? kLine : kGrid);
        c.text((db > 0 ? L"+" : L"") + number(db, 0), {area.x + 4.0f, y - 8.0f, 32.0f, 16.0f}, kTextDim, Font::Small,
               Align::Right);
    }
    // Kurve aus den aktuellen Werten
    float values[EqEffect::kCount];
    for (int i = 0; i < EqEffect::kCount; ++i) values[i] = fx_.param(i);
    const double fs = std::max(fx_.sampleRate(), 44100.0);
    float lastX = 0.0f, lastY = 0.0f;
    for (int px = 0; px <= static_cast<int>(graph_.w); px += 2) {
        const float x = graph_.x + static_cast<float>(px);
        const float y = gainToY(EqEffect::responseDb(values, fs, xToFreq(x)));
        if (px > 0) c.line(lastX, lastY, x, y, kAccent, 2.0f);
        lastX = x;
        lastY = y;
    }
    // Griffe
    static const wchar_t* const names[] = {L"HP", L"1", L"2", L"3", L"4", L"TP"};
    for (std::size_t i = 0; i < handles_.size(); ++i) {
        const Handle& h = handles_[i];
        bool active = true;
        if (i == 0) active = fx_.param(EqEffect::kLowCutOn) >= 0.5f;
        if (i == 5) active = fx_.param(EqEffect::kHighCutOn) >= 0.5f;
        const float x = freqToX(fx_.param(h.freq)), y = handleY(h);
        const Color color = active ? kBandColors[i] : kButton;
        c.fillRounded({x - 8.0f, y - 8.0f, 16.0f, 16.0f}, 8.0f, color);
        c.text(names[i], {x - 10.0f, y - 8.0f, 20.0f, 16.0f}, kDark, Font::Small, Align::Center);
    }
}

// --- Kompressor: Kennlinie und Absenkung ------------------------------------------

void EffectView::drawCompressorGraph(Canvas& c, const Rect& area) {
    c.fillRounded(area, 6.0f, kPanel);
    const auto& comp = static_cast<const CompressorEffect&>(fx_);
    const float side = area.h - 34.0f;
    const Rect g{area.x + 44.0f, area.y + 12.0f, side, side};
    c.fill(g, kDark);
    const auto toX = [&](double db) { return g.x + static_cast<float>((std::clamp(db, -60.0, 0.0) + 60.0) / 60.0) * g.w; };
    const auto toY = [&](double db) { return g.bottom() - static_cast<float>((std::clamp(db, -60.0, 0.0) + 60.0) / 60.0) * g.h; };
    for (double db : {-48.0, -36.0, -24.0, -12.0}) {
        c.line(toX(db), g.y, toX(db), g.bottom(), kGrid);
        c.line(g.x, toY(db), g.right(), toY(db), kGrid);
        c.text(number(db, 0), {g.x - 40.0f, toY(db) - 8.0f, 34.0f, 16.0f}, kTextDim, Font::Small, Align::Right);
        c.text(number(db, 0), {toX(db) - 20.0f, g.bottom() + 2.0f, 40.0f, 16.0f}, kTextDim, Font::Small, Align::Center);
    }
    c.line(g.x, g.bottom(), g.right(), g.y, kLine);  // 1:1
    const double threshold = fx_.param(CompressorEffect::kThreshold), ratio = fx_.param(CompressorEffect::kRatio),
                 knee = fx_.param(CompressorEffect::kKnee);
    float lastX = 0.0f, lastY = 0.0f;
    for (int i = 0; i <= 120; ++i) {
        const double in = -60.0 + i * 0.5;
        const float x = toX(in), y = toY(CompressorEffect::curve(in, threshold, ratio, knee));
        if (i > 0) c.line(lastX, lastY, x, y, kAccent, 2.0f);
        lastX = x;
        lastY = y;
    }
    const double input = comp.inputDb();
    if (input > -60.0) {
        const float x = toX(input), y = toY(CompressorEffect::curve(input, threshold, ratio, knee));
        c.fillRounded({x - 5.0f, y - 5.0f, 10.0f, 10.0f}, 5.0f, kText);
    }

    // Rechts: Absenkung als Balken und in Zahlen
    const float infoX = g.right() + 30.0f, infoW = area.right() - infoX - 16.0f;
    const double reduction = comp.reductionDb();
    c.text(L"Absenkung", {infoX, area.y + 12.0f, infoW, 18.0f}, kTextDim, Font::Small);
    c.text(number(reduction, 1) + L" dB", {infoX, area.y + 30.0f, infoW, 30.0f}, kText, Font::Large);
    const Rect bar{infoX, area.y + 72.0f, infoW, 10.0f};
    c.fillRounded(bar, 3.0f, kDark);
    const float w = bar.w * static_cast<float>(std::clamp(reduction / 24.0, 0.0, 1.0));
    if (w > 0.5f) c.fillRounded({bar.x, bar.y, w, bar.h}, 3.0f, kMuteOn);
    double makeup = fx_.param(CompressorEffect::kMakeup);
    if (fx_.param(CompressorEffect::kAutoMakeup) >= 0.5f) makeup += CompressorEffect::autoMakeup(threshold, ratio);
    c.text(L"Aufholen gesamt", {infoX, area.y + 100.0f, infoW, 18.0f}, kTextDim, Font::Small);
    c.text(L"+" + number(makeup, 1) + L" dB", {infoX, area.y + 100.0f, infoW, 18.0f}, kText, Font::Small, Align::Right);
    c.text(L"Eingang", {infoX, area.y + 122.0f, infoW, 18.0f}, kTextDim, Font::Small);
    c.text(input > -100.0 ? number(input, 1) + L" dB" : std::wstring(L"—"), {infoX, area.y + 122.0f, infoW, 18.0f},
           kText, Font::Small, Align::Right);
    c.text(L"Der Punkt zeigt, wo das Signal", {infoX, area.y + 150.0f, infoW, 16.0f}, kTextDim, Font::Small);
    c.text(L"gerade auf der Kennlinie liegt.", {infoX, area.y + 165.0f, infoW, 16.0f}, kTextDim, Font::Small);
}

// --- Hall: Abklingkurve ------------------------------------------------------------

void EffectView::drawReverbGraph(Canvas& c, const Rect& area) {
    c.fillRounded(area, 6.0f, kPanel);
    const Rect g{area.x + 44.0f, area.y + 12.0f, area.w - 60.0f, area.h - 36.0f};
    c.fill(g, kDark);
    const double predelay = fx_.param(ReverbEffect::kPredelay) * 0.001, decay = fx_.param(ReverbEffect::kDecay);
    const double span = std::max(1.0, (predelay + decay) * 1.15);
    const auto toX = [&](double s) { return g.x + static_cast<float>(std::clamp(s / span, 0.0, 1.0)) * g.w; };
    const auto toY = [&](double db) { return g.y + static_cast<float>(std::clamp(-db / 60.0, 0.0, 1.0)) * g.h; };
    const double step = span > 6.0 ? 2.0 : span > 2.5 ? 1.0 : 0.5;
    for (double s = step; s < span; s += step) {
        c.line(toX(s), g.y, toX(s), g.bottom(), kGrid);
        c.text(number(s, step < 1.0 ? 1 : 0) + L" s", {toX(s) - 24.0f, g.bottom() + 2.0f, 48.0f, 16.0f}, kTextDim,
               Font::Small, Align::Center);
    }
    for (double db : {-20.0, -40.0}) {
        c.line(g.x, toY(db), g.right(), toY(db), kGrid);
        c.text(number(db, 0) + L" dB", {area.x + 2.0f, toY(db) - 8.0f, 40.0f, 16.0f}, kTextDim, Font::Small, Align::Right);
    }
    // Direktschall, Vorverzoegerung, dann linear (in dB) abfallend bis -60 dB nach "Nachhall"
    c.line(g.x, g.y, g.x, g.bottom(), kText, 2.0f);
    const float start = toX(predelay);
    c.line(start, toY(-6.0), toX(predelay + decay), g.bottom(), kAccent, 2.0f);
    c.line(start, g.bottom(), start, toY(-6.0), kAccent, 2.0f);
    c.text(L"Nachhall " + number(decay, 1) + L" s bis −60 dB", {g.right() - 220.0f, g.y + 4.0f, 214.0f, 18.0f},
           kTextDim, Font::Small, Align::Right);
}

// --- Echo: Wiederholungen ---------------------------------------------------------

void EffectView::drawDelayGraph(Canvas& c, const Rect& area) {
    c.fillRounded(area, 6.0f, kPanel);
    const auto& delay = static_cast<const DelayEffect&>(fx_);
    float values[DelayEffect::kCount];
    for (int i = 0; i < DelayEffect::kCount; ++i) values[i] = fx_.param(i);
    const double tempo = delay.lastTempo();
    const double seconds = DelayEffect::delaySeconds(values, tempo);
    const double feedback = values[DelayEffect::kFeedback] * 0.01;
    const bool pingPong = values[DelayEffect::kPingPong] >= 0.5f;

    const Rect g{area.x + 16.0f, area.y + 30.0f, area.w - 32.0f, area.h - 50.0f};
    c.fill(g, kDark);
    std::wstring info = L"Verzögerung " + number(seconds * 1000.0, 0) + L" ms";
    if (values[DelayEffect::kSync] >= 0.5f) info += L" (" + std::wstring(fx_.info(DelayEffect::kNote).choices[static_cast<int>(values[DelayEffect::kNote])]) + L" bei " + number(tempo, 0) + L" BPM)";
    c.text(info, {area.x + 16.0f, area.y + 6.0f, area.w - 32.0f, 20.0f}, kText, Font::Small);
    if (pingPong)
        c.text(L"blau links, gelb rechts", {area.x + 16.0f, area.y + 6.0f, area.w - 32.0f, 20.0f}, kTextDim, Font::Small,
               Align::Right);

    const int taps = 8;
    const double span = seconds * (taps + 0.5);
    const auto toX = [&](double s) { return g.x + static_cast<float>(s / span) * g.w; };
    c.fill({toX(0.0) + 2.0f, g.y + 4.0f, 6.0f, g.h - 4.0f}, kText);  // Original
    double level = 1.0;
    for (int n = 1; n <= taps; ++n) {
        const float h = (g.h - 4.0f) * static_cast<float>(level);
        if (h < 1.0f) break;
        const Color color = pingPong && n % 2 == 0 ? kSoloOn : kAccent;
        c.fill({toX(seconds * n) - 3.0f, g.bottom() - h, 6.0f, h}, color);
        level *= feedback;
    }
    c.text(number(span, 1) + L" s", {g.right() - 60.0f, g.bottom() + 2.0f, 60.0f, 16.0f}, kTextDim, Font::Small,
           Align::Right);
}

// --- Gate: Pegel, Schwelle und Zustand -----------------------------------------------

void EffectView::drawGateGraph(Canvas& c, const Rect& area) {
    c.fillRounded(area, 6.0f, kPanel);
    const auto& gate = static_cast<const GateEffect&>(fx_);
    const Rect bar{area.x + 16.0f, area.y + 40.0f, area.w - 150.0f, 22.0f};
    const auto toX = [&](double db) { return bar.x + static_cast<float>((std::clamp(db, -80.0, 0.0) + 80.0) / 80.0) * bar.w; };
    c.fill(bar, kDark);
    const double threshold = fx_.param(GateEffect::kThreshold), hysteresis = fx_.param(GateEffect::kHysteresis);
    // Bereich der Hysterese: hier bleibt ein offenes Gate offen
    c.fill({toX(threshold - hysteresis), bar.y, toX(threshold) - toX(threshold - hysteresis), bar.h}, rgb(0x4C9AFF, 0.25f));
    const double input = gate.inputDb();
    if (input > -80.0) c.fill({bar.x, bar.y + 6.0f, toX(input) - bar.x, bar.h - 12.0f}, gate.isOpen() ? kMeter : kTextDim);
    c.line(toX(threshold), bar.y - 6.0f, toX(threshold), bar.bottom() + 6.0f, kText, 2.0f);
    for (double db : {-60.0, -40.0, -20.0, 0.0})
        c.text(number(db, 0), {toX(db) - 20.0f, bar.bottom() + 6.0f, 40.0f, 16.0f}, kTextDim, Font::Small, Align::Center);
    c.text(L"Eingang", {bar.x, area.y + 12.0f, 200.0f, 18.0f}, kTextDim, Font::Small);
    c.text(L"Schwelle " + number(threshold, 0) + L" dB", {toX(threshold) - 100.0f, area.y + 12.0f, 200.0f, 18.0f}, kText,
           Font::Small, Align::Center);

    // Zustand rechts
    const float infoX = bar.right() + 24.0f, infoW = area.right() - infoX - 16.0f;
    const bool open = gate.isOpen();
    c.fillRounded({infoX, area.y + 16.0f, 14.0f, 14.0f}, 7.0f, open ? kMeter : kRecord);
    c.text(open ? L"offen" : L"zu", {infoX + 22.0f, area.y + 12.0f, infoW - 22.0f, 22.0f}, kText);
    const double gain = gate.gainDb();
    c.text(L"Verstärkung", {infoX, area.y + 44.0f, infoW, 18.0f}, kTextDim, Font::Small);
    c.text(gain <= -79.9 ? std::wstring(L"stumm") : number(gain, 1) + L" dB", {infoX, area.y + 62.0f, infoW, 18.0f}, kText,
           Font::Small);
}

// --- Chorus: Verlauf der Verzoegerung links und rechts -------------------------------

void EffectView::drawChorusGraph(Canvas& c, const Rect& area) {
    c.fillRounded(area, 6.0f, kPanel);
    const Rect g{area.x + 16.0f, area.y + 30.0f, area.w - 32.0f, area.h - 44.0f};
    c.fill(g, kDark);
    const double rate = fx_.param(ChorusEffect::kRate), depth = fx_.param(ChorusEffect::kDepth) * 0.01,
                 width = fx_.param(ChorusEffect::kWidth) * 0.01;
    c.text(L"Verzögerung schwankt " + number(fx_.param(ChorusEffect::kDelay), 0) + L" bis " +
               number(fx_.param(ChorusEffect::kDelay) + depth * ChorusEffect::kMaxModulationMs, 1) + L" ms, einmal in " +
               number(1.0 / rate, rate > 1.0 ? 2 : 1) + L" s",
           {area.x + 16.0f, area.y + 6.0f, area.w - 32.0f, 20.0f}, kText, Font::Small);
    c.text(L"blau links, gelb rechts", {area.x + 16.0f, area.y + 6.0f, area.w - 32.0f, 20.0f}, kTextDim, Font::Small,
           Align::Right);
    // Zwei Perioden, Ausschlag nach "Tiefe"
    for (int ch = 0; ch < 2; ++ch) {
        const double offset = ch == 0 ? 0.0 : width * 0.25;
        float lastX = 0.0f, lastY = 0.0f;
        for (int px = 0; px <= static_cast<int>(g.w); px += 3) {
            const double p = 2.0 * static_cast<double>(px) / g.w + offset;
            const float x = g.x + static_cast<float>(px);
            const float y = g.y + g.h * 0.5f - static_cast<float>(std::sin(2.0 * 3.14159265358979 * p) * depth) * (g.h * 0.42f);
            if (px > 0) c.line(lastX, lastY, x, y, ch == 0 ? kAccent : kSoloOn, 2.0f);
            lastX = x;
            lastY = y;
        }
    }
}

// --- Ganzes Feld -------------------------------------------------------------------

void EffectView::draw(Canvas& c, float width, float height) {
    (void)height;
    hits_.clear();
    c.clear(kBg);

    c.text(fx_.name(), {kMargin + 4.0f, 10.0f, 160.0f, 26.0f}, kText);
    const wchar_t* hint = L"";
    switch (kind_) {
        case Kind::Eq: hint = L"Griffe in der Kurve ziehen: Frequenz und Pegel. Mausrad auf einem Griff: Güte."; break;
        case Kind::Compressor: hint = L"Hält den Pegel gleichmäßig: Was über der Schwelle liegt, wird abgesenkt."; break;
        case Kind::Reverb: hint = L"Tipp: auf einen Bus legen, Mischung 100 %, Spuren über die Sends dazugeben."; break;
        case Kind::Delay: hint = L"Im Takt folgt das Echo dem Tempo des Projekts."; break;
        case Kind::Gate: hint = L"Schaltet leise Stellen stumm, z. B. Rauschen des Mikrofons in Gesangspausen."; break;
        case Kind::Chorus: hint = L"Macht Synths, Gitarren und Stimmen breiter und lebendiger."; break;
        case Kind::Other: break;
    }
    c.text(hint, {kMargin + 120.0f, 13.0f, width - kMargin * 2.0f - 120.0f, 22.0f}, kTextDim, Font::Small);

    float y = kHeaderH;
    if (graphH_ > 0.0f) {
        const Rect area{kMargin, y, width - 2.0f * kMargin, graphH_};
        switch (kind_) {
            case Kind::Eq: drawEqGraph(c, area); break;
            case Kind::Compressor: drawCompressorGraph(c, area); break;
            case Kind::Reverb: drawReverbGraph(c, area); break;
            case Kind::Delay: drawDelayGraph(c, area); break;
            case Kind::Gate: drawGateGraph(c, area); break;
            case Kind::Chorus: drawChorusGraph(c, area); break;
            case Kind::Other: break;
        }
        y += graphH_ + kGap;
    }
    const auto n = static_cast<float>(sections_.size());
    const float boxW = (width - 2.0f * kMargin - (n - 1.0f) * kGap) / n;
    const float boxH = height_ - y - kMargin;
    for (std::size_t i = 0; i < sections_.size(); ++i)
        drawSection(c, {kMargin + static_cast<float>(i) * (boxW + kGap), y, boxW, boxH}, sections_[i]);
}

void EffectView::mouseDown(float x, float y) {
    if (kind_ == Kind::Eq) {
        dragHandle_ = handleAt(x, y);
        if (dragHandle_ >= 0) {
            // Hoch- und Tiefpass schalten sich beim Anfassen ein.
            if (dragHandle_ == 0) set(EqEffect::kLowCutOn, 1.0f);
            if (dragHandle_ == 5) set(EqEffect::kHighCutOn, 1.0f);
            return;
        }
    }
    for (const Hit& hit : hits_) {
        if (!hit.rect.contains(x, y)) continue;
        if (hit.kind == HitKind::Toggle) {
            set(hit.param, fx_.param(hit.param) >= 0.5f ? 0.0f : 1.0f);
        } else {
            dragParam_ = hit.param;
            dragStartX_ = x;
            dragStartPos_ = fx::toPosition(fx_.info(hit.param), fx_.param(hit.param));
            dragBar_ = std::max(40.0f, hit.barWidth);
        }
        return;
    }
}

void EffectView::mouseMove(float x, float y, bool fine) {
    if (dragHandle_ >= 0) {
        const Handle& h = handles_[static_cast<std::size_t>(dragHandle_)];
        set(h.freq, static_cast<float>(xToFreq(x)));
        if (h.gain >= 0) set(h.gain, static_cast<float>(yToGain(y)));
        return;
    }
    if (dragParam_ < 0) return;
    const fx::ParamInfo& p = fx_.info(dragParam_);
    const float delta = (x - dragStartX_) / dragBar_ * (fine ? 0.1f : 1.0f);
    set(dragParam_, fx::fromPosition(p, std::clamp(dragStartPos_ + delta, 0.0f, 1.0f)));
}

void EffectView::mouseUp() {
    dragParam_ = -1;
    dragHandle_ = -1;
}

void EffectView::doubleClick(float x, float y) {
    if (kind_ == Kind::Eq) {
        const int handle = handleAt(x, y);
        if (handle >= 0) {
            const Handle& h = handles_[static_cast<std::size_t>(handle)];
            if (h.gain >= 0) set(h.gain, 0.0f);
            if (h.q >= 0) set(h.q, fx_.info(h.q).def);
            if (h.gain < 0) set(handle == 0 ? EqEffect::kLowCutOn : EqEffect::kHighCutOn, 0.0f);  // Filter aus
            return;
        }
    }
    for (const Hit& hit : hits_) {
        if (!hit.rect.contains(x, y)) continue;
        if (hit.kind == HitKind::Slider) {
            dragParam_ = -1;
            set(hit.param, fx_.info(hit.param).def);
        } else {
            set(hit.param, fx_.param(hit.param) >= 0.5f ? 0.0f : 1.0f);  // zweiter Klick eines Doppelklicks
        }
        return;
    }
}

void EffectView::wheel(float x, float y, float notches, bool fine) {
    if (kind_ == Kind::Eq) {
        const int handle = handleAt(x, y);
        if (handle >= 0) {
            const Handle& h = handles_[static_cast<std::size_t>(handle)];
            if (h.q >= 0) set(h.q, fx_.param(h.q) * std::pow(fine ? 1.02f : 1.12f, notches));
            return;
        }
    }
    for (const Hit& hit : hits_) {
        if (!hit.rect.contains(x, y) || hit.kind != HitKind::Slider) continue;
        const fx::ParamInfo& p = fx_.info(hit.param);
        float step = fine ? 0.002f : 0.01f;
        if (p.choiceCount > 1) step = 1.0f / static_cast<float>(p.choiceCount - 1);
        set(hit.param, fx::fromPosition(p, std::clamp(fx::toPosition(p, fx_.param(hit.param)) + notches * step, 0.0f, 1.0f)));
        return;
    }
}

}  // namespace ui
