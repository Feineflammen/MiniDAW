#pragma once

#include "disk/PeakData.h"
#include "engine/Engine.h"
#include "model/Project.h"

#include <filesystem>
#include <functional>
#include <array>
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

    // Eingang (kTrackInputAudio, kTrackInputMidiAll oder Kanal 0..15) und
    // MIDI-Ausgang (kTrackOutputSynth oder Kanal 0..15) einer Spur.
    void setTrackInput(int track, int input);
    void setTrackMidiOutput(int track, int output);

    // MIDI-Clips
    int addMidiClip(int track, MidiClipModel clip);  // liefert den Index oder -1
    int moveMidiClip(int track, int index, int newTrack, std::int64_t newStart);
    bool removeMidiClip(int track, int index);

    // --- Aufnahme ---
    // Anzahl der Eingangskanaele des Audiogeraets, 0 = kein Eingang.
    void setInputChannels(int channels) noexcept { inputChannels_ = channels; }
    int inputChannels() const noexcept { return inputChannels_; }
    // Ordner fuer neue Aufnahmen.
    void setRecordingFolder(std::filesystem::path folder) { recordingFolder_ = std::move(folder); }
    // Liefert die Gesamtlatenz (Eingang + Ausgang) in Frames. Um diesen Betrag
    // wird eine Aufnahme nach vorn geschoben, damit sie zum Gehoerten passt.
    void setLatencyProvider(std::function<std::int64_t()> provider) { latencyProvider_ = std::move(provider); }

    // Vom Benutzer eingestellter Zusatz zur geschaetzten Latenz, in Millisekunden.
    // Positiv schiebt Aufnahmen weiter nach vorn. Gleicht Soundkarten aus,
    // deren Treiber eine falsche Latenz melden.
    void setRecordOffsetMs(int ms) noexcept { recordOffsetMs_ = ms < -500 ? -500 : ms > 500 ? 500 : ms; }
    int recordOffsetMs() const noexcept { return recordOffsetMs_; }
    // Vom Geraet gemeldete Latenz in Frames (ohne den Zusatz).
    std::int64_t estimatedLatencyFrames() const;
    // Um so viel werden Aufnahmen tatsaechlich verschoben.
    std::int64_t appliedLatencyFrames() const;

    // Schaltet eine Spur fuer die Aufnahme scharf, -1 = keine. Immer nur eine Spur.
    void armTrack(int track);
    int armedTrack() const noexcept { return armedTrack_; }
    void setMonitor(bool on);
    bool monitor() const noexcept { return monitor_; }

    // Startet Wiedergabe und Aufnahme auf der scharfgeschalteten Spur.
    // Beendet wird mit Engine::stop().
    bool startRecording(std::string& message);
    bool isRecording() const noexcept { return recordSink_ != nullptr || midiRecording_; }
    int recordTrack() const noexcept { return recordTrack_; }
    // Fuer die Anzeige waehrend der Aufnahme. false, solange noch nichts aufgenommen ist.
    bool recordProgress(std::int64_t& start, std::int64_t& frames) const noexcept;

    // Regelmaessig aufrufen (einmal pro Bild). Legt den Clip an, sobald eine
    // beendete Aufnahme fertig geschrieben ist. true, wenn sich das Projekt
    // geaendert hat; message enthaelt dann gegebenenfalls einen Hinweis.
    bool poll(std::string& message);

    // Vor dem Beenden rufen, nachdem der Audio-Thread gestoppt ist: schliesst
    // eine noch laufende Aufnahme ab. Der Disk-Thread muss dabei noch laufen.
    void finishRecordingOnShutdown();

    // Ende des letzten Clips, in Frames.
    std::int64_t projectLength() const noexcept;

    bool peaksIdle() const noexcept { return peakBuilder_.idle(); }

private:
    static bool validTrack(int track) noexcept { return track >= 0 && track < kProjectTracks; }
    std::shared_ptr<AudioFile> openFile(const std::filesystem::path& path, std::string& error);
    void syncTrack(int track);
    void syncMidi(int track);
    void finishMidiRecording(std::int64_t end);

    Engine& engine_;
    Project project_;
    double sampleRate_ = 44100.0;
    std::vector<std::weak_ptr<AudioFile>> files_;

    int inputChannels_ = 0;
    int armedTrack_ = -1;
    bool monitor_ = false;
    int recordOffsetMs_ = 0;
    std::filesystem::path recordingFolder_;
    std::function<std::int64_t()> latencyProvider_;
    std::shared_ptr<RecordSink> recordSink_;
    int recordTrack_ = -1;

    // Laufende MIDI-Aufnahme
    bool midiRecording_ = false;
    bool midiRecordBegun_ = false;
    std::int64_t midiRecordStart_ = 0;
    std::vector<MidiNote> midiRecordNotes_;
    struct OpenNote { bool open = false; std::int64_t start = 0; std::uint8_t velocity = 0; };
    std::array<OpenNote, 128> midiRecordOpen_{};

    PeakBuilder peakBuilder_;
};
