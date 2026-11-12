#pragma once

#include "audio/AudioCallback.h"
#include "disk/RecordSink.h"
#include "disk/TrackStream.h"
#include "engine/Synth.h"
#include "midi/MidiMessage.h"
#include "util/SpscQueue.h"

#include <array>
#include <atomic>
#include <cstdint>
#include <memory>
#include <mutex>
#include <vector>

// Die Engine rechnet das Audio. Sie kennt kein Audiogeraet: Sie bekommt nur
// "rechne N Samples" und laesst sich deshalb auch offline betreiben.
//
// Drei Seiten:
//  - UI-Thread: die Methoden im ersten Block. Sie schicken Kommandos ueber eine
//    lock-freie Queue und lesen Zustand ueber Atomics. Genau EIN Thread darf sie rufen.
//  - Disk-Thread: serviceDisk() liest die Audiodateien der Clips vor und
//    schreibt laufende Aufnahmen.
//  - Audio-Thread: prepare() und process().
class Engine final : public AudioCallback {
public:
    static constexpr int kNumTracks = 16;
    static constexpr float kTestToneLevel = 0.15f;

    // --- UI-Thread -------------------------------------------------------
    // Alle Setter liefern false, wenn der Index ungueltig oder die Queue voll ist.
    bool play() noexcept;
    bool stop() noexcept;  // beendet auch eine laufende Aufnahme
    bool locate(std::int64_t samplePosition) noexcept;

    bool setTrackGain(int track, float gain) noexcept;  // linear, 1.0 = 0 dB
    bool setTrackPan(int track, float pan) noexcept;    // -1 (links) .. +1 (rechts)
    bool setTrackMute(int track, bool mute) noexcept;
    bool setTrackSolo(int track, bool solo) noexcept;
    bool setMasterGain(float gain) noexcept;

    // Testton als Quelle einer Spur (fuer Tests). 0 Hz schaltet ihn aus.
    bool setTrackTestTone(int track, float hz) noexcept;

    // Ersetzt die Audio-Clips einer Spur. Wirkt, sobald der Disk-Thread sie uebernimmt.
    bool setTrackPlaylist(int track, Playlist playlist);

    // Live-MIDI (Keyboard, Rechnertastatur) fuer die scharfgeschaltete Spur.
    // Darf als einzige Methode aus mehreren Threads gerufen werden, zum
    // Beispiel aus dem Rueckruf des MIDI-Treibers; nie aus dem Audio-Thread.
    bool postMidi(MidiMessage message);
    // Meldet eine fehlerhafte Nachricht vom MIDI-Treiber (nur fuer die Anzeige).
    void reportMidiError() noexcept { midiErrors_.fetch_add(1, std::memory_order_relaxed); }

    // Fuer die MIDI-Anzeige: Anzahl aller bisher eingegangenen Nachrichten,
    // die zuletzt eingegangene und die Anzahl der Fehler.
    std::uint32_t midiMessageCount() const noexcept { return midiCount_.load(std::memory_order_relaxed); }
    std::uint32_t midiErrorCount() const noexcept { return midiErrors_.load(std::memory_order_relaxed); }
    MidiMessage lastMidiMessage() const noexcept {
        const std::uint32_t packed = lastMidi_.load(std::memory_order_relaxed);
        return {static_cast<std::uint8_t>(packed & 0xFF), static_cast<std::uint8_t>((packed >> 8) & 0xFF),
                static_cast<std::uint8_t>((packed >> 16) & 0xFF)};
    }

    // Spur, die den Eingang bekommt (Pegelanzeige, Mithoeren). -1 = keine.
    bool setArmedTrack(int track) noexcept;
    // Eingang ueber die scharfgeschaltete Spur mithoeren.
    bool setInputMonitor(bool on) noexcept;

    // Startet die Wiedergabe und nimmt den Eingang in sink auf, bis stop() kommt.
    // Der Aufrufer haelt sink am Leben, bis sink->done() gilt, und ruft dann
    // releaseRecordSink().
    bool record(std::shared_ptr<RecordSink> sink);
    void releaseRecordSink();

    // Anzahl der Bloecke, in denen einer Spur Daten von der Platte fehlten.
    std::uint32_t underruns() const noexcept { return underruns_.load(std::memory_order_relaxed); }

    std::int64_t position() const noexcept { return positionOut_.load(std::memory_order_relaxed); }
    bool isPlaying() const noexcept { return playingOut_.load(std::memory_order_relaxed); }
    bool isRecording() const noexcept { return recordingOut_.load(std::memory_order_relaxed); }

    // Liefert den hoechsten Pegel seit dem letzten Aufruf und setzt ihn zurueck.
    float takeTrackPeak(int track) noexcept;
    float takeMasterPeak() noexcept;
    float takeInputPeak() noexcept;

    // --- Disk-Thread -----------------------------------------------------
    // Liest Audiodaten fuer alle Spuren vor und schreibt Aufnahmedaten.
    // true, wenn etwas zu tun war. Wird von DiskThread in einer Schleife
    // gerufen, in Tests direkt.
    bool serviceDisk();

    // --- Audio-Thread ----------------------------------------------------
    void prepare(double sampleRate, int numInputs, int numOutputs, int maxFrames) override;
    void process(const float* const* in, int numInputs, float* const* out, int numOutputs,
                 int numFrames) noexcept override;

private:
    enum class CommandType : std::uint8_t {
        Play, Stop, Locate,
        TrackGain, TrackPan, TrackMute, TrackSolo, TrackTestTone,
        MasterGain,
        ArmTrack, InputMonitor, Record,
    };

    struct Command {
        CommandType type{};
        std::int32_t track = 0;
        float value = 0.0f;
        std::int64_t position = 0;
        RecordSink* sink = nullptr;
    };

    struct Track {
        // Mixerwerte
        float gain = 1.0f;
        float pan = 0.0f;
        bool mute = false;
        bool solo = false;
        // Aktuelle und angestrebte Verstaerkung je Seite (wird pro Block ueberblendet)
        float curL = 0.0f, curR = 0.0f;
        float tgtL = 0.0f, tgtR = 0.0f;
        // Testton
        float toneHz = 0.0f;
        double phase = 0.0;
        double phaseInc = 0.0;
    };

    bool send(const Command& c) noexcept { return commands_.push(c); }
    void drainCommands() noexcept;
    void updateTargets() noexcept;
    void renderBlock(int numFrames) noexcept;

    SpscQueue<Command, 1024> commands_;

    // Live-MIDI: mehrere Absender teilen sich die Schreibseite ueber einen Mutex,
    // der Audio-Thread liest ohne Sperre.
    SpscQueue<MidiMessage, 1024> midiIn_;
    std::mutex midiPostMutex_;
    std::atomic<std::uint32_t> midiCount_{0};
    std::atomic<std::uint32_t> midiErrors_{0};
    std::atomic<std::uint32_t> lastMidi_{0};

    // Zustand des Audio-Threads
    double sampleRate_ = 48000.0;
    int maxFrames_ = 0;
    std::array<Track, kNumTracks> tracks_{};
    float masterGain_ = 1.0f;
    float masterCur_ = 1.0f;
    float transportCur_ = 0.0f;  // blendet die Wiedergabe bei Start und Stopp
    bool wantPlay_ = false;      // Wunsch des Benutzers
    bool rolling_ = false;       // Transport laeuft (auch noch waehrend des Ausblendens)
    std::int64_t position_ = 0;
    int armedTrack_ = -1;
    bool monitor_ = false;
    RecordSink* recordSink_ = nullptr;
    const float* inL_ = nullptr;  // Eingang des aktuellen Blocks oder nullptr
    const float* inR_ = nullptr;
    std::vector<float> masterL_, masterR_;
    std::vector<float> trackL_, trackR_;
    std::array<TrackStream, kNumTracks> streams_;
    std::array<Synth, kNumTracks> synths_;
    std::array<MidiMessage, 128> liveMidi_{};  // Nachrichten fuer den naechsten Block
    int liveMidiCount_ = 0;

    // Rueckkanal zur UI
    std::atomic<std::int64_t> positionOut_{0};
    std::atomic<bool> playingOut_{false};
    std::atomic<bool> recordingOut_{false};
    std::array<std::atomic<float>, kNumTracks> trackPeak_{};
    std::atomic<float> masterPeak_{0.0f};
    std::atomic<float> inputPeak_{0.0f};
    std::atomic<std::uint32_t> underruns_{0};

    // Aufnahmeziel fuer den Disk-Thread (UI und Disk duerfen warten)
    std::mutex sinkMutex_;
    std::shared_ptr<RecordSink> diskSink_;
};
