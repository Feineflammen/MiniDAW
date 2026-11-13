#include "ui/MixerView.h"
#include "ui/Theme.h"

#include <algorithm>
#include <cmath>
#include <iterator>
#include <string>

namespace ui {

using namespace theme;

namespace {

constexpr float kStripW = 58.0f;
constexpr float kStripGap = 2.0f;
constexpr float kGroupGap = 14.0f;  // Abstand vor den Bussen und vor dem Master
constexpr float kMargin = 8.0f;

constexpr float kNameY = 4.0f;
constexpr float kSlotY = 24.0f;
constexpr float kSlotStep = 18.0f;
constexpr float kSendY = 118.0f;
constexpr float kSendStep = 15.0f;
constexpr float kOutputY = 180.0f;
constexpr float kPanY = 202.0f;
constexpr float kFaderY = 222.0f;
constexpr float kFaderBottomGap = 46.0f;  // unter dem Fader: dB-Anzeige und M/S

// Markierungen neben dem Fader, in dB.
constexpr float kScaleMarks[] = {6.0f, 0.0f, -6.0f, -12.0f, -24.0f, -48.0f};

bool isTrack(int strip) { return strip >= 0 && strip < kProjectTracks; }
bool isBus(int strip) { return strip >= kFirstBusStrip && strip < kMasterStripIndex; }
bool isMaster(int strip) { return strip == kMasterStripIndex; }

float sliderPosition(const Rect& rect, float x) { return std::clamp((x - rect.x) / rect.w, 0.0f, 1.0f); }

float decay(float meter, float peak) {
    const float m = std::max(peak, meter * 0.82f);
    return m < 0.0001f ? 0.0f : m;
}

}  // namespace

MixerView::MixerView(ProjectController& controller, Engine& engine) : controller_(controller), engine_(engine) {}

// --- Geometrie ---------------------------------------------------------------

// Der Master steht immer rechts am Rand; Spuren und Busse rollen links davon.
float MixerView::stripX(int strip) const {
    if (strip == kMasterStripIndex) return area_.right() - kMargin - kStripW;
    float x = area_.x + kMargin + static_cast<float>(strip) * (kStripW + kStripGap) - scroll_;
    if (strip >= kFirstBusStrip) x += kGroupGap;
    return x;
}

// Breite aller Spuren und Busse (ohne Master).
float MixerView::contentWidth() const {
    return 2.0f * kMargin + static_cast<float>(kMasterStripIndex) * (kStripW + kStripGap) + kGroupGap;
}

// Sichtbare Breite fuer Spuren und Busse.
float MixerView::scrollWidth() const { return std::max(0.0f, area_.w - kStripW - kMargin - kGroupGap); }

Rect MixerView::slotRect(int strip, int slot) const {
    return {stripX(strip) + 3.0f, area_.y + kSlotY + static_cast<float>(slot) * kSlotStep, kStripW - 6.0f, 16.0f};
}

Rect MixerView::sendRect(int strip, int bus) const {
    return {stripX(strip) + 16.0f, area_.y + kSendY + static_cast<float>(bus) * kSendStep, kStripW - 24.0f, 11.0f};
}

Rect MixerView::outputRect(int strip) const { return {stripX(strip) + 3.0f, area_.y + kOutputY, kStripW - 6.0f, 16.0f}; }

Rect MixerView::panRect(int strip) const { return {stripX(strip) + 8.0f, area_.y + kPanY, kStripW - 16.0f, 12.0f}; }

Rect MixerView::faderRect(int strip) const {
    const float top = area_.y + kFaderY;
    return {stripX(strip) + 12.0f, top, 20.0f, std::max(40.0f, area_.bottom() - kFaderBottomGap - top)};
}

Rect MixerView::meterRect(int strip) const {
    const Rect f = faderRect(strip);
    return {stripX(strip) + 40.0f, f.y, 8.0f, f.h};
}

Rect MixerView::muteRect(int strip) const {
    return {stripX(strip) + 4.0f, area_.bottom() - 24.0f, isTrack(strip) ? 24.0f : kStripW - 8.0f, 18.0f};
}

Rect MixerView::soloRect(int strip) const { return {stripX(strip) + 30.0f, area_.bottom() - 24.0f, 24.0f, 18.0f}; }

MixerView::Hit MixerView::hitTest(float x, float y) const {
    if (!area_.contains(x, y)) return {};
    for (int s = 0; s < kProjectStrips; ++s) {
        const float sx = stripX(s);
        if (x < sx || x >= sx + kStripW) continue;
        if (!isMaster(s) && x >= area_.x + scrollWidth()) continue;
        for (int slot = isTrack(s) ? 0 : 1; slot < kSlotsPerStrip; ++slot)
            if (slotRect(s, slot).contains(x, y)) return {Part::Slot, s, slot};
        if (isTrack(s)) {
            for (int b = 0; b < kProjectBuses; ++b) {
                const Rect r = sendRect(s, b);
                if (y >= r.y - 2.0f && y < r.bottom() + 2.0f && x >= sx + 2.0f) return {Part::Send, s, b};
            }
            if (outputRect(s).contains(x, y)) return {Part::Output, s, 0};
        }
        if (!isMaster(s)) {
            const Rect p = panRect(s);
            if (y >= p.y - 3.0f && y < p.bottom() + 3.0f) return {Part::Pan, s, 0};
        }
        const Rect f = faderRect(s);
        if (y >= f.y - 6.0f && y < f.bottom() + 6.0f) return {Part::Fader, s, 0};
        if (!isMaster(s) && muteRect(s).contains(x, y)) return {Part::Mute, s, 0};
        if (isTrack(s) && soloRect(s).contains(x, y)) return {Part::Solo, s, 0};
        return {Part::None, s, 0};
    }
    return {};
}

// --- Werte der Kanalzuege ----------------------------------------------------

float MixerView::stripGain(int strip) const {
    const Project& p = controller_.project();
    if (isTrack(strip)) return p.tracks[static_cast<std::size_t>(strip)].gain;
    if (isBus(strip)) return p.buses[static_cast<std::size_t>(strip - kFirstBusStrip)].gain;
    return p.masterGain;
}

float MixerView::stripPan(int strip) const {
    const Project& p = controller_.project();
    if (isTrack(strip)) return p.tracks[static_cast<std::size_t>(strip)].pan;
    if (isBus(strip)) return p.buses[static_cast<std::size_t>(strip - kFirstBusStrip)].pan;
    return 0.0f;
}

bool MixerView::stripMute(int strip) const {
    const Project& p = controller_.project();
    if (isTrack(strip)) return p.tracks[static_cast<std::size_t>(strip)].mute;
    if (isBus(strip)) return p.buses[static_cast<std::size_t>(strip - kFirstBusStrip)].mute;
    return false;
}

float MixerView::stripMeter(int strip) const {
    if (isTrack(strip)) return trackMeters_ ? (*trackMeters_)[static_cast<std::size_t>(strip)] : 0.0f;
    if (isBus(strip)) return busMeters_[static_cast<std::size_t>(strip - kFirstBusStrip)];
    return masterMeter_;
}

void MixerView::setGain(int strip, float gain) {
    if (isTrack(strip)) controller_.setTrackGain(strip, gain);
    else if (isBus(strip)) controller_.setBusGain(strip - kFirstBusStrip, gain);
    else if (isMaster(strip)) controller_.setMasterGain(gain);
}

void MixerView::setPan(int strip, float pan) {
    if (std::fabs(pan) < 0.06f) pan = 0.0f;  // Mitte rastet ein
    if (isTrack(strip)) controller_.setTrackPan(strip, pan);
    else if (isBus(strip)) controller_.setBusPan(strip - kFirstBusStrip, pan);
}

// Ausgang weiterschalten: Master, Bus 1 bis 4.
void MixerView::stepOutput(int track, int delta) {
    const int current = controller_.project().tracks[static_cast<std::size_t>(track)].output;
    const int count = kProjectBuses + 1;  // -1 .. 3
    controller_.setTrackOutput(track, ((current + 1 + delta) % count + count) % count - 1);
}

// --- Laufender Betrieb und Zeichnen ------------------------------------------

void MixerView::tick() {
    for (int b = 0; b < kProjectBuses; ++b) {
        auto& m = busMeters_[static_cast<std::size_t>(b)];
        m = decay(m, engine_.takeBusPeak(b));
    }
}

void MixerView::draw(Canvas& c, const Rect& area) {
    area_ = area;
    const float visible = scrollWidth();
    const float total = contentWidth();
    scroll_ = std::clamp(scroll_, 0.0f, std::max(0.0f, total - visible));
    c.fill(area, kBg);
    c.pushClip({area.x, area.y, visible, area.h});
    for (int s = 0; s < kMasterStripIndex; ++s) {
        const float sx = stripX(s);
        if (sx > area.x + visible || sx + kStripW < area.x) continue;
        drawStrip(c, s);
    }
    c.popClip();
    c.pushClip(area);
    drawStrip(c, kMasterStripIndex);
    c.popClip();

    // Rollbalken, wenn nicht alle Kanalzuege hineinpassen.
    if (total > visible + 0.5f && visible > 0.0f) {
        const float barW = std::max(40.0f, visible * visible / total);
        const float barX = area.x + (visible - barW) * scroll_ / (total - visible);
        c.fillRounded({barX, area.bottom() - 4.0f, barW, 3.0f}, 1.5f, kTextDim);
    }
}

void MixerView::drawStrip(Canvas& c, int s) {
    const Project& project = controller_.project();
    const float sx = stripX(s);
    const float top = area_.y;
    const Rect body{sx, top + 1.0f, kStripW, area_.h - 2.0f};
    c.fillRounded(body, 4.0f, kPanel);

    // Name mit Farbstreifen
    std::wstring name;
    Color stripe = kMasterColor;
    if (isTrack(s)) {
        name = project.tracks[static_cast<std::size_t>(s)].name;
        stripe = rgb(kClipColors[static_cast<std::size_t>(s) % std::size(kClipColors)]);
    } else if (isBus(s)) {
        name = project.buses[static_cast<std::size_t>(s - kFirstBusStrip)].name;
        stripe = kBusColor;
    } else {
        name = L"Master";
    }
    c.fill({sx + 4.0f, top + kNameY, kStripW - 8.0f, 2.0f}, stripe);
    c.text(name, {sx + 2.0f, top + kNameY + 2.0f, kStripW - 4.0f, 16.0f}, kText, Font::Small, Align::Center);

    // Instrument und Effekte
    for (int slot = isTrack(s) ? 0 : 1; slot < kSlotsPerStrip; ++slot) {
        const Rect r = slotRect(s, slot);
        const SlotModel* model = controller_.slot(s, slot);
        const bool used = model && model->used();
        const bool active = activeStrip_ == s && activeSlot_ == slot;
        const Color background = active ? kAccent : !used ? kDark : model->instrument ? rgb(0x34506F) : kButton;
        c.fillRounded(r, 3.0f, background);
        std::wstring label;
        if (used) label = model->name;
        else if (slot == 0) label = L"Instrument";
        else label = L"FX " + std::to_wstring(slot);
        c.text(label, {r.x + 3.0f, r.y, r.w - 6.0f, r.h}, active ? kDark : used ? kText : rgb(0x5D6068), Font::Small,
               used ? Align::Left : Align::Center);
    }

    // Sends (nur Spuren): Pegel hinter dem Fader zu Bus 1 bis 4.
    if (isTrack(s)) {
        const TrackModel& track = project.tracks[static_cast<std::size_t>(s)];
        for (int b = 0; b < kProjectBuses; ++b) {
            const Rect r = sendRect(s, b);
            const float level = track.sends[static_cast<std::size_t>(b)];
            c.text(std::to_wstring(b + 1), {sx + 3.0f, r.y - 2.0f, 10.0f, r.h + 4.0f}, level > 0.0f ? kBusColor : kTextDim,
                   Font::Small, Align::Center);
            drawSlider(c, r, gainToPosition(level), 0.0f);
        }
        const Rect out = outputRect(s);
        c.fillRounded(out, 3.0f, kDark);
        const std::wstring outText =
            track.output < 0 ? std::wstring(L"→ Master") : L"→ Bus " + std::to_wstring(track.output + 1);
        c.text(outText, out, track.output < 0 ? kTextDim : kBusColor, Font::Small, Align::Center);
    } else if (isBus(s)) {
        c.text(L"→ Master", outputRect(s), kTextDim, Font::Small, Align::Center);
    }

    if (!isMaster(s)) drawSlider(c, panRect(s), (stripPan(s) + 1.0f) * 0.5f, 0.5f);

    // Fader mit Skala und Pegelanzeige
    const Rect f = faderRect(s);
    const float gain = stripGain(s);
    const float centre = f.x + f.w * 0.5f;
    for (const float db : kScaleMarks) {
        const float y = f.bottom() - f.h * (db - kMinDb) / (kMaxDb - kMinDb);
        c.line(f.x - 8.0f, y + 0.5f, f.x - 2.0f, y + 0.5f, db == 0.0f ? kTextDim : kLine);
    }
    c.fillRounded({centre - 2.0f, f.y, 4.0f, f.h}, 2.0f, kDark);
    const float thumbY = f.bottom() - f.h * gainToPosition(gain);
    c.fillRounded({centre - 2.0f, thumbY, 4.0f, f.bottom() - thumbY}, 2.0f, rgb(0x3D6EAF));
    c.fillRounded({f.x, thumbY - 5.0f, f.w, 10.0f}, 2.0f, kText);
    c.line(f.x + 3.0f, thumbY + 0.5f, f.right() - 3.0f, thumbY + 0.5f, kDark);

    const Rect m = meterRect(s);
    c.fill(m, kDark);
    const float peak = stripMeter(s);
    const float level = gainToPosition(peak);
    if (level > 0.0f) {
        const float h = m.h * level;
        c.fill({m.x, m.bottom() - h, m.w, h}, peak >= 1.0f ? kMeterHot : kMeter);
    }

    c.text(formatDb(gain), {sx, area_.bottom() - 44.0f, kStripW, 16.0f}, kTextDim, Font::Small, Align::Center);

    if (!isMaster(s)) {
        const bool mute = stripMute(s);
        const Rect mr = muteRect(s);
        c.fillRounded(mr, 3.0f, mute ? kMuteOn : kButton);
        c.text(L"M", mr, mute ? kDark : kTextDim, Font::Small, Align::Center);
    }
    if (isTrack(s)) {
        const bool solo = project.tracks[static_cast<std::size_t>(s)].solo;
        const Rect sr = soloRect(s);
        c.fillRounded(sr, 3.0f, solo ? kSoloOn : kButton);
        c.text(L"S", sr, solo ? kDark : kTextDim, Font::Small, Align::Center);
    }
}

// --- Bedienung ---------------------------------------------------------------

bool MixerView::mouseDown(float x, float y, bool doubleClick) {
    if (!area_.contains(x, y)) return false;
    const Hit hit = hitTest(x, y);
    const int s = hit.strip;
    switch (hit.part) {
        case Part::Slot: {
            const SlotModel* model = controller_.slot(s, hit.index);
            if (model && model->used()) {
                if (onOpenEditor) onOpenEditor(s, hit.index);
            } else if (onPickPlugin) {
                onPickPlugin(s, hit.index, slotRect(s, hit.index));
            }
            break;
        }
        case Part::Send: {
            const float level = controller_.project().tracks[static_cast<std::size_t>(s)].sends[static_cast<std::size_t>(hit.index)];
            if (doubleClick) {
                controller_.setTrackSend(s, hit.index, level > 0.0f ? 0.0f : 1.0f);
            } else {
                drag_ = Drag::Send;
                dragStrip_ = s;
                dragBus_ = hit.index;
                mouseMove(x, y, false);
            }
            break;
        }
        case Part::Output:
            stepOutput(s, 1);
            break;
        case Part::Pan:
            if (doubleClick) {
                setPan(s, 0.0f);
            } else {
                drag_ = Drag::Pan;
                dragStrip_ = s;
                mouseMove(x, y, false);
            }
            break;
        case Part::Fader:
            if (doubleClick) {
                setGain(s, 1.0f);
            } else {
                // Der Fader springt nicht an die Maus, er folgt ihrer Bewegung.
                drag_ = Drag::Fader;
                dragStrip_ = s;
                dragStartPos_ = gainToPosition(stripGain(s));
                dragStartY_ = dragLastY_ = y;
            }
            break;
        case Part::Mute:
            if (isTrack(s)) controller_.setTrackMute(s, !stripMute(s));
            else if (isBus(s)) controller_.setBusMute(s - kFirstBusStrip, !stripMute(s));
            break;
        case Part::Solo:
            controller_.setTrackSolo(s, !controller_.project().tracks[static_cast<std::size_t>(s)].solo);
            break;
        case Part::None:
            break;
    }
    return true;
}

bool MixerView::rightMouseDown(float x, float y) {
    if (!area_.contains(x, y)) return false;
    const Hit hit = hitTest(x, y);
    if (hit.part == Part::Slot && onPickPlugin) onPickPlugin(hit.strip, hit.index, slotRect(hit.strip, hit.index));
    else if (hit.part == Part::Output) stepOutput(hit.strip, -1);
    return true;
}

void MixerView::mouseMove(float x, float y, bool fine) {
    switch (drag_) {
        case Drag::Fader: {
            const float height = faderRect(dragStrip_).h;
            // Mit Umschalt fein: ein Fuenftel der Bewegung.
            dragStartPos_ -= (y - dragLastY_) / height * (fine ? 0.2f : 1.0f);
            dragStartPos_ = std::clamp(dragStartPos_, 0.0f, 1.0f);
            dragLastY_ = y;
            setGain(dragStrip_, positionToGain(dragStartPos_));
            break;
        }
        case Drag::Pan:
            setPan(dragStrip_, sliderPosition(panRect(dragStrip_), x) * 2.0f - 1.0f);
            break;
        case Drag::Send:
            controller_.setTrackSend(dragStrip_, dragBus_, positionToGain(sliderPosition(sendRect(dragStrip_, dragBus_), x)));
            break;
        case Drag::None:
            break;
    }
}

void MixerView::mouseUp() {
    drag_ = Drag::None;
    dragStrip_ = -1;
}

void MixerView::wheel(float x, float y, float notches) {
    const Hit hit = hitTest(x, y);
    const float step = notches > 0.0f ? 1.0f : -1.0f;
    const float oneDb = 1.0f / (kMaxDb - kMinDb);
    switch (hit.part) {
        case Part::Fader:
            setGain(hit.strip, positionToGain(std::clamp(gainToPosition(stripGain(hit.strip)) + step * oneDb, 0.0f, 1.0f)));
            return;
        case Part::Pan:
            setPan(hit.strip, std::clamp(stripPan(hit.strip) + step * 0.05f, -1.0f, 1.0f));
            return;
        case Part::Send: {
            const float level = controller_.project().tracks[static_cast<std::size_t>(hit.strip)].sends[static_cast<std::size_t>(hit.index)];
            controller_.setTrackSend(hit.strip, hit.index,
                                     positionToGain(std::clamp(gainToPosition(level) + step * oneDb, 0.0f, 1.0f)));
            return;
        }
        case Part::Output:
            stepOutput(hit.strip, notches > 0.0f ? 1 : -1);
            return;
        default:
            // Sonst rollt das Mausrad die Kanalzuege seitwaerts.
            scroll_ = std::clamp(scroll_ - notches * 120.0f, 0.0f, std::max(0.0f, contentWidth() - scrollWidth()));
            return;
    }
}

}  // namespace ui
