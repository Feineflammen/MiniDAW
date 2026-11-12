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
    std::wstring pluginName;       // Name des geladenen Plugins, leer = keins
    bool pluginIsInstrument = false;
    std::vector<ClipModel> clips;
    std::vector<MidiClipModel> midiClips;
};

struct Project {
    std::array<TrackModel, kProjectTracks> tracks;
    float masterGain = 1.0f;
    double bpm = 120.0;  // Tempo fuer Raster und Metronom, Taktart ist 4/4
};
