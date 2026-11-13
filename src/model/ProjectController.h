#pragma once

#include "disk/PeakData.h"
#include "engine/Engine.h"
#include "model/Project.h"
#include "model/ProjectFile.h"

#include <filesystem>
#include <functional>
#include <array>
#include <memory>
#include <string>
#include <vector>

// Sichert und laedt die Plugins der Kanalzuege. Der ProjectController kennt
// keine Plugin-Technik; die Umsetzung liegt beim VST3-Teil.
// strip: 0..15 Spuren, 16..19 Busse, 20 Master; slot: 0 Instrument, 1..4 Effekte.
class PluginPersistence {
public:
    virtual ~PluginPersistence() = default;
    // Liefert Datei und Einstellungen des Plugins auf diesem Platz. false, wenn er leer ist.
    virtual bool capture(int strip, int slot, std::filesystem::path& file, std::vector<char>& state) = 0;
    // Laedt das Plugin aus file auf den Platz und stellt seine Einstellungen her.
    // Ein leerer Pfad leert den Platz. slot -1 (alte Projektdateien): ein
    // Instrument kommt auf Platz 0, ein Effekt auf Platz 1.
    virtual bool restore(int strip, int slot, const std::filesystem::path& file, const std::vector<char>& state,
                         std::string& error) = 0;
};

// Einziger Weg, das Projektmodell zu aendern. Jede Aenderung landet im Modell
// und wird an die Engine weitergegeben. Nur vom UI-Thread benutzen.
//
// Hier haengt spaeter Undo/Redo ein (Schritt 9): Jede Methode wird dann zu
// einem Kommando mit apply() und revert().
class ProjectController {
public:
    explicit ProjectController(Engine& engine);

    const Project& project() const noexcept { return project_; }

    // --- Projektdatei ---
    void setPluginPersistence(PluginPersistence* persistence) noexcept { persistence_ = persistence; }
    // Leert das Projekt.
    void newProject();
    // Speichert das Projekt. Bei false steht der Grund in message.
    bool saveProject(const std::filesystem::path& path, std::string& message);
    // Laedt ein Projekt. Bei false steht der Grund in message und das bisherige
    // Projekt ist unveraendert; bei true kann message Hinweise enthalten
    // (fehlende Dateien, nicht ladbare Plugins).
    bool loadProject(const std::filesystem::path& path, std::string& message);
    // Pfad der Projektdatei, leer solange nie gespeichert wurde.
    const std::filesystem::path& projectPath() const noexcept { return projectPath_; }
    // true, wenn seit dem letzten Speichern oder Laden etwas geaendert wurde.
    // Einstellungen in Plugin-Fenstern werden dabei nicht erkannt.
    bool modified() const noexcept { return modified_; }
    // Laenge fuer den Export: Ende des letzten Audio- oder MIDI-Clips.
    std::int64_t contentLength() const noexcept;

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

    // Teilt einen Clip an der Zeitachsen-Position at in zwei. Liefert false,
    // wenn at nicht im Clip liegt. Der hintere Teil kommt ans Ende der Liste.
    bool splitClip(int track, int index, std::int64_t at);
    bool splitMidiClip(int track, int index, std::int64_t at);
    // Setzt Anfang und Ende eines Audio-Clips auf der Zeitachse neu (Kanten
    // ziehen). Das Material bleibt an seinem Platz, nur der sichtbare Ausschnitt
    // aendert sich; begrenzt auf die Datei.
    bool trimClip(int track, int index, std::int64_t newStart, std::int64_t newEnd);
    // Neue Laenge eines MIDI-Clips (Noten dahinter bleiben gespeichert, klingen aber nicht).
    bool resizeMidiClip(int track, int index, std::int64_t newLength);

    // Laenge der Blenden an Schnittkanten von Audio-Clips, in Frames.
    static constexpr std::int64_t kCutFade = 128;

    void setTrackGain(int track, float gain);
    void setTrackPan(int track, float pan);
    void setTrackMute(int track, bool mute);
    void setTrackSolo(int track, bool solo);
    void setMasterGain(float gain);

    // Mixer: Sends nach dem Fader (linear), Ausgang (-1 = Master, 0..3 = Bus), Busse.
    void setTrackSend(int track, int bus, float level);
    void setTrackOutput(int track, int bus);
    void setBusGain(int bus, float gain);
    void setBusPan(int bus, float pan);
    void setBusMute(int bus, bool mute);

    // Eingang (kTrackInputAudio, kTrackInputMidiAll oder Kanal 0..15) und
    // MIDI-Ausgang (kTrackOutputSynth oder Kanal 0..15) einer Spur.
    void setTrackInput(int track, int input);
    void setTrackMidiOutput(int track, int output);

    // Setzt das Plugin einer Spur (nullptr = keins). Ein Instrument bekommt das
    // MIDI der Spur anstelle des eingebauten Synthesizers, ein Effekt bearbeitet
    // ihren Klang. Das bisherige Plugin bleibt am Leben, bis der Audio-Thread es
    // nicht mehr benutzt (siehe poll()).
    void setPlugin(int strip, int slot, std::shared_ptr<AudioPlugin> plugin, std::wstring name);
    // Anzeige-Daten eines Platzes, nullptr bei ungueltiger Angabe.
    const SlotModel* slot(int strip, int slot) const noexcept;
    // Vor dem Beenden rufen, nachdem der Audio-Thread gestoppt ist: gibt alle Plugins frei.
    void releasePlugins();

    // Tempo (Schlaege pro Minute) fuer Raster und Metronom.
    void setBpm(double bpm);
    double bpm() const noexcept { return project_.bpm; }
    double framesPerBeat() const noexcept { return sampleRate_ * 60.0 / project_.bpm; }
    void setMetronome(bool on);
    bool metronome() const noexcept { return metronome_; }

    // Ersetzt die Noten eines MIDI-Clips und setzt seine Laenge.
    bool updateMidiClip(int track, int index, std::vector<MidiNote> notes, std::int64_t length);

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
    // Uebernimmt eine gemessene Latenz (Frames): Der Zusatz wird so gesetzt,
    // dass Aufnahmen genau um diesen Betrag verschoben werden.
    void applyMeasuredLatency(double frames);
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
    void clearProject();
    void finishMidiRecording(std::int64_t end);

    Engine& engine_;
    Project project_;
    PluginPersistence* persistence_ = nullptr;
    std::filesystem::path projectPath_;
    bool modified_ = false;
    double sampleRate_ = 44100.0;
    std::vector<std::weak_ptr<AudioFile>> files_;
    SlotArray* slotsOf(int strip) noexcept;
    std::array<std::array<std::shared_ptr<AudioPlugin>, kSlotsPerStrip>, kProjectStrips> plugins_;
    std::vector<std::shared_ptr<AudioPlugin>> retiringPlugins_;  // warten auf Rueckmeldung der Engine

    int inputChannels_ = 0;
    int armedTrack_ = -1;
    bool monitor_ = false;
    bool metronome_ = false;
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
