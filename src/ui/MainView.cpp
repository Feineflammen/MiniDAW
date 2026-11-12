#include "ui/MainView.h"

#include <algorithm>
#include <cmath>
#include <cwchar>
#include <iterator>
#include <utility>

namespace ui {

namespace {

// Aufteilung
constexpr float kTopH = 44.0f;
constexpr float kRulerH = 24.0f;
constexpr float kHeaderW = 220.0f;
constexpr float kRowH = 44.0f;

const Rect kRewindRect{8.0f, 8.0f, 34.0f, 28.0f};
const Rect kPlayRect{46.0f, 8.0f, 58.0f, 28.0f};
const Rect kRecordRect{108.0f, 8.0f, 46.0f, 28.0f};
const Rect kTimeRect{162.0f, 6.0f, 124.0f, 32.0f};
const Rect kMasterLabelRect{298.0f, 8.0f, 50.0f, 28.0f};
const Rect kMasterRect{350.0f, 15.0f, 150.0f, 14.0f};
const Rect kMasterMeterRect{512.0f, 18.0f, 100.0f, 8.0f};
const Rect kMonitorRect{690.0f, 8.0f, 76.0f, 28.0f};
const Rect kLatencyMinusRect{778.0f, 12.0f, 20.0f, 20.0f};
const Rect kLatencyTextRect{800.0f, 8.0f, 112.0f, 28.0f};
const Rect kLatencyPlusRect{914.0f, 12.0f, 20.0f, 20.0f};

// Farben
constexpr Color kBg = rgb(0x1B1C1F);
constexpr Color kPanel = rgb(0x2A2C31);
constexpr Color kRowA = rgb(0x212226);
constexpr Color kRowB = rgb(0x25262B);
constexpr Color kLine = rgb(0x3A3C43);
constexpr Color kGrid = rgb(0x2F3137);
constexpr Color kText = rgb(0xDADCE2);
constexpr Color kTextDim = rgb(0x8D9099);
constexpr Color kAccent = rgb(0x4C9AFF);
constexpr Color kPlayhead = rgb(0xFF5A5A);
constexpr Color kMuteOn = rgb(0xE8A33D);
constexpr Color kRecord = rgb(0xE5484D);
constexpr Color kSoloOn = rgb(0xE6D34A);
constexpr Color kButton = rgb(0x3A3D45);
constexpr Color kMeter = rgb(0x4CD27A);
constexpr Color kMeterHot = rgb(0xFF5A5A);
constexpr Color kDark = rgb(0x15161A);

constexpr unsigned kClipColors[] = {0x3E7CB8, 0x3F9E7A, 0xB07A3C, 0x8A5BB5,
                                    0xB5525F, 0x4D9AA8, 0x8C9440, 0xA8608F};

// Fader: Stellung 0..1 entspricht -60..+6 dB, ganz links ist stumm.
constexpr float kMinDb = -60.0f;
constexpr float kMaxDb = 6.0f;

float gainToPosition(float gain) {
    if (gain <= 0.0f) return 0.0f;
    const float db = 20.0f * std::log10(gain);
    return std::clamp((db - kMinDb) / (kMaxDb - kMinDb), 0.0f, 1.0f);
}

float positionToGain(float position) {
    if (position <= 0.005f) return 0.0f;
    const float db = kMinDb + position * (kMaxDb - kMinDb);
    return std::pow(10.0f, db / 20.0f);
}

float sliderPosition(const Rect& rect, float x) {
    return std::clamp((x - rect.x) / rect.w, 0.0f, 1.0f);
}

std::wstring formatTime(double seconds) {
    if (seconds < 0.0) seconds = 0.0;
    const int minutes = static_cast<int>(seconds / 60.0);
    wchar_t buffer[32];
    std::swprintf(buffer, 32, L"%02d:%06.3f", minutes, seconds - 60.0 * minutes);
    return buffer;
}

// Wandelt die UTF-8-Meldungen des Kerns in UTF-16 um (Zeichen bis U+FFFF).
std::wstring widen(const std::string& text) {
    std::wstring out;
    for (std::size_t i = 0; i < text.size();) {
        const auto c = static_cast<unsigned char>(text[i]);
        unsigned code = c;
        std::size_t length = 1;
        if (c >= 0xE0 && i + 2 < text.size()) {
            code = ((c & 0x0Fu) << 12) | ((static_cast<unsigned char>(text[i + 1]) & 0x3Fu) << 6) |
                   (static_cast<unsigned char>(text[i + 2]) & 0x3Fu);
            length = 3;
        } else if (c >= 0xC0 && i + 1 < text.size()) {
            code = ((c & 0x1Fu) << 6) | (static_cast<unsigned char>(text[i + 1]) & 0x3Fu);
            length = 2;
        }
        out.push_back(static_cast<wchar_t>(code));
        i += length;
    }
    return out;
}

std::wstring formatGain(float gain) {
    if (gain <= 0.0f) return L"-inf";
    wchar_t buffer[32];
    std::swprintf(buffer, 32, L"%+.1f", 20.0 * std::log10(static_cast<double>(gain)));
    return buffer;
}

}  // namespace

MainView::MainView(ProjectController& controller, Engine& engine)
    : controller_(controller), engine_(engine) {
    framesPerPixel_ = controller_.sampleRate() / 50.0;  // 50 Pixel pro Sekunde
}

// --- Geometrie ---------------------------------------------------------------

float MainView::frameToX(double frame) const {
    return kHeaderW + static_cast<float>((frame - viewStart_) / framesPerPixel_);
}

double MainView::xToFrame(float x) const {
    return viewStart_ + static_cast<double>(x - kHeaderW) * framesPerPixel_;
}

float MainView::rowY(int track) const { return kTopH + kRulerH + static_cast<float>(track) * kRowH; }

int MainView::rowAt(float y) const {
    if (y < kTopH + kRulerH) return -1;
    const int row = static_cast<int>((y - kTopH - kRulerH) / kRowH);
    return row < kProjectTracks ? row : -1;
}

Rect MainView::armRect(int track) const { return {62.0f, rowY(track) + 12.0f, 20.0f, 20.0f}; }
Rect MainView::muteRect(int track) const { return {86.0f, rowY(track) + 12.0f, 20.0f, 20.0f}; }
Rect MainView::soloRect(int track) const { return {110.0f, rowY(track) + 12.0f, 20.0f, 20.0f}; }
Rect MainView::gainRect(int track) const { return {140.0f, rowY(track) + 7.0f, 56.0f, 14.0f}; }
Rect MainView::panRect(int track) const { return {140.0f, rowY(track) + 25.0f, 56.0f, 12.0f}; }
Rect MainView::meterRect(int track) const { return {204.0f, rowY(track) + 5.0f, 8.0f, 34.0f}; }

double MainView::rulerStep() const {
    static constexpr double kSteps[] = {0.01, 0.02, 0.05, 0.1, 0.2, 0.5, 1,   2,
                                        5,    10,   15,   30,  60,  120, 300, 600};
    const double secondsPerPixel = framesPerPixel_ / controller_.sampleRate();
    for (double step : kSteps)
        if (step / secondsPerPixel >= 80.0) return step;
    return 1200.0;
}

// --- Laufender Betrieb -------------------------------------------------------

void MainView::tick() {
    for (int t = 0; t < kProjectTracks; ++t) {
        auto& m = meters_[static_cast<std::size_t>(t)];
        m = std::max(engine_.takeTrackPeak(t), m * 0.82f);
        if (m < 0.0001f) m = 0.0f;
    }
    masterMeter_ = std::max(engine_.takeMasterPeak(), masterMeter_ * 0.82f);
    if (masterMeter_ < 0.0001f) masterMeter_ = 0.0f;

    inputMeter_ = std::max(engine_.takeInputPeak(), inputMeter_ * 0.82f);
    if (inputMeter_ < 0.0001f) inputMeter_ = 0.0f;

    if (statusTicks_ > 0 && --statusTicks_ == 0) status_.clear();

    // Fertig geschriebene Aufnahmen als Clip uebernehmen.
    std::string message;
    if (controller_.poll(message)) {
        const int track = controller_.armedTrack();
        if (track >= 0) {
            selTrack_ = track;
            selIndex_ = static_cast<int>(controller_.project().tracks[static_cast<std::size_t>(track)].clips.size()) - 1;
        }
    }
    if (!message.empty()) setStatus(widen(message));

    // Ansicht mitlaufen lassen, wenn der Abspielzeiger rechts hinauslaeuft.
    if (engine_.isPlaying() && drag_ == Drag::None) {
        const float x = frameToX(static_cast<double>(engine_.position()));
        if (x > width_ - 20.0f || x < kHeaderW) {
            viewStart_ = std::max(0.0, static_cast<double>(engine_.position()) -
                                           0.1 * static_cast<double>(width_ - kHeaderW) * framesPerPixel_);
        }
    }
}

void MainView::setStatus(const std::wstring& message) {
    status_ = message;
    statusTicks_ = 240;  // etwa acht Sekunden
}

void MainView::togglePlay() {
    if (engine_.isPlaying()) engine_.stop();
    else engine_.play();
}

void MainView::toggleRecord() {
    if (controller_.isRecording()) {
        engine_.stop();
        return;
    }
    std::string message;
    if (!controller_.startRecording(message)) setStatus(widen(message));
}

// --- Zeichnen ----------------------------------------------------------------

void MainView::draw(Canvas& c, float width, float height) {
    width_ = width;
    height_ = height;
    c.clear(kBg);
    drawTimeline(c);
    drawRuler(c);
    drawHeaders(c);
    drawTopBar(c);
}

void MainView::drawSlider(Canvas& c, const Rect& rect, float position, float fillFrom) {
    const float mid = rect.y + rect.h * 0.5f;
    c.fillRounded({rect.x, mid - 1.5f, rect.w, 3.0f}, 1.5f, kDark);
    const float a = rect.x + std::min(position, fillFrom) * rect.w;
    const float b = rect.x + std::max(position, fillFrom) * rect.w;
    if (b - a > 0.5f) c.fillRounded({a, mid - 1.5f, b - a, 3.0f}, 1.5f, kAccent);
    c.fillRounded({rect.x + position * rect.w - 3.5f, rect.y, 7.0f, rect.h}, 2.0f, kText);
}

void MainView::drawTopBar(Canvas& c) {
    c.fill({0.0f, 0.0f, width_, kTopH}, kPanel);
    c.line(0.0f, kTopH - 0.5f, width_, kTopH - 0.5f, kLine);

    c.fillRounded(kRewindRect, 4.0f, kButton);
    c.text(L"|<", kRewindRect, kText, Font::Normal, Align::Center);

    const bool playing = engine_.isPlaying();
    c.fillRounded(kPlayRect, 4.0f, playing ? kAccent : kButton);
    c.text(playing ? L"Stop" : L"Play", kPlayRect, playing ? kDark : kText, Font::Normal, Align::Center);

    const bool recording = controller_.isRecording();
    c.fillRounded(kRecordRect, 4.0f, recording ? kRecord : kButton);
    c.text(L"Rec", kRecordRect, recording ? rgb(0xFFFFFF) : kRecord, Font::Normal, Align::Center);

    c.fillRounded(kTimeRect, 4.0f, kDark);
    c.text(formatTime(static_cast<double>(engine_.position()) / controller_.sampleRate()), kTimeRect,
           kText, Font::Large, Align::Center);

    c.text(L"Master", kMasterLabelRect, kTextDim, Font::Normal, Align::Left);
    drawSlider(c, kMasterRect, gainToPosition(controller_.project().masterGain), 0.0f);

    c.fill(kMasterMeterRect, kDark);
    const float level = gainToPosition(masterMeter_);
    if (level > 0.0f) {
        c.fill({kMasterMeterRect.x, kMasterMeterRect.y, kMasterMeterRect.w * level, kMasterMeterRect.h},
               masterMeter_ >= 1.0f ? kMeterHot : kMeter);
    }
    c.text(formatGain(controller_.project().masterGain) + L" dB",
           {kMasterMeterRect.right() + 10.0f, 8.0f, 70.0f, 28.0f}, kTextDim, Font::Small);

    const bool monitor = controller_.monitor();
    c.fillRounded(kMonitorRect, 4.0f, monitor ? kAccent : kButton);
    c.text(L"Mith\u00F6ren", kMonitorRect, monitor ? kDark : kTextDim, Font::Normal, Align::Center);

    // Aufnahme-Latenz: angewendeter Wert, in Klammern der eingestellte Zusatz.
    c.fillRounded(kLatencyMinusRect, 3.0f, kButton);
    c.text(L"\u2212", kLatencyMinusRect, kText, Font::Normal, Align::Center);
    c.fillRounded(kLatencyPlusRect, 3.0f, kButton);
    c.text(L"+", kLatencyPlusRect, kText, Font::Normal, Align::Center);
    {
        const double ms = 1000.0 * static_cast<double>(controller_.appliedLatencyFrames()) / controller_.sampleRate();
        wchar_t label[64];
        if (controller_.recordOffsetMs() != 0)
            std::swprintf(label, 64, L"Latenz %.0f ms (%+d)", ms, controller_.recordOffsetMs());
        else
            std::swprintf(label, 64, L"Latenz %.0f ms", ms);
        c.text(label, kLatencyTextRect, controller_.recordOffsetMs() != 0 ? kText : kTextDim, Font::Small,
               Align::Center);
    }

    // Rechts: Meldung oder Geraeteinfo mit Aussetzer-Zaehler.
    const Rect right{kLatencyPlusRect.right() + 14.0f, 8.0f,
                     std::max(0.0f, width_ - kLatencyPlusRect.right() - 24.0f), 28.0f};
    if (!status_.empty()) {
        c.text(status_, right, kMuteOn, Font::Normal, Align::Right);
    } else {
        std::wstring info = deviceInfo_;
        const auto underruns = engine_.underruns();
        if (underruns > 0) info += L"   Aussetzer: " + std::to_wstring(underruns);
        c.text(info, right, kTextDim, Font::Small, Align::Right);
    }
}

void MainView::drawRuler(Canvas& c) {
    const Rect ruler{kHeaderW, kTopH, std::max(0.0f, width_ - kHeaderW), kRulerH};
    c.fill({0.0f, kTopH, width_, kRulerH}, kPanel);
    c.pushClip(ruler);

    const double rate = controller_.sampleRate();
    const double step = rulerStep();
    const double firstSecond = std::floor(viewStart_ / rate / step) * step;
    for (double s = firstSecond;; s += step) {
        const float x = frameToX(s * rate);
        if (x > width_) break;
        if (x < kHeaderW - 80.0f) continue;
        c.line(x + 0.5f, kTopH + 14.0f, x + 0.5f, kTopH + kRulerH, kTextDim);

        const int minutes = static_cast<int>(s / 60.0);
        wchar_t label[32];
        if (step < 1.0) std::swprintf(label, 32, L"%d:%05.2f", minutes, s - 60.0 * minutes);
        else std::swprintf(label, 32, L"%d:%02d", minutes, static_cast<int>(s - 60.0 * minutes + 0.5));
        c.text(label, {x + 4.0f, kTopH, 80.0f, kRulerH - 4.0f}, kTextDim, Font::Small);
    }

    const float px = frameToX(static_cast<double>(engine_.position()));
    c.fill({px - 4.0f, kTopH + kRulerH - 8.0f, 9.0f, 8.0f}, kPlayhead);
    c.popClip();
    c.line(0.0f, kTopH + kRulerH - 0.5f, width_, kTopH + kRulerH - 0.5f, kLine);
}

void MainView::drawClip(Canvas& c, const ClipModel& clip, int track, std::int64_t start, bool selected,
                        float alpha) {
    const float x0 = frameToX(static_cast<double>(start));
    const float x1 = frameToX(static_cast<double>(start + clip.length));
    const float visible0 = std::max(x0, kHeaderW);
    const float visible1 = std::min(x1, width_);
    if (visible1 <= visible0) return;

    // Rechteck auf den sichtbaren Bereich begrenzen (mit etwas Rand fuer die Ecken).
    const float rx0 = std::max(x0, kHeaderW - 8.0f);
    const float rx1 = std::min(x1, width_ + 8.0f);
    const Rect body{rx0, rowY(track) + 2.0f, std::max(2.0f, rx1 - rx0), kRowH - 4.0f};

    const unsigned base = kClipColors[static_cast<std::size_t>(track) % std::size(kClipColors)];
    c.fillRounded(body, 4.0f, rgb(base, 0.55f * alpha));

    // Wellenform: pro Pixelspalte ein senkrechter Strich vom kleinsten zum groessten Wert.
    if (clip.file && clip.file->peaks) {
        const PeakData& peaks = *clip.file->peaks;
        const std::size_t done = peaks.done.load(std::memory_order_acquire);
        const float centre = body.y + body.h * 0.5f + 5.0f;
        const float half = (body.h - 14.0f) * 0.5f;
        const Color wave = rgb(0xF2F4F8, 0.85f * alpha);
        const double clipEnd = static_cast<double>(clip.offset + clip.length);

        for (float px = std::floor(visible0); px < visible1; px += 1.0f) {
            double f0 = static_cast<double>(clip.offset) + static_cast<double>(px - x0) * framesPerPixel_;
            double f1 = f0 + framesPerPixel_;
            f0 = std::max(f0, static_cast<double>(clip.offset));
            f1 = std::min(f1, clipEnd);
            if (f1 <= f0) continue;

            const std::size_t b0 = static_cast<std::size_t>(f0) / PeakData::kBucket;
            if (b0 >= done) break;
            const std::size_t b1 = std::min(static_cast<std::size_t>(f1) / PeakData::kBucket, done - 1);

            float lo = 0.0f, hi = 0.0f;
            for (std::size_t b = b0; b <= b1; ++b) {
                lo = std::min(lo, peaks.lo[b]);
                hi = std::max(hi, peaks.hi[b]);
            }
            float y1 = centre - std::min(hi, 1.0f) * half;
            float y2 = centre - std::max(lo, -1.0f) * half;
            if (y2 - y1 < 1.0f) { y1 -= 0.5f; y2 += 0.5f; }
            c.line(px + 0.5f, y1, px + 0.5f, y2, wave);
        }
    }

    c.strokeRounded(body, 4.0f, selected ? rgb(0xFFFFFF, alpha) : rgb(base, alpha), selected ? 2.0f : 1.0f);
    if (clip.file) {
        c.text(clip.file->name, {visible0 + 5.0f, body.y + 1.0f, std::max(0.0f, visible1 - visible0 - 8.0f), 14.0f},
               rgb(0xFFFFFF, 0.9f * alpha), Font::Small);
    }
}

void MainView::drawTimeline(Canvas& c) {
    const float top = kTopH + kRulerH;
    const Rect area{kHeaderW, top, std::max(0.0f, width_ - kHeaderW), std::max(0.0f, height_ - top)};
    c.pushClip(area);

    for (int t = 0; t < kProjectTracks; ++t)
        c.fill({kHeaderW, rowY(t), area.w, kRowH}, (t % 2 == 0) ? kRowA : kRowB);

    // Senkrechte Hilfslinien an den Lineal-Marken.
    const double rate = controller_.sampleRate();
    const double step = rulerStep();
    const float bottom = rowY(kProjectTracks);
    for (double s = std::floor(viewStart_ / rate / step) * step;; s += step) {
        const float x = frameToX(s * rate);
        if (x > width_) break;
        if (x >= kHeaderW) c.line(x + 0.5f, top, x + 0.5f, bottom, kGrid);
    }

    const Project& project = controller_.project();
    const bool draggingClip = drag_ == Drag::Clip && dragMoved_;
    for (int t = 0; t < kProjectTracks; ++t) {
        const auto& clips = project.tracks[static_cast<std::size_t>(t)].clips;
        for (int i = 0; i < static_cast<int>(clips.size()); ++i) {
            const bool selected = t == selTrack_ && i == selIndex_;
            // Der gezogene Clip bleibt blass an seinem alten Platz stehen.
            drawClip(c, clips[static_cast<std::size_t>(i)], t, clips[static_cast<std::size_t>(i)].start,
                     selected && !draggingClip, (selected && draggingClip) ? 0.35f : 1.0f);
        }
    }

    // Laufende Aufnahme: roter Balken von der Startposition bis zum Abspielzeiger.
    std::int64_t recStart = 0, recFrames = 0;
    if (controller_.isRecording() && controller_.recordProgress(recStart, recFrames)) {
        const float x0 = std::max(frameToX(static_cast<double>(recStart)), kHeaderW - 8.0f);
        const float x1 = std::min(frameToX(static_cast<double>(recStart + recFrames)), width_ + 8.0f);
        if (x1 > x0) {
            const Rect body{x0, rowY(controller_.recordTrack()) + 2.0f, std::max(2.0f, x1 - x0), kRowH - 4.0f};
            c.fillRounded(body, 4.0f, rgb(0xE5484D, 0.45f));
            c.strokeRounded(body, 4.0f, kRecord);
            c.text(L"Aufnahme", {std::max(x0, kHeaderW) + 5.0f, body.y + 1.0f, 80.0f, 14.0f},
                   rgb(0xFFFFFF, 0.9f), Font::Small);
        }
    }

    if (draggingClip && selTrack_ >= 0) {
        const auto& clips = project.tracks[static_cast<std::size_t>(selTrack_)].clips;
        if (selIndex_ >= 0 && selIndex_ < static_cast<int>(clips.size()))
            drawClip(c, clips[static_cast<std::size_t>(selIndex_)], ghostTrack_, ghostStart_, true, 0.9f);
    }

    const float px = frameToX(static_cast<double>(engine_.position()));
    c.line(px + 0.5f, top, px + 0.5f, height_, kPlayhead);
    c.popClip();
}

void MainView::drawHeaders(Canvas& c) {
    const Project& project = controller_.project();
    c.fill({0.0f, kTopH + kRulerH, kHeaderW, std::max(0.0f, height_ - kTopH - kRulerH)}, kPanel);

    for (int t = 0; t < kProjectTracks; ++t) {
        const TrackModel& track = project.tracks[static_cast<std::size_t>(t)];
        const float y = rowY(t);

        c.fill({0.0f, y + 6.0f, 3.0f, kRowH - 12.0f},
               rgb(kClipColors[static_cast<std::size_t>(t) % std::size(kClipColors)]));
        c.text(track.name, {10.0f, y, 50.0f, kRowH}, kText);

        const bool armed = controller_.armedTrack() == t;
        const Rect a = armRect(t);
        c.fillRounded(a, 3.0f, armed ? kRecord : kButton);
        c.text(L"R", a, armed ? rgb(0xFFFFFF) : kTextDim, Font::Normal, Align::Center);

        const Rect m = muteRect(t);
        c.fillRounded(m, 3.0f, track.mute ? kMuteOn : kButton);
        c.text(L"M", m, track.mute ? kDark : kTextDim, Font::Normal, Align::Center);

        const Rect s = soloRect(t);
        c.fillRounded(s, 3.0f, track.solo ? kSoloOn : kButton);
        c.text(L"S", s, track.solo ? kDark : kTextDim, Font::Normal, Align::Center);

        drawSlider(c, gainRect(t), gainToPosition(track.gain), 0.0f);
        drawSlider(c, panRect(t), (track.pan + 1.0f) * 0.5f, 0.5f);

        const Rect meter = meterRect(t);
        c.fill(meter, kDark);
        // Die scharfgeschaltete Spur zeigt zusaetzlich den Eingangspegel.
        const float peak = std::max(meters_[static_cast<std::size_t>(t)], armed ? inputMeter_ : 0.0f);
        const float level = gainToPosition(peak);
        if (level > 0.0f) {
            const float h = meter.h * level;
            c.fill({meter.x, meter.bottom() - h, meter.w, h}, peak >= 1.0f ? kMeterHot : kMeter);
        }

        c.line(0.0f, y + kRowH - 0.5f, kHeaderW, y + kRowH - 0.5f, kLine);
    }
    c.line(kHeaderW - 0.5f, kTopH, kHeaderW - 0.5f, height_, kLine);
}

// --- Bedienung ---------------------------------------------------------------

void MainView::locateTo(float x) {
    const double frame = std::max(0.0, xToFrame(std::max(x, kHeaderW)));
    engine_.locate(static_cast<std::int64_t>(frame));
}

// Rastet eine Clip-Position an Nullpunkt, Abspielzeiger und Kanten anderer Clips ein.
std::int64_t MainView::snap(std::int64_t start, std::int64_t length, int skipTrack, int skipIndex) const {
    const double tolerance = 8.0 * framesPerPixel_;
    double bestDistance = tolerance;
    std::int64_t best = start;

    const auto consider = [&](std::int64_t edge, bool alsoEnd) {
        const double d1 = std::fabs(static_cast<double>(start - edge));
        if (d1 < bestDistance) { bestDistance = d1; best = edge; }
        if (alsoEnd) {
            const double d2 = std::fabs(static_cast<double>(start + length - edge));
            if (d2 < bestDistance) { bestDistance = d2; best = edge - length; }
        }
    };

    consider(0, false);
    consider(engine_.position(), true);
    const Project& project = controller_.project();
    for (int t = 0; t < kProjectTracks; ++t) {
        const auto& clips = project.tracks[static_cast<std::size_t>(t)].clips;
        for (int i = 0; i < static_cast<int>(clips.size()); ++i) {
            if (t == skipTrack && i == skipIndex) continue;
            const ClipModel& other = clips[static_cast<std::size_t>(i)];
            consider(other.start, true);
            consider(other.start + other.length, true);
        }
    }
    return std::max<std::int64_t>(0, best);
}

void MainView::mouseDown(float x, float y, bool doubleClick) {
    dragStartX_ = x;
    dragStartY_ = y;
    dragMoved_ = false;

    // Transportleiste
    if (y < kTopH) {
        if (kRewindRect.contains(x, y)) {
            engine_.locate(0);
            viewStart_ = 0.0;
        } else if (kPlayRect.contains(x, y)) {
            togglePlay();
        } else if (kRecordRect.contains(x, y)) {
            toggleRecord();
        } else if (kMonitorRect.contains(x, y)) {
            controller_.setMonitor(!controller_.monitor());
        } else if (kLatencyMinusRect.contains(x, y)) {
            controller_.setRecordOffsetMs(controller_.recordOffsetMs() - 5);
        } else if (kLatencyPlusRect.contains(x, y)) {
            controller_.setRecordOffsetMs(controller_.recordOffsetMs() + 5);
        } else if (kLatencyTextRect.contains(x, y) && doubleClick) {
            controller_.setRecordOffsetMs(0);
        } else if (x >= kMasterRect.x - 6.0f && x < kMasterRect.right() + 6.0f) {
            if (doubleClick) {
                controller_.setMasterGain(1.0f);
            } else {
                drag_ = Drag::MasterGain;
                applyDrag(x, y);
            }
        }
        return;
    }

    // Lineal
    if (y < kTopH + kRulerH) {
        if (x >= kHeaderW) {
            drag_ = Drag::Scrub;
            locateTo(x);
        }
        return;
    }

    const int track = rowAt(y);
    if (track < 0) return;

    // Spurkopf
    if (x < kHeaderW) {
        const TrackModel& model = controller_.project().tracks[static_cast<std::size_t>(track)];
        const Rect gain = gainRect(track);
        const Rect pan = panRect(track);
        if (armRect(track).contains(x, y)) {
            controller_.armTrack(controller_.armedTrack() == track ? -1 : track);
        } else if (muteRect(track).contains(x, y)) {
            controller_.setTrackMute(track, !model.mute);
        } else if (soloRect(track).contains(x, y)) {
            controller_.setTrackSolo(track, !model.solo);
        } else if (x >= gain.x - 6.0f && x < gain.right() + 6.0f && y >= gain.y - 2.0f && y < gain.bottom() + 2.0f) {
            if (doubleClick) {
                controller_.setTrackGain(track, 1.0f);
            } else {
                drag_ = Drag::TrackGain;
                dragTrack_ = track;
                applyDrag(x, y);
            }
        } else if (x >= pan.x - 6.0f && x < pan.right() + 6.0f && y >= pan.y - 2.0f && y < pan.bottom() + 2.0f) {
            if (doubleClick) {
                controller_.setTrackPan(track, 0.0f);
            } else {
                drag_ = Drag::TrackPan;
                dragTrack_ = track;
                applyDrag(x, y);
            }
        }
        return;
    }

    // Zeitachse: Clip unter der Maus suchen (der zuletzt angelegte liegt oben).
    const auto frame = static_cast<std::int64_t>(xToFrame(x));
    const auto& clips = controller_.project().tracks[static_cast<std::size_t>(track)].clips;
    for (int i = static_cast<int>(clips.size()) - 1; i >= 0; --i) {
        const ClipModel& clip = clips[static_cast<std::size_t>(i)];
        if (frame >= clip.start && frame < clip.start + clip.length) {
            selTrack_ = track;
            selIndex_ = i;
            drag_ = Drag::Clip;
            grabOffset_ = frame - clip.start;
            ghostStart_ = clip.start;
            ghostTrack_ = track;
            return;
        }
    }

    // Leere Stelle: Auswahl aufheben und Abspielzeiger setzen.
    selTrack_ = selIndex_ = -1;
    drag_ = Drag::Scrub;
    locateTo(x);
}

void MainView::applyDrag(float x, float y) {
    switch (drag_) {
        case Drag::MasterGain:
            controller_.setMasterGain(positionToGain(sliderPosition(kMasterRect, x)));
            break;
        case Drag::TrackGain:
            controller_.setTrackGain(dragTrack_, positionToGain(sliderPosition(gainRect(dragTrack_), x)));
            break;
        case Drag::TrackPan: {
            float pan = sliderPosition(panRect(dragTrack_), x) * 2.0f - 1.0f;
            if (std::fabs(pan) < 0.06f) pan = 0.0f;  // Mitte rastet ein
            controller_.setTrackPan(dragTrack_, pan);
            break;
        }
        case Drag::Scrub:
            locateTo(x);
            break;
        case Drag::Clip: {
            if (!dragMoved_ && std::fabs(x - dragStartX_) < 4.0f && std::fabs(y - dragStartY_) < 4.0f) break;
            dragMoved_ = true;
            const auto& clips = controller_.project().tracks[static_cast<std::size_t>(selTrack_)].clips;
            if (selIndex_ < 0 || selIndex_ >= static_cast<int>(clips.size())) break;
            const std::int64_t length = clips[static_cast<std::size_t>(selIndex_)].length;
            const auto wanted = std::max<std::int64_t>(0, static_cast<std::int64_t>(xToFrame(x)) - grabOffset_);
            ghostStart_ = snap(wanted, length, selTrack_, selIndex_);
            const int row = static_cast<int>(std::floor((y - kTopH - kRulerH) / kRowH));
            ghostTrack_ = std::clamp(row, 0, kProjectTracks - 1);
            break;
        }
        case Drag::None:
            break;
    }
}

void MainView::mouseMove(float x, float y) {
    if (drag_ != Drag::None) applyDrag(x, y);
}

void MainView::mouseUp(float x, float y) {
    if (drag_ == Drag::Clip) {
        applyDrag(x, y);
        if (dragMoved_) {
            const int index = controller_.moveClip(selTrack_, selIndex_, ghostTrack_, ghostStart_);
            if (index >= 0) {
                selTrack_ = ghostTrack_;
                selIndex_ = index;
            }
        }
    }
    drag_ = Drag::None;
    dragTrack_ = -1;
    dragMoved_ = false;
}

void MainView::wheel(float x, float y, float notches, bool ctrl) {
    // Mausrad ueber der Latenz-Anzeige: in Schritten von 1 ms verstellen.
    if (y < kTopH && x >= kLatencyMinusRect.x && x < kLatencyPlusRect.right()) {
        controller_.setRecordOffsetMs(controller_.recordOffsetMs() + (notches > 0.0f ? 1 : -1));
        return;
    }
    if (ctrl) {
        // Zoomen um die Mausposition.
        const float anchorX = std::max(x, kHeaderW);
        const double anchorFrame = xToFrame(anchorX);
        framesPerPixel_ = std::clamp(framesPerPixel_ * std::pow(0.8, static_cast<double>(notches)), 8.0, 16384.0);
        viewStart_ = std::max(0.0, anchorFrame - static_cast<double>(anchorX - kHeaderW) * framesPerPixel_);
    } else {
        viewStart_ = std::max(0.0, viewStart_ - static_cast<double>(notches) * 120.0 * framesPerPixel_);
    }
}

void MainView::key(Key k) {
    switch (k) {
        case Key::Space:
            togglePlay();
            break;
        case Key::Record:
            toggleRecord();
            break;
        case Key::Home:
            engine_.locate(0);
            viewStart_ = 0.0;
            break;
        case Key::Delete:
            if (drag_ == Drag::None && controller_.removeClip(selTrack_, selIndex_))
                selTrack_ = selIndex_ = -1;
            break;
    }
}

void MainView::dropFiles(float x, float y, const std::vector<std::filesystem::path>& paths) {
    int track = rowAt(y);
    if (track < 0) track = 0;

    // Auf dem Spurkopf abgelegt: an den Anfang. In der Zeitachse: an die Mausposition.
    std::int64_t start = 0;
    const bool atMouse = x >= kHeaderW;
    if (atMouse) start = std::max<std::int64_t>(0, static_cast<std::int64_t>(xToFrame(x)));

    bool first = true;
    for (const auto& path : paths) {
        if (track >= kProjectTracks) {
            setStatus(L"Nicht genug Spuren f\u00FCr alle Dateien.");
            break;
        }
        std::string message;
        if (controller_.addClip(track, path, start, message)) {
            const auto& clips = controller_.project().tracks[static_cast<std::size_t>(track)].clips;
            const int index = static_cast<int>(clips.size()) - 1;
            if (atMouse && first) {
                // Die erste Datei bestimmt die eingerastete Position, die anderen folgen ihr.
                start = snap(start, clips.back().length, track, index);
                controller_.moveClip(track, index, track, start);
            }
            selTrack_ = track;
            selIndex_ = index;
            first = false;
            if (!message.empty()) setStatus(widen(message));
            ++track;  // mehrere Dateien landen untereinander
        } else {
            setStatus(path.filename().wstring() + L": " + widen(message));
        }
    }
}

}  // namespace ui
