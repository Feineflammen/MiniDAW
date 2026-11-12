#include "ui/PianoRoll.h"

#include <algorithm>
#include <cmath>
#include <string>

namespace ui {

namespace {

constexpr float kHeaderH = 24.0f;
constexpr float kKeysW = 52.0f;
constexpr float kVelocityH = 46.0f;
constexpr float kRowH = 10.0f;

constexpr Color kPanel = rgb(0x24262B);
constexpr Color kHeader = rgb(0x2E3037);
constexpr Color kLine = rgb(0x3A3C43);
constexpr Color kRowLight = rgb(0x2B2D33);
constexpr Color kRowDark = rgb(0x25272C);
constexpr Color kOutside = rgb(0x1B1C1F);
constexpr Color kGridFine = rgb(0x33353C);
constexpr Color kGridBeat = rgb(0x42454E);
constexpr Color kGridBar = rgb(0x5A5E69);
constexpr Color kText = rgb(0xDADCE2);
constexpr Color kTextDim = rgb(0x8D9099);
constexpr Color kButton = rgb(0x3A3D45);
constexpr Color kNote = rgb(0x4C9AFF);
constexpr Color kWhiteKey = rgb(0xD9DBE0);
constexpr Color kBlackKey = rgb(0x2A2B30);
constexpr Color kPlayhead = rgb(0xFF5A5A);

bool isBlackKey(int pitch) {
    const int n = ((pitch % 12) + 12) % 12;
    return n == 1 || n == 3 || n == 6 || n == 8 || n == 10;
}

}  // namespace

void PianoRoll::open(int track, int index) {
    track_ = track;
    index_ = index;
    selected_ = -1;
    drag_ = Drag::None;
    editing_ = false;
    fitPending_ = true;
    if (!clip()) close();
}

void PianoRoll::close() {
    track_ = index_ = -1;
    selected_ = -1;
    drag_ = Drag::None;
    editing_ = false;
}

const MidiClipModel* PianoRoll::clip() const {
    if (track_ < 0 || track_ >= kProjectTracks) return nullptr;
    const auto& clips = controller_.project().tracks[static_cast<std::size_t>(track_)].midiClips;
    if (index_ < 0 || index_ >= static_cast<int>(clips.size())) return nullptr;
    return &clips[static_cast<std::size_t>(index_)];
}

const std::vector<MidiNote>& PianoRoll::notes() const {
    static const std::vector<MidiNote> kEmpty;
    if (editing_) return edit_;
    const MidiClipModel* c = clip();
    return c ? c->notes : kEmpty;
}

// Rasterweite in Frames: eine ganze Note hat vier Schlaege.
std::int64_t PianoRoll::gridStep() const {
    return std::max<std::int64_t>(1, static_cast<std::int64_t>(controller_.framesPerBeat() * 4.0 / division_));
}

// Rastet eine Position (relativ zum Clip) am Raster der Zeitachse ein.
std::int64_t PianoRoll::snap(std::int64_t frame, bool floor) const {
    const MidiClipModel* c = clip();
    if (!c) return frame;
    const double step = controller_.framesPerBeat() * 4.0 / division_;
    const double absolute = static_cast<double>(c->start + frame) / step;
    const double snapped = (floor ? std::floor(absolute + 1e-9) : std::round(absolute)) * step;
    return static_cast<std::int64_t>(std::llround(snapped)) - c->start;
}

float PianoRoll::xOf(double frame) const {
    return grid_.x + static_cast<float>((frame - viewStart_) / framesPerPixel_);
}

double PianoRoll::frameAt(float x) const {
    return viewStart_ + static_cast<double>(x - grid_.x) * framesPerPixel_;
}

float PianoRoll::yOf(int pitch) const { return grid_.y + static_cast<float>(topPitch_ - pitch) * kRowH; }

int PianoRoll::pitchAt(float y) const {
    return topPitch_ - static_cast<int>(std::floor((y - grid_.y) / kRowH));
}

int PianoRoll::noteAt(float x, float y) const {
    const auto& list = notes();
    const int pitch = pitchAt(y);
    const double frame = frameAt(x);
    for (int i = static_cast<int>(list.size()) - 1; i >= 0; --i) {
        const MidiNote& n = list[static_cast<std::size_t>(i)];
        if (n.pitch == pitch && frame >= static_cast<double>(n.start) &&
            frame < static_cast<double>(n.start + n.length))
            return i;
    }
    return -1;
}

void PianoRoll::beginEdit() {
    const MidiClipModel* c = clip();
    if (!c) return;
    edit_ = c->notes;
    editing_ = true;
    changed_ = false;
}

// Uebergibt die bearbeiteten Noten an das Projekt. Ragt eine Note ueber das
// Clip-Ende hinaus, waechst der Clip bis zum naechsten Taktstrich.
void PianoRoll::commit() {
    const MidiClipModel* c = clip();
    if (!editing_ || !c) { editing_ = false; return; }
    editing_ = false;
    if (!changed_) return;

    // Die Auswahl ueber das Umsortieren hinweg behalten.
    MidiNote selectedNote{};
    const bool hadSelection = selected_ >= 0 && selected_ < static_cast<int>(edit_.size());
    if (hadSelection) selectedNote = edit_[static_cast<std::size_t>(selected_)];

    std::int64_t length = c->length;
    std::int64_t maxEnd = 0;
    for (const auto& n : edit_) maxEnd = std::max(maxEnd, n.start + n.length);
    if (maxEnd > length) {
        const double bar = controller_.framesPerBeat() * 4.0;
        const double endBar = std::ceil(static_cast<double>(c->start + maxEnd) / bar - 1e-9);
        length = static_cast<std::int64_t>(std::llround(endBar * bar)) - c->start;
    }
    controller_.updateMidiClip(track_, index_, edit_, length);

    selected_ = -1;
    if (hadSelection) {
        const MidiClipModel* updated = clip();
        for (int i = 0; updated && i < static_cast<int>(updated->notes.size()); ++i) {
            const MidiNote& n = updated->notes[static_cast<std::size_t>(i)];
            if (n.start == selectedNote.start && n.pitch == selectedNote.pitch && n.length == selectedNote.length) {
                selected_ = i;
                break;
            }
        }
    }
}

// --- Zeichnen ----------------------------------------------------------------

void PianoRoll::draw(Canvas& c, const Rect& panel) {
    const MidiClipModel* clipPtr = clip();
    if (!clipPtr) { close(); return; }
    const MidiClipModel& midi = *clipPtr;

    panel_ = panel;
    header_ = {panel.x, panel.y, panel.w, kHeaderH};
    keys_ = {panel.x, panel.y + kHeaderH, kKeysW, std::max(0.0f, panel.h - kHeaderH - kVelocityH)};
    grid_ = {panel.x + kKeysW, keys_.y, std::max(1.0f, panel.w - kKeysW), keys_.h};
    velocity_ = {grid_.x, grid_.bottom(), grid_.w, kVelocityH};
    closeRect_ = {panel.right() - 30.0f, panel.y + 3.0f, 22.0f, 18.0f};
    divisionRect_ = {panel.x + 300.0f, panel.y + 3.0f, 92.0f, 18.0f};

    const int rows = std::max(1, static_cast<int>(grid_.h / kRowH));
    if (fitPending_) {
        // Den ganzen Clip zeigen und senkrecht auf die vorhandenen Noten zielen.
        fitPending_ = false;
        framesPerPixel_ = std::max(1.0, static_cast<double>(midi.length) * 1.05 / static_cast<double>(grid_.w));
        viewStart_ = 0.0;
        int low = 127, high = 0;
        for (const auto& n : midi.notes) {
            low = std::min<int>(low, n.pitch);
            high = std::max<int>(high, n.pitch);
        }
        const int centre = low <= high ? (low + high) / 2 : 66;
        topPitch_ = centre + rows / 2;
    }
    topPitch_ = std::clamp(topPitch_, std::min(127, rows - 1), 127);

    c.fill(panel, kPanel);

    // Notenraster
    c.pushClip(grid_);
    for (int r = 0; r <= rows; ++r) {
        const int pitch = topPitch_ - r;
        if (pitch < 0) break;
        c.fill({grid_.x, yOf(pitch), grid_.w, kRowH}, isBlackKey(pitch) ? kRowDark : kRowLight);
        if (pitch % 12 == 0) c.line(grid_.x, yOf(pitch) + kRowH - 0.5f, grid_.right(), yOf(pitch) + kRowH - 0.5f, kGridBeat);
    }

    // Bereich ausserhalb des Clips abdunkeln.
    const float clipX0 = xOf(0.0);
    const float clipX1 = xOf(static_cast<double>(midi.length));
    if (clipX0 > grid_.x) c.fill({grid_.x, grid_.y, clipX0 - grid_.x, grid_.h}, rgb(0x1B1C1F, 0.75f));
    if (clipX1 < grid_.right()) c.fill({clipX1, grid_.y, grid_.right() - clipX1, grid_.h}, rgb(0x1B1C1F, 0.75f));

    // Senkrechte Linien: Raster, Schlaege, Takte (an der Zeitachse ausgerichtet).
    const double beat = controller_.framesPerBeat();
    double step = beat * 4.0 / division_;
    while (step / framesPerPixel_ < 6.0) step *= 2.0;  // zu dichte Linien auslassen
    const double firstAbs = std::floor((static_cast<double>(midi.start) + viewStart_) / step) * step;
    for (double abs = firstAbs;; abs += step) {
        const float x = xOf(abs - static_cast<double>(midi.start));
        if (x > grid_.right()) break;
        if (x < grid_.x) continue;
        const double beats = abs / beat;
        const bool onBeat = std::fabs(beats - std::round(beats)) < 1e-6;
        const bool onBar = onBeat && static_cast<std::int64_t>(std::llround(beats)) % 4 == 0;
        c.line(x + 0.5f, grid_.y, x + 0.5f, grid_.bottom(), onBar ? kGridBar : onBeat ? kGridBeat : kGridFine);
    }

    // Noten
    const auto& list = notes();
    for (int i = 0; i < static_cast<int>(list.size()); ++i) {
        const MidiNote& n = list[static_cast<std::size_t>(i)];
        const float x0 = xOf(static_cast<double>(n.start));
        const float x1 = xOf(static_cast<double>(n.start + n.length));
        if (x1 < grid_.x || x0 > grid_.right()) continue;
        const Rect r{x0, yOf(n.pitch) + 1.0f, std::max(3.0f, x1 - x0 - 1.0f), kRowH - 2.0f};
        const float strength = 0.45f + 0.55f * static_cast<float>(n.velocity) / 127.0f;
        c.fillRounded(r, 2.0f, rgb(0x4C9AFF, strength));
        if (i == selected_) c.strokeRounded(r, 2.0f, rgb(0xFFFFFF), 1.5f);
    }

    // Abspielzeiger
    const float playX = xOf(static_cast<double>(engine_.position() - midi.start));
    if (playX >= grid_.x && playX <= grid_.right())
        c.line(playX + 0.5f, grid_.y, playX + 0.5f, grid_.bottom(), kPlayhead);
    c.popClip();

    // Klaviatur
    c.pushClip(keys_);
    for (int r = 0; r <= rows; ++r) {
        const int pitch = topPitch_ - r;
        if (pitch < 0) break;
        const float y = yOf(pitch);
        c.fill({keys_.x, y, keys_.w, kRowH - 1.0f}, isBlackKey(pitch) ? kBlackKey : kWhiteKey);
        if (pitch % 12 == 0)
            c.text(L"C" + std::to_wstring(pitch / 12 - 1), {keys_.x + 4.0f, y - 1.0f, keys_.w - 6.0f, kRowH},
                   rgb(0x1B1C1F), Font::Small);
    }
    c.popClip();

    // Anschlagstaerken
    c.fill({panel.x, velocity_.y, panel.w, kVelocityH}, kOutside);
    c.line(panel.x, velocity_.y + 0.5f, panel.right(), velocity_.y + 0.5f, kLine);
    c.text(L"Anschlag", {panel.x + 4.0f, velocity_.y, kKeysW, kVelocityH}, kTextDim, Font::Small);
    c.pushClip(velocity_);
    for (int i = 0; i < static_cast<int>(list.size()); ++i) {
        const MidiNote& n = list[static_cast<std::size_t>(i)];
        const float x = xOf(static_cast<double>(n.start));
        if (x < velocity_.x - 4.0f || x > velocity_.right()) continue;
        const float h = (kVelocityH - 6.0f) * static_cast<float>(n.velocity) / 127.0f;
        c.fill({x, velocity_.bottom() - h, 4.0f, h}, i == selected_ ? rgb(0xFFFFFF) : kNote);
    }
    c.popClip();

    // Kopfzeile
    c.fill(header_, kHeader);
    c.line(panel.x, panel.y + 0.5f, panel.right(), panel.y + 0.5f, kLine);
    c.text(L"Noten: " + midi.name + L" (Spur " + std::to_wstring(track_ + 1) + L")",
           {panel.x + 10.0f, panel.y, 280.0f, kHeaderH}, kText);
    c.fillRounded(divisionRect_, 3.0f, kButton);
    c.text(L"Raster 1/" + std::to_wstring(division_), divisionRect_, kText, Font::Small, Align::Center);
    c.text(L"Klick: Note setzen   Ziehen: verschieben, am rechten Rand verlängern   Rechtsklick: löschen",
           {divisionRect_.right() + 16.0f, panel.y, std::max(0.0f, closeRect_.x - divisionRect_.right() - 24.0f), kHeaderH},
           kTextDim, Font::Small);
    c.fillRounded(closeRect_, 3.0f, kButton);
    c.text(L"×", closeRect_, kText, Font::Normal, Align::Center);
}

// --- Bedienung ---------------------------------------------------------------

void PianoRoll::applyVelocity(float x, float y) {
    const int value = std::clamp(static_cast<int>(std::lround(127.0f * (velocity_.bottom() - y) / (kVelocityH - 6.0f))), 1, 127);
    // Die Note, deren Anfang dem Mauszeiger am naechsten liegt (hoechstens 8 Pixel entfernt).
    int best = -1;
    float bestDistance = 8.0f;
    for (int i = 0; i < static_cast<int>(edit_.size()); ++i) {
        const float d = std::fabs(xOf(static_cast<double>(edit_[static_cast<std::size_t>(i)].start)) + 2.0f - x);
        if (d < bestDistance || (d == bestDistance && i == selected_)) { bestDistance = d; best = i; }
    }
    if (best < 0) return;
    edit_[static_cast<std::size_t>(best)].velocity = static_cast<std::uint8_t>(value);
    selected_ = best;
    changed_ = true;
}

void PianoRoll::mouseDown(float x, float y, bool rightButton) {
    if (!clip()) return;

    if (header_.contains(x, y)) {
        if (closeRect_.contains(x, y)) {
            close();
        } else if (divisionRect_.contains(x, y)) {
            division_ = rightButton ? (division_ == 4 ? 32 : division_ / 2) : (division_ == 32 ? 4 : division_ * 2);
        }
        return;
    }

    if (velocity_.contains(x, y) && !rightButton) {
        beginEdit();
        drag_ = Drag::Velocity;
        applyVelocity(x, y);
        return;
    }

    if (!grid_.contains(x, y)) return;

    const int hit = noteAt(x, y);
    if (rightButton) {
        if (hit >= 0) {
            beginEdit();
            edit_.erase(edit_.begin() + hit);
            changed_ = true;
            selected_ = -1;
            commit();
        }
        return;
    }

    beginEdit();
    if (hit >= 0) {
        selected_ = hit;
        const MidiNote& n = edit_[static_cast<std::size_t>(hit)];
        const float right = xOf(static_cast<double>(n.start + n.length));
        if (x > right - 6.0f) {
            drag_ = Drag::Resize;
        } else {
            drag_ = Drag::Move;
            grabOffset_ = static_cast<std::int64_t>(frameAt(x)) - n.start;
        }
        return;
    }

    // Leere Stelle: neue Note in der angeklickten Rasterzelle. Weiterziehen verlaengert sie.
    const int pitch = pitchAt(y);
    if (pitch < 0 || pitch > 127) { editing_ = false; return; }
    MidiNote note;
    note.start = std::max<std::int64_t>(0, snap(static_cast<std::int64_t>(frameAt(x)), true));
    note.length = gridStep();
    note.pitch = static_cast<std::uint8_t>(pitch);
    note.velocity = 100;
    edit_.push_back(note);
    selected_ = static_cast<int>(edit_.size()) - 1;
    changed_ = true;
    drag_ = Drag::Resize;
}

void PianoRoll::mouseMove(float x, float y) {
    if (drag_ == Drag::None || !editing_) return;
    if (drag_ == Drag::Velocity) { applyVelocity(x, y); return; }
    if (selected_ < 0 || selected_ >= static_cast<int>(edit_.size())) return;

    MidiNote& n = edit_[static_cast<std::size_t>(selected_)];
    const auto frame = static_cast<std::int64_t>(frameAt(x));
    if (drag_ == Drag::Move) {
        const std::int64_t start = std::max<std::int64_t>(0, snap(frame - grabOffset_, false));
        const int pitch = std::clamp(pitchAt(y), 0, 127);
        if (start != n.start || pitch != n.pitch) changed_ = true;
        n.start = start;
        n.pitch = static_cast<std::uint8_t>(pitch);
    } else if (drag_ == Drag::Resize) {
        const std::int64_t end = snap(frame, false);
        const std::int64_t length = std::max(gridStep(), end - n.start);
        if (length != n.length) changed_ = true;
        n.length = length;
    }
}

void PianoRoll::mouseUp(float x, float y) {
    if (drag_ != Drag::None) mouseMove(x, y);
    drag_ = Drag::None;
    commit();
}

void PianoRoll::wheel(float x, float /*y*/, float notches, bool ctrl, bool shift) {
    if (ctrl) {
        // Waagerecht zoomen um die Mausposition.
        const float anchorX = std::clamp(x, grid_.x, grid_.right());
        const double anchor = frameAt(anchorX);
        framesPerPixel_ = std::clamp(framesPerPixel_ * std::pow(0.8, static_cast<double>(notches)), 2.0, 8192.0);
        viewStart_ = anchor - static_cast<double>(anchorX - grid_.x) * framesPerPixel_;
    } else if (shift) {
        viewStart_ -= static_cast<double>(notches) * 100.0 * framesPerPixel_;
    } else {
        topPitch_ += notches > 0.0f ? 3 : -3;  // senkrecht durch die Oktaven
    }
    const MidiClipModel* c = clip();
    const double limit = c ? static_cast<double>(c->length) : 0.0;
    viewStart_ = std::clamp(viewStart_, -20.0 * framesPerPixel_, std::max(0.0, limit));
}

bool PianoRoll::deleteSelected() {
    const MidiClipModel* c = clip();
    if (!c || selected_ < 0 || selected_ >= static_cast<int>(c->notes.size())) return false;
    beginEdit();
    edit_.erase(edit_.begin() + selected_);
    changed_ = true;
    selected_ = -1;
    commit();
    return true;
}

}  // namespace ui
