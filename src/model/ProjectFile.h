#pragma once

#include "model/Project.h"

#include <array>
#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

// Inhalt einer Projektdatei, so wie er gespeichert wird. Audiodateien und
// Plugins stehen hier als Pfade; geoeffnet und geladen werden sie erst vom
// ProjectController.
struct SavedClip {
    std::filesystem::path file;
    std::int64_t start = 0;
    std::int64_t offset = 0;
    std::int64_t length = 0;
};

// Ein Plugin-Platz: Datei des Plugins, Anzeigename und Einstellungen, wie das
// Plugin sie selbst liefert. slot -1 kommt aus alten Dateien (ein Plugin je
// Spur); dann entscheidet der Typ des Plugins ueber den Platz.
struct SavedSlot {
    int slot = -1;
    std::filesystem::path file;
    std::wstring name;
    std::vector<char> state;
};

struct SavedTrack {
    std::wstring name;
    float gain = 1.0f;
    float pan = 0.0f;
    bool mute = false;
    bool solo = false;
    int input = kTrackInputAudio;
    int midiOutput = kTrackOutputSynth;
    std::vector<SavedSlot> slots;
    std::array<float, kProjectBuses> sends{};
    int output = -1;  // -1 = Master
    std::vector<SavedClip> clips;
    std::vector<MidiClipModel> midiClips;
};

struct SavedBus {
    std::wstring name;
    float gain = 1.0f;
    float pan = 0.0f;
    bool mute = false;
    std::vector<SavedSlot> slots;
};

struct SavedProject {
    double sampleRate = 0.0;  // Samplerate, mit der gespeichert wurde (Positionen sind in Frames)
    double bpm = 120.0;
    float masterGain = 1.0f;
    std::array<SavedTrack, kProjectTracks> tracks;
    std::array<SavedBus, kProjectBuses> buses;
    std::vector<SavedSlot> masterSlots;
};

// Das Dateiformat ist zeilenweiser Text in UTF-8, damit man es lesen und
// notfalls von Hand reparieren kann. Pfade unterhalb des Projektordners
// werden relativ gespeichert, alle anderen vollstaendig.
bool writeProjectFile(const std::filesystem::path& path, const SavedProject& project, std::string& error);
bool readProjectFile(const std::filesystem::path& path, SavedProject& project, std::string& error);

// Umwandlung zwischen UTF-8 und den breiten Zeichenketten des Modells.
std::string toUtf8(const std::wstring& text);
std::wstring fromUtf8(const std::string& text);
