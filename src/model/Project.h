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

struct TrackModel {
    std::wstring name;
    float gain = 1.0f;
    float pan = 0.0f;
    bool mute = false;
    bool solo = false;
    std::vector<ClipModel> clips;
};

struct Project {
    std::array<TrackModel, kProjectTracks> tracks;
    float masterGain = 1.0f;
};
