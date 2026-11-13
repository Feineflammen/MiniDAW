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
constexpr float kHeaderW = 372.0f;
constexpr float kPickerRowH = 22.0f;
constexpr int kPickerRows = 16;
constexpr float kRowH = 44.0f;
constexpr float kStatusH = 24.0f;
constexpr float kEditorH = 310.0f;

// Feld links neben dem Lineal: Taktposition, Tempo, Metronom.
const Rect kBarRect{10.0f, 44.0f, 84.0f, 24.0f};
const Rect kBpmRect{100.0f, 47.0f, 84.0f, 18.0f};
const Rect kClickRect{192.0f, 47.0f, 50.0f, 18.0f};

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

// Beschreibt eine MIDI-Nachricht in Klartext, zum Beispiel "Kanal 1: Note C4 an (Anschlag 100)".
std::wstring describeMidi(MidiMessage m) {
    static const wchar_t* const kNames[] = {L"C", L"C#", L"D", L"D#", L"E", L"F",
                                            L"F#", L"G", L"G#", L"A", L"A#", L"H"};
    const int channel = (m.status & 0x0F) + 1;
    const std::wstring note = std::wstring(kNames[m.data1 % 12]) + std::to_wstring(m.data1 / 12 - 1);
    wchar_t buffer[96];
    if (m.isNoteOn()) std::swprintf(buffer, 96, L"Kanal %d: Note %ls an (Anschlag %d)", channel, note.c_str(), m.data2);
    else if (m.isNoteOff()) std::swprintf(buffer, 96, L"Kanal %d: Note %ls aus", channel, note.c_str());
    else if (m.type() == 0xB0) std::swprintf(buffer, 96, L"Kanal %d: Regler %d = %d", channel, m.data1, m.data2);
    else if (m.type() == 0xE0) std::swprintf(buffer, 96, L"Kanal %d: Pitchbend", channel);
    else if (m.type() == 0xC0) std::swprintf(buffer, 96, L"Kanal %d: Programm %d", channel, m.data1 + 1);
    else std::swprintf(buffer, 96, L"Kanal %d: Aftertouch", channel);
    return buffer;
}

std::wstring formatGain(float gain) {
    if (gain <= 0.0f) return L"-inf";
    wchar_t buffer[32];
    std::swprintf(buffer, 32, L"%+.1f", 20.0 * std::log10(static_cast<double>(gain)));
    return buffer;
}

}  // namespace

MainView::MainView(ProjectController& controller, Engine& engine)
    : controller_(controller), engine_(engine), pianoRoll_(controller, engine) {
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
Rect MainView::inputRect(int track) const { return {220.0f, rowY(track) + 5.0f, 66.0f, 16.0f}; }
Rect MainView::outputRect(int track) const { return {220.0f, rowY(track) + 23.0f, 66.0f, 16.0f}; }
Rect MainView::editorRect() const {
    return {0.0f, height_ - kStatusH - kEditorH, width_, kEditorH};
}
Rect MainView::pluginRect(int track) const { return {290.0f, rowY(track) + 5.0f, 76.0f, 16.0f}; }
Rect MainView::editorButtonRect(int track) const { return {290.0f, rowY(track) + 23.0f, 76.0f, 16.0f}; }

// Die Liste klappt rechts neben den Spurkoepfen auf, auf Hoehe der Spur, soweit Platz ist.
Rect MainView::pickerRect() const {
    const int rows = std::min(kPickerRows, static_cast<int>(pickerNames_.size()) + 1);
    const float h = 26.0f + static_cast<float>(std::max(rows, 2)) * kPickerRowH + 6.0f;
    const float top = kTopH + kRulerH + 4.0f;
    const float y = std::clamp(rowY(std::max(0, pickerTrack_)), top, std::max(top, height_ - kStatusH - h - 4.0f));
    return {kHeaderW + 6.0f, y, 340.0f, h};
}
Rect MainView::midiOutRect() const { return {width_ * 0.36f, height_ - kStatusH + 3.0f, 250.0f, kStatusH - 6.0f}; }

// Abstand der Lineal-Marken in Schlaegen: einzelne Schlaege, wenn Platz ist,
// sonst 1, 2, 4, 8 ... Takte.
double MainView::rulerBeats() const {
    const double pixelsPerBeat = controller_.framesPerBeat() / framesPerPixel_;
    if (pixelsPerBeat >= 60.0) return 1.0;
    double beats = 4.0;
    while (beats * pixelsPerBeat < 60.0) beats *= 2.0;
    return beats;
}

// Abstand der Lineal-Marken in Sekunden.
double MainView::rulerStep() const { return rulerBeats() * 60.0 / controller_.bpm(); }

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

    // MIDI-Anzeige: LED kurz aufleuchten lassen, letzte Nachricht merken.
    if (midiLedTicks_ > 0) --midiLedTicks_;
    if (midiErrorTicks_ > 0) --midiErrorTicks_;
    if (midiTextTicks_ > 0 && --midiTextTicks_ == 0) midiText_.clear();
    const std::uint32_t midiCount = engine_.midiMessageCount();
    if (midiCount != midiSeen_) {
        midiSeen_ = midiCount;
        midiLedTicks_ = 4;
        midiTextTicks_ = 90;
        const MidiMessage last = engine_.lastMidiMessage();
        midiText_ = describeMidi(last);
        // Die haeufigsten Gruende fuer "kein Ton" gleich benennen.
        if (last.isNoteOn() && status_.empty()) {
            const int armed = controller_.armedTrack();
            if (armed < 0) {
                setStatus(L"MIDI kommt an, aber keine Spur ist scharfgeschaltet: R an einer Spur anklicken.");
            } else {
                const TrackModel& track = controller_.project().tracks[static_cast<std::size_t>(armed)];
                const int channel = last.status & 0x0F;
                if (track.input == kTrackInputAudio)
                    setStatus(L"Die scharfe Spur steht auf Audio: Eingang (Feld rechts am Spurkopf) auf MIDI stellen.");
                else if (track.input >= 0 && track.input != channel && !lastMidiFromKeyboard_)
                    setStatus(L"Die scharfe Spur h\u00F6rt auf MIDI-Kanal " + std::to_wstring(track.input + 1) +
                              L", die Note kam auf Kanal " + std::to_wstring(channel + 1) + L".");
                else if (track.midiOutput >= 0 && midiOutInfo_.empty())
                    setStatus(L"Die Spur sendet an ein MIDI-Ger\u00E4t, aber es ist keins gew\u00E4hlt (unten \"MIDI aus\" anklicken).");
            }
        }
        lastMidiFromKeyboard_ = false;
    }
    const std::uint32_t midiErrors = engine_.midiErrorCount();
    if (midiErrors != midiErrorsSeen_) {
        midiErrorsSeen_ = midiErrors;
        midiErrorTicks_ = 30;
    }

    // Fertig geschriebene Aufnahmen als Clip uebernehmen.
    std::string message;
    if (controller_.poll(message)) {
        const int track = controller_.armedTrack();
        if (track >= 0) {
            const TrackModel& model = controller_.project().tracks[static_cast<std::size_t>(track)];
            selTrack_ = track;
            selMidi_ = model.input != kTrackInputAudio;
            selIndex_ = static_cast<int>(selMidi_ ? model.midiClips.size() : model.clips.size()) - 1;
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

void MainView::projectReplaced() {
    selTrack_ = selIndex_ = -1;
    selMidi_ = false;
    drag_ = Drag::None;
    pickerTrack_ = -1;
    pianoRoll_.close();
    viewStart_ = 0.0;
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
    if (pianoRoll_.isOpen()) pianoRoll_.draw(c, editorRect());
    drawStatusBar(c);
    drawTopBar(c);
    if (pickerTrack_ >= 0) drawPluginPicker(c);
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
        c.text(status_, right, kMuteOn, Font::Small, Align::Right);
    } else if (engine_.underruns() > 0) {
        c.text(L"Aussetzer: " + std::to_wstring(engine_.underruns()), right, kTextDim, Font::Small, Align::Right);
    }
}

// --- Plugin-Auswahl ----------------------------------------------------------

void MainView::openPluginPicker(int track) {
    if (!pluginHost_ || track < 0 || track >= kProjectTracks) return;
    pickerNames_ = pluginHost_->pluginNames();
    pickerTrack_ = track;
    pickerScroll_ = 0;
}

void MainView::drawPluginPicker(Canvas& c) {
    const Rect r = pickerRect();
    c.fillRounded({r.x + 2.0f, r.y + 3.0f, r.w, r.h}, 6.0f, rgb(0x000000, 0.45f));  // Schatten
    c.fillRounded(r, 6.0f, rgb(0x2E3037));
    c.strokeRounded(r, 6.0f, kLine);
    c.text(L"Plugin f\u00FCr Spur " + std::to_wstring(pickerTrack_ + 1), {r.x + 12.0f, r.y + 2.0f, r.w - 24.0f, 24.0f},
           kTextDim, Font::Small);

    // Zeile 0 ist immer "kein Plugin", danach die gefundenen Plugins.
    const int total = static_cast<int>(pickerNames_.size()) + 1;
    const int visible = std::min(kPickerRows, total);
    const std::wstring& current = controller_.project().tracks[static_cast<std::size_t>(pickerTrack_)].pluginName;
    for (int i = 0; i < visible; ++i) {
        const int item = pickerScroll_ + i;
        if (item >= total) break;
        const Rect row{r.x + 6.0f, r.y + 26.0f + static_cast<float>(i) * kPickerRowH, r.w - 12.0f, kPickerRowH - 2.0f};
        const bool isNone = item == 0;
        const bool active = isNone ? current.empty() : false;
        if (active) c.fillRounded(row, 3.0f, kButton);
        c.text(isNone ? std::wstring(L"Kein Plugin (eingebauter Synthesizer)")
                      : pickerNames_[static_cast<std::size_t>(item - 1)],
               {row.x + 8.0f, row.y, row.w - 16.0f, row.h}, isNone ? kTextDim : kText);
    }
    if (pickerNames_.empty() && pluginHost_) {
        c.text(L"Keine Plugins gefunden in " + pluginHost_->pluginFolder(),
               {r.x + 14.0f, r.y + 26.0f + kPickerRowH, r.w - 28.0f, kPickerRowH}, kMuteOn, Font::Small);
    } else if (total > visible) {
        // Rollbalken
        const float trackH = static_cast<float>(visible) * kPickerRowH;
        const float barH = std::max(16.0f, trackH * static_cast<float>(visible) / static_cast<float>(total));
        const float barY = r.y + 26.0f + (trackH - barH) * static_cast<float>(pickerScroll_) /
                                             static_cast<float>(std::max(1, total - visible));
        c.fillRounded({r.right() - 5.0f, barY, 3.0f, barH}, 1.5f, kTextDim);
    }
}

// Klick bei offener Auswahl. Liefert true, wenn der Klick damit erledigt ist.
bool MainView::pickerMouseDown(float x, float y) {
    if (pickerTrack_ < 0) return false;
    const Rect r = pickerRect();
    const int track = pickerTrack_;
    pickerTrack_ = -1;  // jeder Klick schliesst die Liste
    if (!r.contains(x, y) || !pluginHost_) return true;

    const int row = static_cast<int>(std::floor((y - r.y - 26.0f) / kPickerRowH));
    const int item = pickerScroll_ + row;
    if (row < 0 || item < 0 || item > static_cast<int>(pickerNames_.size())) return true;

    if (item == 0) {
        pluginHost_->unload(track);
    } else {
        std::wstring error;
        if (!pluginHost_->load(track, item - 1, error)) {
            setStatus(error);
        } else {
            // Ein frisch geladenes Plugin zeigt gleich sein Fenster.
            std::wstring ignored;
            pluginHost_->openEditor(track, ignored);
        }
    }
    return true;
}

// Statusleiste unten: links das Audiogeraet, rechts MIDI mit LED.
void MainView::drawStatusBar(Canvas& c) {
    const float y = height_ - kStatusH;
    c.fill({0.0f, y, width_, kStatusH}, kPanel);
    c.line(0.0f, y + 0.5f, width_, y + 0.5f, kLine);
    c.text(deviceInfo_, {10.0f, y, width_ * 0.36f - 20.0f, kStatusH}, kTextDim, Font::Small);

    // MIDI-Ausgabegeraet: anklicken schaltet zum naechsten weiter.
    const Rect out = midiOutRect();
    c.fillRounded(out, 3.0f, kButton);
    c.text(L"MIDI aus: " + (midiOutInfo_.empty() ? std::wstring(L"kein Ger\u00E4t") : midiOutInfo_),
           {out.x + 8.0f, out.y, out.w - 16.0f, out.h}, midiOutInfo_.empty() ? kTextDim : kText, Font::Small);

    // LED: gruen bei jeder gueltigen Nachricht, rot nach einem Uebertragungsfehler.
    const Rect led{width_ - 22.0f, y + 7.0f, 10.0f, 10.0f};
    const Color ledColor = midiErrorTicks_ > 0 ? kPlayhead : midiLedTicks_ > 0 ? kMeter : kDark;
    c.fillRounded(led, 5.0f, ledColor);
    c.strokeRounded(led, 5.0f, kLine);

    std::wstring text = L"MIDI ein: " + midiInfo_;
    if (midiErrorTicks_ > 0) text += L"   \u00DCbertragungsfehler";
    else if (!midiText_.empty()) text += L"   " + midiText_;
    const float textX = out.right() + 12.0f;
    c.text(text, {textX, y, std::max(0.0f, width_ - 30.0f - textX), kStatusH},
           midiText_.empty() ? kTextDim : kText, Font::Small, Align::Right);
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

        // Beschriftung in Takten, bei einzelnen Schlaegen als "Takt.Schlag".
        const auto beatIndex = static_cast<std::int64_t>(std::llround(s * controller_.bpm() / 60.0));
        const std::int64_t bar = beatIndex / 4 + 1;
        const std::int64_t beatInBar = beatIndex % 4 + 1;
        wchar_t label[32];
        if (beatInBar == 1) std::swprintf(label, 32, L"%lld", static_cast<long long>(bar));
        else std::swprintf(label, 32, L"%lld.%lld", static_cast<long long>(bar), static_cast<long long>(beatInBar));
        c.text(label, {x + 4.0f, kTopH, 80.0f, kRulerH - 4.0f}, beatInBar == 1 ? kText : kTextDim, Font::Small);
    }

    const float px = frameToX(static_cast<double>(engine_.position()));
    c.fill({px - 4.0f, kTopH + kRulerH - 8.0f, 9.0f, 8.0f}, kPlayhead);
    c.popClip();

    // Links daneben: Taktposition, Tempo und Metronom.
    {
        const double beats = static_cast<double>(engine_.position()) / controller_.framesPerBeat();
        const auto whole = static_cast<std::int64_t>(std::floor(beats + 1e-9));
        wchar_t label[48];
        std::swprintf(label, 48, L"Takt %lld.%lld", static_cast<long long>(whole / 4 + 1),
                      static_cast<long long>(whole % 4 + 1));
        c.text(label, kBarRect, kText, Font::Small);

        c.fillRounded(kBpmRect, 3.0f, kDark);
        std::swprintf(label, 48, L"%.0f BPM", controller_.bpm());
        c.text(label, kBpmRect, kText, Font::Small, Align::Center);

        const bool click = controller_.metronome();
        c.fillRounded(kClickRect, 3.0f, click ? kAccent : kButton);
        c.text(L"Klick", kClickRect, click ? kDark : kTextDim, Font::Small, Align::Center);
    }
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

// MIDI-Clip: Die Noten erscheinen als kleine Balken, nach Tonhoehe gestaffelt.
void MainView::drawMidiClip(Canvas& c, const MidiClipModel& clip, int track, std::int64_t start, bool selected,
                            float alpha) {
    const float x0 = frameToX(static_cast<double>(start));
    const float x1 = frameToX(static_cast<double>(start + clip.length));
    const float visible0 = std::max(x0, kHeaderW);
    const float visible1 = std::min(x1, width_);
    if (visible1 <= visible0) return;

    const float rx0 = std::max(x0, kHeaderW - 8.0f);
    const float rx1 = std::min(x1, width_ + 8.0f);
    const Rect body{rx0, rowY(track) + 2.0f, std::max(2.0f, rx1 - rx0), kRowH - 4.0f};
    const unsigned base = kClipColors[static_cast<std::size_t>(track) % std::size(kClipColors)];
    c.fillRounded(body, 4.0f, rgb(base, 0.40f * alpha));

    int low = 127, high = 0;
    for (const auto& note : clip.notes) {
        low = std::min<int>(low, note.pitch);
        high = std::max<int>(high, note.pitch);
    }
    if (low > high) { low = 60; high = 72; }
    if (high - low < 12) {  // mindestens eine Oktave zeigen, damit einzelne Noten nicht riesig werden
        const int pad = (12 - (high - low)) / 2;
        low -= pad;
        high = low + 12;
    }
    const float top = body.y + 15.0f;
    const float rangeH = body.h - 19.0f;
    const float noteH = std::max(2.0f, rangeH / static_cast<float>(high - low + 1));

    c.pushClip({visible0, body.y, visible1 - visible0, body.h});
    for (const auto& note : clip.notes) {
        const float nx0 = frameToX(static_cast<double>(start + note.start));
        const float nx1 = frameToX(static_cast<double>(start + std::min(note.start + note.length, clip.length)));
        if (nx1 < visible0 || nx0 > visible1) continue;
        const float ny = top + rangeH - static_cast<float>(note.pitch - low + 1) * rangeH / static_cast<float>(high - low + 1);
        c.fill({nx0, ny, std::max(2.0f, nx1 - nx0 - 1.0f), noteH}, rgb(0xF2F4F8, 0.9f * alpha));
    }
    c.popClip();

    c.strokeRounded(body, 4.0f, selected ? rgb(0xFFFFFF, alpha) : rgb(base, alpha), selected ? 2.0f : 1.0f);
    c.text(clip.name, {visible0 + 5.0f, body.y + 1.0f, std::max(0.0f, visible1 - visible0 - 8.0f), 14.0f},
           rgb(0xFFFFFF, 0.9f * alpha), Font::Small);
}

void MainView::drawTimeline(Canvas& c) {
    const float top = kTopH + kRulerH;
    const Rect area{kHeaderW, top, std::max(0.0f, width_ - kHeaderW),
                    std::max(0.0f, height_ - kStatusH - top)};
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
            const bool selected = !selMidi_ && t == selTrack_ && i == selIndex_;
            // Der gezogene Clip bleibt blass an seinem alten Platz stehen.
            drawClip(c, clips[static_cast<std::size_t>(i)], t, clips[static_cast<std::size_t>(i)].start,
                     selected && !draggingClip, (selected && draggingClip) ? 0.35f : 1.0f);
        }
    }

    for (int t = 0; t < kProjectTracks; ++t) {
        const auto& clips = project.tracks[static_cast<std::size_t>(t)].midiClips;
        for (int i = 0; i < static_cast<int>(clips.size()); ++i) {
            const bool selected = selMidi_ && t == selTrack_ && i == selIndex_;
            drawMidiClip(c, clips[static_cast<std::size_t>(i)], t, clips[static_cast<std::size_t>(i)].start,
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

    if (draggingClip && selTrack_ >= 0 && selIndex_ >= 0) {
        const TrackModel& model = project.tracks[static_cast<std::size_t>(selTrack_)];
        if (selMidi_ && selIndex_ < static_cast<int>(model.midiClips.size()))
            drawMidiClip(c, model.midiClips[static_cast<std::size_t>(selIndex_)], ghostTrack_, ghostStart_, true, 0.9f);
        else if (!selMidi_ && selIndex_ < static_cast<int>(model.clips.size()))
            drawClip(c, model.clips[static_cast<std::size_t>(selIndex_)], ghostTrack_, ghostStart_, true, 0.9f);
    }

    const float px = frameToX(static_cast<double>(engine_.position()));
    c.line(px + 0.5f, top, px + 0.5f, height_ - kStatusH, kPlayhead);
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

        // Eingang und MIDI-Ausgang der Spur.
        const Rect in = inputRect(t);
        c.fillRounded(in, 3.0f, kDark);
        const std::wstring inText = track.input == kTrackInputAudio     ? std::wstring(L"Audio")
                                    : track.input == kTrackInputMidiAll ? std::wstring(L"MIDI alle")
                                                                        : L"MIDI " + std::to_wstring(track.input + 1);
        c.text(inText, in, track.input == kTrackInputAudio ? kTextDim : kText, Font::Small, Align::Center);

        const Rect outBox = outputRect(t);
        c.fillRounded(outBox, 3.0f, kDark);
        const std::wstring outText = track.midiOutput == kTrackOutputSynth
                                         ? std::wstring(track.pluginIsInstrument ? L"\u2192 Plugin" : L"\u2192 Synth")
                                         : L"\u2192 MIDI " + std::to_wstring(track.midiOutput + 1);
        const bool usesMidi = track.input != kTrackInputAudio || !track.midiClips.empty();
        c.text(outText, outBox, usesMidi ? kText : kTextDim, Font::Small, Align::Center);

        // Plugin der Spur und Knopf fuer sein Fenster.
        const Rect plug = pluginRect(t);
        const bool hasPlugin = !track.pluginName.empty();
        c.fillRounded(plug, 3.0f, pickerTrack_ == t ? kAccent : kDark);
        c.text(hasPlugin ? track.pluginName : std::wstring(L"Plugin \u2026"), {plug.x + 4.0f, plug.y, plug.w - 8.0f, plug.h},
               pickerTrack_ == t ? kDark : hasPlugin ? kText : kTextDim, Font::Small, hasPlugin ? Align::Left : Align::Center);
        const Rect edit = editorButtonRect(t);
        c.fillRounded(edit, 3.0f, hasPlugin ? kButton : kDark);
        c.text(L"Fenster", edit, hasPlugin ? kText : rgb(0x55585F), Font::Small, Align::Center);

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
    // Naechster Schlag des Taktrasters.
    {
        const double beat = controller_.framesPerBeat();
        consider(static_cast<std::int64_t>(std::llround(std::round(static_cast<double>(start) / beat) * beat)), false);
    }
    const Project& project = controller_.project();
    for (int t = 0; t < kProjectTracks; ++t) {
        const auto& clips = project.tracks[static_cast<std::size_t>(t)].clips;
        for (int i = 0; i < static_cast<int>(clips.size()); ++i) {
            if (!selMidi_ && t == skipTrack && i == skipIndex) continue;
            const ClipModel& other = clips[static_cast<std::size_t>(i)];
            consider(other.start, true);
            consider(other.start + other.length, true);
        }
        const auto& midiClips = project.tracks[static_cast<std::size_t>(t)].midiClips;
        for (int i = 0; i < static_cast<int>(midiClips.size()); ++i) {
            if (selMidi_ && t == skipTrack && i == skipIndex) continue;
            consider(midiClips[static_cast<std::size_t>(i)].start, true);
            consider(midiClips[static_cast<std::size_t>(i)].start + midiClips[static_cast<std::size_t>(i)].length, true);
        }
    }
    return std::max<std::int64_t>(0, best);
}

bool MainView::selectedClip(std::int64_t& start, std::int64_t& length) const {
    if (selTrack_ < 0 || selIndex_ < 0) return false;
    const TrackModel& model = controller_.project().tracks[static_cast<std::size_t>(selTrack_)];
    if (selMidi_) {
        if (selIndex_ >= static_cast<int>(model.midiClips.size())) return false;
        start = model.midiClips[static_cast<std::size_t>(selIndex_)].start;
        length = model.midiClips[static_cast<std::size_t>(selIndex_)].length;
    } else {
        if (selIndex_ >= static_cast<int>(model.clips.size())) return false;
        start = model.clips[static_cast<std::size_t>(selIndex_)].start;
        length = model.clips[static_cast<std::size_t>(selIndex_)].length;
    }
    return true;
}

// Eingang weiterschalten: Audio, MIDI alle, MIDI 1 bis 16 (und wieder von vorn).
void MainView::stepInput(int track, int delta) {
    const int current = controller_.project().tracks[static_cast<std::size_t>(track)].input;
    const int count = 18;  // -2 .. 15
    const int next = ((current + 2 + delta) % count + count) % count - 2;
    if (controller_.isRecording()) return;
    controller_.setTrackInput(track, next);
    if (controller_.armedTrack() == track) releaseAllNotes();
}

// MIDI-Ausgang weiterschalten: Synthesizer, MIDI 1 bis 16.
void MainView::stepOutput(int track, int delta) {
    const int current = controller_.project().tracks[static_cast<std::size_t>(track)].midiOutput;
    const int count = 17;  // -1 .. 15
    controller_.setTrackMidiOutput(track, ((current + 1 + delta) % count + count) % count - 1);
}

void MainView::rightMouseDown(float x, float y) {
    if (pickerTrack_ >= 0) { pickerTrack_ = -1; return; }
    if (pianoRoll_.isOpen() && editorRect().contains(x, y)) {
        pianoRoll_.mouseDown(x, y, true);
        return;
    }
    if (kBpmRect.contains(x, y)) {
        controller_.setBpm(controller_.bpm() - 1.0);
        return;
    }
    const int track = rowAt(y);
    if (track < 0 || x >= kHeaderW) return;
    if (inputRect(track).contains(x, y)) stepInput(track, -1);
    else if (outputRect(track).contains(x, y)) stepOutput(track, -1);
}

void MainView::mouseDown(float x, float y, bool doubleClick) {
    dragStartX_ = x;
    dragStartY_ = y;
    dragMoved_ = false;

    if (pickerMouseDown(x, y)) return;

    // Statusleiste
    if (y >= height_ - kStatusH) {
        if (midiOutRect().contains(x, y) && onCycleMidiOut) onCycleMidiOut();
        return;
    }

    // Noten-Editor
    if (pianoRoll_.isOpen() && editorRect().contains(x, y)) {
        editorDrag_ = true;
        pianoRoll_.mouseDown(x, y, false);
        return;
    }

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
        if (kBpmRect.contains(x, y)) {
            controller_.setBpm(controller_.bpm() + 1.0);
        } else if (kClickRect.contains(x, y)) {
            controller_.setMetronome(!controller_.metronome());
        } else if (x >= kHeaderW) {
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
        if (pluginRect(track).contains(x, y)) {
            openPluginPicker(track);
        } else if (editorButtonRect(track).contains(x, y)) {
            std::wstring error;
            if (pluginHost_ && !pluginHost_->openEditor(track, error) && !error.empty()) setStatus(error);
        } else if (inputRect(track).contains(x, y)) {
            stepInput(track, 1);
        } else if (outputRect(track).contains(x, y)) {
            stepOutput(track, 1);
        } else if (armRect(track).contains(x, y)) {
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
    const auto& midiClips = controller_.project().tracks[static_cast<std::size_t>(track)].midiClips;
    for (int i = static_cast<int>(midiClips.size()) - 1; i >= 0; --i) {
        const MidiClipModel& clip = midiClips[static_cast<std::size_t>(i)];
        if (frame >= clip.start && frame < clip.start + clip.length) {
            selTrack_ = track;
            selIndex_ = i;
            selMidi_ = true;
            if (doubleClick) {  // Doppelklick oeffnet den Noten-Editor
                pianoRoll_.open(track, i);
                return;
            }
            const float x1 = frameToX(static_cast<double>(clip.start + clip.length));
            if (x1 - frameToX(static_cast<double>(clip.start)) > 24.0f && x1 - x < 6.0f) {
                drag_ = Drag::TrimEnd;  // rechte Kante: Laenge aendern
                return;
            }
            drag_ = Drag::Clip;
            grabOffset_ = frame - clip.start;
            ghostStart_ = clip.start;
            ghostTrack_ = track;
            return;
        }
    }
    const auto& clips = controller_.project().tracks[static_cast<std::size_t>(track)].clips;
    for (int i = static_cast<int>(clips.size()) - 1; i >= 0; --i) {
        const ClipModel& clip = clips[static_cast<std::size_t>(i)];
        if (frame >= clip.start && frame < clip.start + clip.length) {
            selTrack_ = track;
            selIndex_ = i;
            selMidi_ = false;
            // Nahe an einer Kante: die Kante ziehen (Clip kuerzen oder verlaengern).
            const float x0 = frameToX(static_cast<double>(clip.start));
            const float x1 = frameToX(static_cast<double>(clip.start + clip.length));
            if (x1 - x0 > 24.0f && x - x0 < 6.0f) { drag_ = Drag::TrimStart; return; }
            if (x1 - x0 > 24.0f && x1 - x < 6.0f) { drag_ = Drag::TrimEnd; return; }
            drag_ = Drag::Clip;
            grabOffset_ = frame - clip.start;
            ghostStart_ = clip.start;
            ghostTrack_ = track;
            return;
        }
    }

    // Doppelklick auf eine leere Stelle einer MIDI-Spur: neuer Clip von vier
    // Takten ab dem Taktstrich davor, gleich im Noten-Editor geoeffnet.
    if (doubleClick && controller_.project().tracks[static_cast<std::size_t>(track)].input != kTrackInputAudio) {
        const double bar = controller_.framesPerBeat() * 4.0;
        MidiClipModel clip;
        clip.name = L"MIDI";
        clip.start = static_cast<std::int64_t>(std::llround(std::floor(static_cast<double>(frame) / bar) * bar));
        clip.length = static_cast<std::int64_t>(std::llround(4.0 * bar));
        const int index = controller_.addMidiClip(track, std::move(clip));
        if (index >= 0) {
            selTrack_ = track;
            selIndex_ = index;
            selMidi_ = true;
            pianoRoll_.open(track, index);
        }
        return;
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
            std::int64_t clipStart = 0, length = 0;
            if (!selectedClip(clipStart, length)) break;
            const auto wanted = std::max<std::int64_t>(0, static_cast<std::int64_t>(xToFrame(x)) - grabOffset_);
            ghostStart_ = dragFree_ ? wanted : snap(wanted, length, selTrack_, selIndex_);
            const int row = static_cast<int>(std::floor((y - kTopH - kRulerH) / kRowH));
            ghostTrack_ = std::clamp(row, 0, kProjectTracks - 1);
            break;
        }
        case Drag::TrimStart:
        case Drag::TrimEnd: {
            std::int64_t clipStart = 0, length = 0;
            if (!selectedClip(clipStart, length)) break;
            auto edge = std::max<std::int64_t>(0, static_cast<std::int64_t>(xToFrame(x)));
            if (!dragFree_) edge = snap(edge, 0, selTrack_, selIndex_);
            if (selMidi_) {
                controller_.resizeMidiClip(selTrack_, selIndex_, std::max<std::int64_t>(1, edge - clipStart));
            } else if (drag_ == Drag::TrimStart) {
                controller_.trimClip(selTrack_, selIndex_, std::min(edge, clipStart + length - 1), clipStart + length);
            } else {
                controller_.trimClip(selTrack_, selIndex_, clipStart, std::max(edge, clipStart + 1));
            }
            break;
        }
        case Drag::None:
            break;
    }
}

// Verschiebt den ausgewaehlten Clip um wenige Millisekunden (Pfeiltasten).
void MainView::nudgeSelected(double ms) {
    std::int64_t start = 0, length = 0;
    if (!selectedClip(start, length)) {
        setStatus(L"Erst einen Clip anklicken, dann mit den Pfeiltasten verschieben.");
        return;
    }
    const auto delta = static_cast<std::int64_t>(std::llround(ms * controller_.sampleRate() / 1000.0));
    const std::int64_t target = std::max<std::int64_t>(0, start + delta);
    const int index = selMidi_ ? controller_.moveMidiClip(selTrack_, selIndex_, selTrack_, target)
                               : controller_.moveClip(selTrack_, selIndex_, selTrack_, target);
    if (index >= 0) selIndex_ = index;
    wchar_t text[64];
    std::swprintf(text, 64, L"Clip verschoben: %+.0f ms", ms);
    setStatus(text);
}

// Teilt den ausgewaehlten Clip am Abspielzeiger; ausgewaehlt bleibt der hintere Teil.
void MainView::splitAtPlayhead() {
    std::int64_t start = 0, length = 0;
    const std::int64_t at = engine_.position();
    if (!selectedClip(start, length) || at <= start || at >= start + length) {
        setStatus(L"Zum Teilen einen Clip anklicken und den Abspielzeiger in ihn setzen.");
        return;
    }
    const bool ok = selMidi_ ? controller_.splitMidiClip(selTrack_, selIndex_, at)
                             : controller_.splitClip(selTrack_, selIndex_, at);
    if (!ok) return;
    const TrackModel& model = controller_.project().tracks[static_cast<std::size_t>(selTrack_)];
    selIndex_ = static_cast<int>(selMidi_ ? model.midiClips.size() : model.clips.size()) - 1;
}

void MainView::mouseMove(float x, float y, bool free) {
    dragFree_ = free;
    if (editorDrag_) { pianoRoll_.mouseMove(x, y); return; }
    if (drag_ != Drag::None) applyDrag(x, y);
}

void MainView::mouseUp(float x, float y, bool free) {
    dragFree_ = free;
    if (editorDrag_) {
        editorDrag_ = false;
        pianoRoll_.mouseUp(x, y);
        return;
    }
    if (drag_ == Drag::Clip) {
        applyDrag(x, y);
        if (dragMoved_) {
            const int index = selMidi_ ? controller_.moveMidiClip(selTrack_, selIndex_, ghostTrack_, ghostStart_)
                                       : controller_.moveClip(selTrack_, selIndex_, ghostTrack_, ghostStart_);
            if (index >= 0) {
                // Der Noten-Editor folgt dem verschobenen Clip; andere Indizes koennen sich aendern.
                const bool editorOnThis = selMidi_ && pianoRoll_.isOpen() && pianoRoll_.track() == selTrack_ &&
                                          pianoRoll_.index() == selIndex_;
                if (selMidi_ && pianoRoll_.isOpen() && ghostTrack_ != selTrack_) {
                    if (editorOnThis) pianoRoll_.open(ghostTrack_, index);
                    else if (pianoRoll_.track() == selTrack_) pianoRoll_.close();
                }
                selTrack_ = ghostTrack_;
                selIndex_ = index;
            }
        }
    }
    drag_ = Drag::None;
    dragTrack_ = -1;
    dragMoved_ = false;
}

void MainView::wheel(float x, float y, float notches, bool ctrl, bool shift) {
    // Offene Plugin-Auswahl: das Mausrad rollt die Liste.
    if (pickerTrack_ >= 0) {
        const int total = static_cast<int>(pickerNames_.size()) + 1;
        pickerScroll_ = std::clamp(pickerScroll_ - static_cast<int>(notches * 3.0f), 0, std::max(0, total - kPickerRows));
        return;
    }
    if (pianoRoll_.isOpen() && editorRect().contains(x, y)) {
        pianoRoll_.wheel(x, y, notches, ctrl, shift);
        return;
    }
    // Mausrad ueber dem Tempo: 1 BPM pro Raste.
    if (kBpmRect.contains(x, y)) {
        controller_.setBpm(controller_.bpm() + (notches > 0.0f ? 1.0 : -1.0));
        return;
    }
    // Mausrad ueber den Feldern am Spurkopf: Eingang oder MIDI-Ausgang weiterschalten.
    if (x < kHeaderW) {
        const int track = rowAt(y);
        if (track >= 0 && inputRect(track).contains(x, y)) { stepInput(track, notches > 0.0f ? 1 : -1); return; }
        if (track >= 0 && outputRect(track).contains(x, y)) { stepOutput(track, notches > 0.0f ? 1 : -1); return; }
    }
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
        case Key::NudgeLeft: nudgeSelected(-1.0); break;
        case Key::NudgeRight: nudgeSelected(1.0); break;
        case Key::NudgeLeftBig: nudgeSelected(-10.0); break;
        case Key::NudgeRightBig: nudgeSelected(10.0); break;
        case Key::Split: splitAtPlayhead(); break;
        case Key::Escape:
            if (pickerTrack_ >= 0) pickerTrack_ = -1;
            else pianoRoll_.close();
            break;
        case Key::Delete:
            // Bei offenem Noten-Editor loescht Entf zuerst die ausgewaehlte Note.
            if (pianoRoll_.isOpen() && pianoRoll_.deleteSelected()) break;
            if (selMidi_ && pianoRoll_.isOpen() && pianoRoll_.track() == selTrack_) pianoRoll_.close();
            if (drag_ == Drag::None &&
                (selMidi_ ? controller_.removeMidiClip(selTrack_, selIndex_)
                          : controller_.removeClip(selTrack_, selIndex_)))
                selTrack_ = selIndex_ = -1;
            break;
    }
}

void MainView::noteKey(int semitone, bool down) {
    if (controller_.armedTrack() < 0) return;
    const int note = 12 * (octave_ + 1) + semitone;
    if (note < 0 || note > 127) return;
    lastMidiFromKeyboard_ = true;
    engine_.postMidi(down ? MidiMessage::noteOn(note, 100) : MidiMessage::noteOff(note), true);
}

void MainView::shiftOctave(int delta) {
    releaseAllNotes();
    octave_ = std::clamp(octave_ + delta, 0, 8);
    setStatus(L"Tastatur-Oktave: C" + std::to_wstring(octave_));
}

void MainView::releaseAllNotes() { engine_.postMidi(MidiMessage::allNotesOff(), true); }

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
            selMidi_ = false;
            first = false;
            if (!message.empty()) setStatus(widen(message));
            ++track;  // mehrere Dateien landen untereinander
        } else {
            setStatus(path.filename().wstring() + L": " + widen(message));
        }
    }
}

}  // namespace ui
