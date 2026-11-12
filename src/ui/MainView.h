#pragma once

#include "engine/Engine.h"
#include "model/ProjectController.h"
#include "ui/Canvas.h"
#include "ui/PianoRoll.h"
#include "ui/PluginHost.h"

#include <array>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <string>
#include <utility>
#include <vector>

namespace ui {

enum class Key { Space, Home, Delete, Record, Escape };

// Die gesamte Oberflaeche: Transportleiste, Spurkoepfe, Zeitachse.
// Kennt kein Fenster. Koordinaten sind geraeteunabhaengige Pixel (96 pro Zoll).
class MainView {
public:
    MainView(ProjectController& controller, Engine& engine);

    void setDeviceInfo(std::wstring info) { deviceInfo_ = std::move(info); }
    void setMidiInfo(std::wstring info) { midiInfo_ = std::move(info); }
    // Name des MIDI-Ausgabegeraets, leer = keins.
    void setMidiOutInfo(std::wstring name) { midiOutInfo_ = std::move(name); }
    // Zugang zu den Plugins; ohne ihn bleiben die Plugin-Felder ohne Funktion.
    void setPluginHost(PluginHost* host) { pluginHost_ = host; }
    // Oeffnet die Plugin-Auswahl fuer eine Spur.
    void openPluginPicker(int track);
    // Wird gerufen, wenn der Benutzer das MIDI-Ausgabegeraet weiterschalten will.
    std::function<void()> onCycleMidiOut;
    void selectClip(int track, int index, bool midi = false) { selTrack_ = track; selIndex_ = index; selMidi_ = midi; }

    // Zeigt eine Meldung oben rechts, fuer einige Sekunden.
    void showMessage(const std::wstring& message) { setStatus(message); }
    // Nach "Neu" oder "Oeffnen": Auswahl, Editor und Ansicht zuruecksetzen.
    void projectReplaced();

    void tick();  // einmal pro Bild: Pegel abholen, Ansicht nachfuehren
    void draw(Canvas& canvas, float width, float height);

    void mouseDown(float x, float y, bool doubleClick);
    void rightMouseDown(float x, float y);
    void mouseMove(float x, float y);
    void mouseUp(float x, float y);
    void wheel(float x, float y, float notches, bool ctrl, bool shift = false);
    // Oeffnet den Noten-Editor fuer einen MIDI-Clip.
    void openPianoRoll(int track, int index) { pianoRoll_.open(track, index); }
    void key(Key k);
    // Rechnertastatur als Klaviatur: semitone 0 = C. Wirkt nur mit scharfgeschalteter Spur.
    void noteKey(int semitone, bool down);
    void shiftOctave(int delta);
    void releaseAllNotes();
    void dropFiles(float x, float y, const std::vector<std::filesystem::path>& paths);

private:
    enum class Drag { None, MasterGain, TrackGain, TrackPan, Scrub, Clip };

    // Geometrie
    float frameToX(double frame) const;
    double xToFrame(float x) const;
    int rowAt(float y) const;
    float rowY(int track) const;
    Rect armRect(int track) const;
    Rect muteRect(int track) const;
    Rect soloRect(int track) const;
    Rect gainRect(int track) const;
    Rect panRect(int track) const;
    Rect meterRect(int track) const;
    Rect inputRect(int track) const;
    Rect outputRect(int track) const;
    Rect midiOutRect() const;
    Rect pluginRect(int track) const;
    Rect editorButtonRect(int track) const;
    Rect pickerRect() const;
    Rect editorRect() const;

    // Zeichnen
    void drawTopBar(Canvas& c);
    void drawRuler(Canvas& c);
    void drawTimeline(Canvas& c);
    void drawHeaders(Canvas& c);
    void drawStatusBar(Canvas& c);
    void drawPluginPicker(Canvas& c);
    bool pickerMouseDown(float x, float y);
    void drawClip(Canvas& c, const ClipModel& clip, int track, std::int64_t start, bool selected,
                  float alpha);
    void drawMidiClip(Canvas& c, const MidiClipModel& clip, int track, std::int64_t start, bool selected,
                      float alpha);
    void drawSlider(Canvas& c, const Rect& rect, float position, float fillFrom);

    // Bedienung
    void applyDrag(float x, float y);
    void locateTo(float x);
    void togglePlay();
    void toggleRecord();
    void stepInput(int track, int delta);
    void stepOutput(int track, int delta);
    bool selectedClip(std::int64_t& start, std::int64_t& length) const;
    std::int64_t snap(std::int64_t start, std::int64_t length, int skipTrack, int skipIndex) const;
    double rulerStep() const;
    double rulerBeats() const;
    void setStatus(const std::wstring& message);

    ProjectController& controller_;
    Engine& engine_;
    PianoRoll pianoRoll_;
    bool editorDrag_ = false;  // der laufende Ziehvorgang gehoert dem Noten-Editor

    // Plugin-Auswahl: eine Liste, die ueber der Zeitachse aufklappt.
    PluginHost* pluginHost_ = nullptr;
    int pickerTrack_ = -1;  // -1 = geschlossen
    int pickerScroll_ = 0;
    std::vector<std::wstring> pickerNames_;

    float width_ = 1280.0f, height_ = 800.0f;
    double viewStart_ = 0.0;         // Frame am linken Rand der Zeitachse
    double framesPerPixel_ = 882.0;  // Zoom

    int selTrack_ = -1, selIndex_ = -1;
    bool selMidi_ = false;  // die Auswahl meint einen MIDI-Clip

    Drag drag_ = Drag::None;
    int dragTrack_ = -1;
    float dragStartX_ = 0.0f, dragStartY_ = 0.0f;
    bool dragMoved_ = false;
    std::int64_t grabOffset_ = 0;
    std::int64_t ghostStart_ = 0;
    int ghostTrack_ = -1;

    std::array<float, kProjectTracks> meters_{};
    float masterMeter_ = 0.0f;
    float inputMeter_ = 0.0f;
    int octave_ = 4;  // Oktave der Rechnertastatur, 4 = C ist das mittlere C (Note 60)

    std::wstring deviceInfo_;
    std::wstring midiInfo_;
    std::wstring midiOutInfo_;
    std::uint32_t midiSeen_ = 0;       // zuletzt gesehener Zaehlerstand der Engine
    std::uint32_t midiErrorsSeen_ = 0;
    int midiLedTicks_ = 0;             // LED leuchtet, solange > 0
    int midiErrorTicks_ = 0;
    int midiTextTicks_ = 0;            // so lange bleibt die letzte Nachricht stehen
    std::wstring midiText_;
    bool lastMidiFromKeyboard_ = false;
    std::wstring status_;
    int statusTicks_ = 0;
};

}  // namespace ui
