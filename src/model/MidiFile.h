#pragma once

#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

// Standard-MIDI-Dateien (.mid), Typ 0 und 1. Plattformunabhaengig, ohne
// Bezug zum Projekt: Beim Lesen kommen Noten mit Zeiten in Sekunden heraus
// (die Tempo-Karte der Datei ist schon eingerechnet), beim Schreiben gehen
// Noten in Ticks bei festem Tempo hinein.

// --- Lesen -------------------------------------------------------------------

struct MidiFileNote {
    double start = 0.0;   // Sekunden ab Dateianfang
    double length = 0.0;  // Sekunden
    std::uint8_t pitch = 60;
    std::uint8_t velocity = 100;
};

// Noten eines Kanals aus einer Spur der Datei. Benutzt eine Spur mehrere
// Kanaele, wird sie in mehrere Teile aufgeteilt.
struct MidiFilePart {
    std::wstring name;  // Spurname aus der Datei, kann leer sein
    int channel = 0;    // 0..15
    bool splitFromTrack = false;  // die Spur der Datei hatte mehrere Kanaele
    std::vector<MidiFileNote> notes;  // nach Start sortiert
};

struct MidiFileData {
    int format = 1;
    std::vector<MidiFilePart> parts;  // nur Teile mit Noten, in Dateireihenfolge
    double firstBpm = 0.0;            // erstes Tempo der Datei, 0 = keine Angabe
    double end = 0.0;                 // Ende der letzten Note in Sekunden
};

// Liest eine Datei aus dem Speicher. false mit Meldung bei kaputten oder
// unbekannten Dateien.
bool parseMidiFile(const std::vector<std::uint8_t>& bytes, MidiFileData& out, std::string& error);
bool loadMidiFile(const std::filesystem::path& path, MidiFileData& out, std::string& error);

// --- Schreiben ---------------------------------------------------------------

struct MidiExportNote {
    std::int64_t tick = 0;
    std::int64_t length = 1;  // Ticks, mindestens 1
    std::uint8_t pitch = 60;
    std::uint8_t velocity = 100;
};

struct MidiExportTrack {
    std::wstring name;
    int channel = 0;
    std::vector<MidiExportNote> notes;
};

constexpr int kMidiExportPpq = 480;  // Ticks pro Viertel

// Typ 1: zuerst eine Tempospur (Tempo, 4/4), dann eine Spur je Eintrag.
std::vector<std::uint8_t> buildMidiFile(const std::vector<MidiExportTrack>& tracks, double bpm,
                                        int ppq = kMidiExportPpq);
bool saveMidiFile(const std::filesystem::path& path, const std::vector<std::uint8_t>& bytes, std::string& error);
