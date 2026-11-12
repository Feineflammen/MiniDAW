#pragma once

#include "disk/PeakData.h"
#include "engine/Engine.h"
#include "model/Project.h"

#include <filesystem>
#include <memory>
#include <string>
#include <vector>

// Einziger Weg, das Projektmodell zu aendern. Jede Aenderung landet im Modell
// und wird an die Engine weitergegeben. Nur vom UI-Thread benutzen.
//
// Hier haengt spaeter Undo/Redo ein (Schritt 9): Jede Methode wird dann zu
// einem Kommando mit apply() und revert().
class ProjectController {
public:
    explicit ProjectController(Engine& engine);

    const Project& project() const noexcept { return project_; }

    // Samplerate des Audiogeraets; dient der Zeitanzeige und der Warnung bei
    // Dateien mit anderer Samplerate.
    void setSampleRate(double sampleRate) noexcept { sampleRate_ = sampleRate; }
    double sampleRate() const noexcept { return sampleRate_; }

    // Legt eine WAV-Datei als Clip auf die Spur. Bei false steht der Grund in
    // message; bei true kann message einen Hinweis enthalten.
    bool addClip(int track, const std::filesystem::path& path, std::int64_t start, std::string& message);

    // Verschiebt einen Clip, auch auf eine andere Spur. Liefert den neuen Index
    // des Clips in der Zielspur oder -1.
    int moveClip(int track, int index, int newTrack, std::int64_t newStart);

    bool removeClip(int track, int index);

    void setTrackGain(int track, float gain);
    void setTrackPan(int track, float pan);
    void setTrackMute(int track, bool mute);
    void setTrackSolo(int track, bool solo);
    void setMasterGain(float gain);

    // Ende des letzten Clips, in Frames.
    std::int64_t projectLength() const noexcept;

    bool peaksIdle() const noexcept { return peakBuilder_.idle(); }

private:
    static bool validTrack(int track) noexcept { return track >= 0 && track < kProjectTracks; }
    std::shared_ptr<AudioFile> openFile(const std::filesystem::path& path, std::string& error);
    void syncTrack(int track);

    Engine& engine_;
    Project project_;
    double sampleRate_ = 44100.0;
    std::vector<std::weak_ptr<AudioFile>> files_;
    PeakBuilder peakBuilder_;
};
