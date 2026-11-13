#pragma once

#include "disk/PeakData.h"
#include "disk/WavFile.h"

#include <array>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <string>
#include <vector>

// Das Projektmodell. Es gehoert dem UI-Thread; geaendert wird es nur ueber
// den ProjectController.

inline constexpr int kProjectTracks = 16;
inline constexpr int kProjectBuses = 4;
inline constexpr int kProjectInserts = 4;
// Plugin-Plaetze je Kanalzug: 0 = Instrument (nur Spuren), 1..4 = Effekte.
inline constexpr int kSlotsPerStrip = 1 + kProjectInserts;
// Kanalzuege: Spuren 0..15, Busse 16..19, Master 20 (wie in der Engine).
inline constexpr int kFirstBusStrip = kProjectTracks;
inline constexpr int kMasterStripIndex = kProjectTracks + kProjectBuses;
inline constexpr int kProjectStrips = kMasterStripIndex + 1;

// Was auf einem Plugin-Platz liegt (fuer die Anzeige).
struct SlotModel {
    std::wstring name;  // leer = Platz frei
    bool instrument = false;
    bool used() const noexcept { return !name.empty(); }
};
using SlotArray = std::array<SlotModel, kSlotsPerStrip>;

// Eine geladene Audiodatei mit ihren Uebersichtsdaten. Mehrere Clips koennen
// dieselbe Datei benutzen.
struct AudioFile {
    std::filesystem::path path;
    std::wstring name;
    std::shared_ptr<WavFile> wav;
    std::shared_ptr<PeakData> peaks;
};

struct ClipModel {
    std::shared_ptr<AudioFile> file;
    std::int64_t start = 0;   // Position auf der Zeitachse, in Frames
    std::int64_t offset = 0;  // erster benutzter Frame der Datei
    std::int64_t length = 0;
};

// Eine Note in einem MIDI-Clip. Zeiten in Frames, relativ zum Clip-Anfang.
struct MidiNote {
    std::int64_t start = 0;
    std::int64_t length = 0;
    std::uint8_t pitch = 60;
    std::uint8_t velocity = 100;
};

struct MidiClipModel {
    std::wstring name;
    std::int64_t start = 0;   // Position auf der Zeitachse, in Frames
    std::int64_t length = 0;
    std::vector<MidiNote> notes;
};

// Eingang einer Spur: Audio oder MIDI (alle Kanaele oder ein bestimmter).
// Die Werte entsprechen Engine::kInputAudio, kInputMidiAll und 0..15.
inline constexpr int kTrackInputAudio = -2;
inline constexpr int kTrackInputMidiAll = -1;
// MIDI-Ausgang einer Spur: eingebauter Synthesizer oder Kanal 0..15 am MIDI-Geraet.
inline constexpr int kTrackOutputSynth = -1;

struct TrackModel {
    std::wstring name;
    float gain = 1.0f;
    float pan = 0.0f;
    bool mute = false;
    bool solo = false;
    int input = kTrackInputAudio;
    int midiOutput = kTrackOutputSynth;
    SlotArray slots;                           // Instrument und Effekte
    std::array<float, kProjectBuses> sends{};  // Send-Pegel an die Busse, linear
    int output = -1;                           // -1 = Master, 0..3 = Bus
    std::vector<ClipModel> clips;
    std::vector<MidiClipModel> midiClips;
};

struct BusModel {
    std::wstring name;
    float gain = 1.0f;
    float pan = 0.0f;
    bool mute = false;
    SlotArray slots;  // Platz 0 bleibt leer
};

struct Project {
    std::array<TrackModel, kProjectTracks> tracks;
    std::array<BusModel, kProjectBuses> buses;
    SlotArray masterSlots;  // Platz 0 bleibt leer
    float masterGain = 1.0f;
    double bpm = 120.0;  // Tempo fuer Raster und Metronom, Taktart ist 4/4
};
