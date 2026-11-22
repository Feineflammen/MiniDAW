#include "ui/MasteringView.h"

#include "ui/Theme.h"

#include <algorithm>
#include <cmath>
#include <cwchar>

namespace ui {

using namespace theme;
using namespace mastering;

namespace {

constexpr float kMargin = 12.0f;
constexpr float kHeaderH = 50.0f;
constexpr float kMeterW = 250.0f;
constexpr float kSliderRowH = 46.0f;

// Zahl mit Komma statt Punkt und echtem Minuszeichen.
std::wstring number(double value, int decimals) {
    wchar_t buffer[48];
    std::swprintf(buffer, 48, L"%.*f", decimals, value);
    std::wstring text = buffer;
    for (auto& ch : text)
        if (ch == L'.') ch = L',';
    if (!text.empty() && text[0] == L'-') {
        bool zero = true;  // "-0,0" als "0,0" zeigen
        for (wchar_t ch : text)
            if (ch >= L'1' && ch <= L'9') zero = false;
        text = zero ? text.substr(1) : L"−" + text.substr(1);
    }
    return text;
}

std::wstring valueText(int param, float value) {
    const ParamInfo& p = kParams[static_cast<std::size_t>(param)];
    if (p.toggle) return value >= 0.5f ? L"an" : L"aus";
    std::wstring unit = p.unit;
    if (param == kRatio) return number(value, 1) + L" : 1";
    int decimals = 1;
    if (unit == L"Hz" || unit == L"%" || unit == L"ms") decimals = 0;
    std::wstring text = number(value, decimals);
    if ((unit == L"dB") && value > 0.04f) text = L"+" + text;
    return text + L" " + unit;
}

// Mehrere Zeilen untereinander (die Canvas bricht nicht selbst um).
void lines(Canvas& c, std::initializer_list<const wchar_t*> rows, float x, float y, float w, Color color) {
    for (const wchar_t* row : rows) {
        c.text(row, {x, y, w, 16.0f}, color, Font::Small);
        y += 15.0f;
    }
}

std::wstring lufsText(double value) { return value <= -100.0 ? std::wstring(L"—") : number(value, 1); }

// Auf der Skala -36..0 LUFS.
float lufsPosition(double value) { return static_cast<float>(std::clamp((value + 36.0) / 36.0, 0.0, 1.0)); }

}  // namespace

const MasteringView::Hit* MasteringView::hitAt(float x, float y) const {
    for (const Hit& hit : hits_)
        if (hit.rect.contains(x, y)) return &hit;
    return nullptr;
}

void MasteringView::set(int param, float value) {
    const float before = fx_.param(param);
    fx_.setParam(param, value);
    if (fx_.param(param) != before && onChange) onChange();
}

void MasteringView::drawSlider(Canvas& c, const Rect& row, int param, bool enabled) {
    const ParamInfo& p = kParams[static_cast<std::size_t>(param)];
    const float value = fx_.param(param);
    const Color dim = enabled ? kTextDim : rgb(0x5C5F66);
    const Color bright = enabled ? kText : rgb(0x6C6F76);
    c.text(p.label, {row.x, row.y, row.w, 18.0f}, dim, Font::Small);
    c.text(valueText(param, value), {row.x, row.y, row.w, 18.0f}, bright, Font::Small, Align::Right);
    const Rect bar{row.x + 2.0f, row.y + 22.0f, row.w - 4.0f, 14.0f};
    // Gefuellt ab 0 dB bzw. ab dem linken Rand
    float zero = 0.0f;
    if (p.min < 0.0f && p.max > 0.0f) zero = toPosition(param, 0.0f);
    if (param == kCeiling) zero = 1.0f;
    theme::drawSlider(c, bar, toPosition(param, value), zero);
    if (!enabled) c.fill({bar.x - 4.0f, bar.y, bar.w + 8.0f, bar.h}, rgb(0x2A2C31, 0.55f));
    hits_.push_back({{row.x, row.y, row.w, kSliderRowH - 4.0f}, HitKind::Slider, param, bar.w});
}

void MasteringView::drawSection(Canvas& c, const Rect& box, const wchar_t* title, int toggle,
                                std::initializer_list<int> sliders, int extraToggle) {
    c.fillRounded(box, 6.0f, kPanel);
    const bool enabled = toggle < 0 || fx_.param(toggle) >= 0.5f;
    float x = box.x + 12.0f;
    if (toggle >= 0) {
        const Rect pill{x, box.y + 12.0f, 30.0f, 16.0f};
        c.fillRounded(pill, 8.0f, enabled ? kAccent : kButton);
        c.fillRounded({enabled ? pill.right() - 14.0f : pill.x + 2.0f, pill.y + 2.0f, 12.0f, 12.0f}, 6.0f,
                      enabled ? kText : kTextDim);
        hits_.push_back({{box.x, box.y, box.w, 38.0f}, HitKind::Toggle, toggle, 0.0f});
        x = pill.right() + 10.0f;
    }
    c.text(title, {x, box.y + 8.0f, box.right() - x - 8.0f, 24.0f}, enabled ? kText : kTextDim);

    float y = box.y + 44.0f;
    for (int param : sliders) {
        drawSlider(c, {box.x + 12.0f, y, box.w - 24.0f, kSliderRowH}, param, enabled);
        y += kSliderRowH;
    }
    if (extraToggle >= 0) {
        const bool on = fx_.param(extraToggle) >= 0.5f;
        const Rect check{box.x + 12.0f, y + 4.0f, 14.0f, 14.0f};
        c.fillRounded(check, 3.0f, on && enabled ? kAccent : kDark);
        c.strokeRounded(check, 3.0f, kLine);
        if (on) c.text(L"✓", {check.x, check.y - 3.0f, check.w, 18.0f}, kText, Font::Small, Align::Center);
        const Rect text{check.right() + 8.0f, y + 1.0f, box.right() - 12.0f - check.right() - 8.0f, 20.0f};
        c.text(L"Bass mono", text, enabled ? kText : rgb(0x6C6F76), Font::Small);
        c.text(L"Stereo unter 120 Hz wird mono", {box.x + 12.0f, y + 22.0f, box.w - 24.0f, 18.0f},
               enabled ? kTextDim : rgb(0x5C5F66), Font::Small);
        hits_.push_back({{box.x + 8.0f, y, box.w - 16.0f, 22.0f}, HitKind::Toggle, extraToggle, 0.0f});
    }
}

void MasteringView::drawMeters(Canvas& c, const Rect& panel) {
    const MasteringEffect::Meters m = fx_.meters();
    c.fillRounded(panel, 6.0f, kPanel);
    const float x = panel.x + 14.0f, w = panel.w - 28.0f;
    float y = panel.y + 10.0f;

    c.text(L"Lautheit integriert", {x, y, w, 20.0f}, kTextDim, Font::Small);
    y += 18.0f;
    c.text(lufsText(m.integrated), {x, y, w - 50.0f, 34.0f}, kText, Font::Large);
    c.text(L"LUFS", {x, y + 6.0f, w, 24.0f}, kTextDim, Font::Normal, Align::Right);
    y += 40.0f;

    // Momentan und Kurzzeit als Balken, Markierung bei -14 LUFS (ueblich beim Streaming)
    const auto bar = [&](const wchar_t* label, double value) {
        c.text(label, {x, y, w, 18.0f}, kTextDim, Font::Small);
        c.text(lufsText(value), {x, y, w, 18.0f}, kText, Font::Small, Align::Right);
        const Rect track{x, y + 20.0f, w, 8.0f};
        c.fillRounded(track, 3.0f, kDark);
        if (value > -100.0)
            c.fillRounded({track.x, track.y, track.w * lufsPosition(value), track.h}, 3.0f,
                          value > -9.0 ? kMuteOn : kMeter);
        const float mark = track.x + track.w * lufsPosition(-14.0);
        c.line(mark, track.y - 2.0f, mark, track.bottom() + 2.0f, kText);
        y += 38.0f;
    };
    bar(L"Kurzzeit (3 s)", m.shortTerm);
    bar(L"Momentan (0,4 s)", m.momentary);

    c.line(x, y + 2.0f, x + w, y + 2.0f, kLine);
    y += 10.0f;

    // True Peak: rot, wenn ueber der eingestellten Grenze
    const double ceiling = fx_.param(kCeiling);
    const bool over = m.truePeakDb > ceiling + 0.1;
    c.text(L"True Peak (höchster)", {x, y, w, 18.0f}, kTextDim, Font::Small);
    c.text(m.truePeakDb <= -100.0 ? std::wstring(L"—") : number(m.truePeakDb, 1) + L" dBTP", {x, y, w, 18.0f},
           over ? kMeterHot : kText, Font::Small, Align::Right);
    y += 26.0f;

    // Absenkung des Limiters, 0..12 dB
    const double gr = std::max(0.0, m.reductionDb);
    c.text(L"Limiter senkt ab", {x, y, w, 18.0f}, kTextDim, Font::Small);
    c.text(number(gr, 1) + L" dB", {x, y, w, 18.0f}, kText, Font::Small, Align::Right);
    const Rect grTrack{x, y + 20.0f, w, 8.0f};
    c.fillRounded(grTrack, 3.0f, kDark);
    const float grW = grTrack.w * static_cast<float>(std::clamp(gr / 12.0, 0.0, 1.0));
    if (grW > 0.5f) c.fillRounded({grTrack.right() - grW, grTrack.y, grW, grTrack.h}, 3.0f, kMuteOn);
    y += 38.0f;

    c.text(L"Eingang integriert", {x, y, w, 18.0f}, kTextDim, Font::Small);
    c.text(lufsText(m.inputIntegrated) + (m.inputIntegrated > -100.0 ? L" LUFS" : L""), {x, y, w, 18.0f}, kText,
           Font::Small, Align::Right);
    y += 20.0f;
    if (m.inputIntegrated > -100.0 && m.integrated > -100.0) {
        const double gain = m.integrated - m.inputIntegrated;
        c.text(L"Unterschied", {x, y, w, 18.0f}, kTextDim, Font::Small);
        c.text((gain > 0.04 ? L"+" : L"") + number(gain, 1) + L" LU", {x, y, w, 18.0f}, kText, Font::Small, Align::Right);
    }
    y += 30.0f;

    const Rect reset{x, y, w, 26.0f};
    c.fillRounded(reset, 4.0f, kButton);
    c.text(L"Messung neu starten", reset, kText, Font::Small, Align::Center);
    hits_.push_back({reset, HitKind::ResetMeters, -1, 0.0f});
    y += 36.0f;

    lines(c,
          {L"Richtwert für Streaming: um −14 LUFS", L"integriert, True Peak höchstens −1 dBTP.",
           L"Die Messung beginnt bei jedem Start", L"der Wiedergabe und beim Export neu."},
          x, y, w, kTextDim);
}

void MasteringView::draw(Canvas& c, float width, float height) {
    hits_.clear();
    c.clear(kBg);

    // Kopf: Titel, Latenz, Umgehen
    c.text(L"Mastering", {kMargin + 4.0f, 12.0f, 140.0f, 26.0f}, kText);
    const double latencyMs = 1000.0 * fx_.latencyFrames() / std::max(1.0, fx_.sampleRate());
    c.text(L"Eingebauter Effekt · Latenz " + number(latencyMs, 1) + L" ms (wird ausgeglichen)",
           {kMargin + 90.0f, 14.0f, 420.0f, 22.0f}, kTextDim, Font::Small);
    const bool bypass = fx_.param(kBypass) >= 0.5f;
    const Rect bypassRect{width - kMargin - 130.0f, 12.0f, 130.0f, 28.0f};
    c.fillRounded(bypassRect, 4.0f, bypass ? kMuteOn : kButton);
    c.text(bypass ? L"Umgangen" : L"Umgehen", bypassRect, bypass ? kDark : kText, Font::Normal, Align::Center);
    hits_.push_back({bypassRect, HitKind::Toggle, kBypass, 0.0f});

    const Rect meters{width - kMargin - kMeterW, kHeaderH + 8.0f, kMeterW, height - kHeaderH - 8.0f - kMargin};
    drawMeters(c, meters);

    // Stufen in Signalreihenfolge, zwei Reihen zu je vier
    const float areaX = kMargin, areaY = kHeaderH + 8.0f;
    const float areaW = meters.x - kMargin - areaX, areaH = meters.h;
    constexpr float gap = 8.0f;
    const float boxW = (areaW - 3.0f * gap) / 4.0f, boxH = (areaH - gap) / 2.0f;
    const auto box = [&](int col, int row) {
        return Rect{areaX + static_cast<float>(col) * (boxW + gap), areaY + static_cast<float>(row) * (boxH + gap), boxW, boxH};
    };
    drawSection(c, box(0, 0), L"Eingang", -1, {kInput});
    drawSection(c, box(1, 0), L"Korrektur", kEqOn, {kLowCut, kMud});
    drawSection(c, box(2, 0), L"Kompressor", kCompOn, {kThreshold, kRatio});
    drawSection(c, box(3, 0), L"Sättigung", kSatOn, {kDrive, kSatMix});
    drawSection(c, box(0, 1), L"Klang", kToneOn, {kBass, kAir});
    drawSection(c, box(1, 1), L"Stereo", kStereoOn, {kWidth}, kMonoBass);
    drawSection(c, box(2, 1), L"Clipper", kClipOn, {kClipDrive});
    drawSection(c, box(3, 1), L"Limiter", kLimitOn, {kLoudness, kCeiling, kRelease});

    // Signalweg-Hinweis unten im Eingang-Feld
    const Rect in = box(0, 0);
    lines(c,
          {L"Signalweg:", L"Eingang → Korrektur", L"→ Kompressor", L"→ Sättigung → Klang", L"→ Stereo → Lautheit",
           L"→ Clipper → Limiter"},
          in.x + 12.0f, in.y + 44.0f + kSliderRowH + 8.0f, in.w - 24.0f, kTextDim);
}

void MasteringView::mouseDown(float x, float y) {
    const Hit* hit = hitAt(x, y);
    if (!hit) return;
    switch (hit->kind) {
        case HitKind::Toggle:
            set(hit->param, fx_.param(hit->param) >= 0.5f ? 0.0f : 1.0f);
            break;
        case HitKind::ResetMeters:
            fx_.resetMeters();
            break;
        case HitKind::Slider:
            // Ziehen veraendert den Wert relativ; ein Klick allein laesst ihn stehen.
            dragParam_ = hit->param;
            dragStartX_ = x;
            dragStartPos_ = toPosition(hit->param, fx_.param(hit->param));
            dragBar_ = std::max(40.0f, hit->barWidth);
            break;
    }
}

void MasteringView::mouseMove(float x, float, bool fine) {
    if (dragParam_ < 0) return;
    const float delta = (x - dragStartX_) / dragBar_ * (fine ? 0.1f : 1.0f);
    set(dragParam_, fromPosition(dragParam_, std::clamp(dragStartPos_ + delta, 0.0f, 1.0f)));
}

void MasteringView::mouseUp() { dragParam_ = -1; }

void MasteringView::doubleClick(float x, float y) {
    const Hit* hit = hitAt(x, y);
    if (!hit || hit->kind != HitKind::Slider) {
        mouseDown(x, y);  // Doppelklick auf einen Schalter schaltet ein zweites Mal
        return;
    }
    dragParam_ = -1;
    set(hit->param, kParams[static_cast<std::size_t>(hit->param)].def);
}

void MasteringView::wheel(float x, float y, float notches, bool fine) {
    const Hit* hit = hitAt(x, y);
    if (!hit || hit->kind != HitKind::Slider) return;
    const float position = toPosition(hit->param, fx_.param(hit->param)) + notches * (fine ? 0.002f : 0.01f);
    set(hit->param, fromPosition(hit->param, std::clamp(position, 0.0f, 1.0f)));
}

}  // namespace ui
