#pragma once

#include "audio/AudioCallback.h"
#include "disk/TrackStream.h"
#include "util/SpscQueue.h"

#include <array>
#include <atomic>
#include <cstdint>
#include <vector>

// Die Engine rechnet das Audio. Sie kennt kein Audiogeraet: Sie bekommt nur
// "rechne N Samples" und laesst sich deshalb auch offline betreiben.
//
// Zwei Seiten:
//  - UI-Thread: die Methoden im ersten Block. Sie schicken Kommandos ueber eine
//    lock-freie Queue und lesen Zustand ueber Atomics. Genau EIN Thread darf sie rufen.
//  - Disk-Thread: serviceDisk() liest die Audiodateien der Clips vor.
//  - Audio-Thread: prepare() und process().
class Engine final : public AudioCallback {
public:
    static constexpr int kNumTracks = 16;
    static constexpr float kTestToneLevel = 0.15f;

    // --- UI-Thread -------------------------------------------------------
    // Alle Setter liefern false, wenn der Index ungueltig oder die Queue voll ist.
    bool play() noexcept;
    bool stop() noexcept;
    bool locate(std::int64_t samplePosition) noexcept;

    bool setTrackGain(int track, float gain) noexcept;  // linear, 1.0 = 0 dB
    bool setTrackPan(int track, float pan) noexcept;    // -1 (links) .. +1 (rechts)
    bool setTrackMute(int track, bool mute) noexcept;
    bool setTrackSolo(int track, bool solo) noexcept;
    bool setMasterGain(float gain) noexcept;

    // Platzhalter-Quelle bis Schritt 3 (Audio-Clips). 0 Hz schaltet den Ton aus.
    bool setTrackTestTone(int track, float hz) noexcept;

    // Ersetzt die Audio-Clips einer Spur. Wirkt, sobald der Disk-Thread sie uebernimmt.
    bool setTrackPlaylist(int track, Playlist playlist);

    // Anzahl der Bloecke, in denen einer Spur Daten von der Platte fehlten.
    std::uint32_t underruns() const noexcept { return underruns_.load(std::memory_order_relaxed); }

    std::int64_t position() const noexcept { return positionOut_.load(std::memory_order_relaxed); }
    bool isPlaying() const noexcept { return playingOut_.load(std::memory_order_relaxed); }

    // Liefert den hoechsten Pegel seit dem letzten Aufruf und setzt ihn zurueck.
    float takeTrackPeak(int track) noexcept;
    float takeMasterPeak() noexcept;

    // --- Disk-Thread -----------------------------------------------------
    // Liest Audiodaten fuer alle Spuren vor. true, wenn etwas zu tun war.
    // Wird von DiskThread in einer Schleife gerufen, in Tests direkt.
    bool serviceDisk();

    // --- Audio-Thread ----------------------------------------------------
    void prepare(double sampleRate, int numChannels, int maxFrames) override;
    void process(float* const* out, int numChannels, int numFrames) noexcept override;

private:
    enum class CommandType : std::uint8_t {
        Play, Stop, Locate,
        TrackGain, TrackPan, TrackMute, TrackSolo, TrackTestTone,
        MasterGain,
    };

    struct Command {
        CommandType type{};
        std::int32_t track = 0;
        float value = 0.0f;
        std::int64_t position = 0;
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

    // Zustand des Audio-Threads
    double sampleRate_ = 48000.0;
    int maxFrames_ = 0;
    std::array<Track, kNumTracks> tracks_{};
    float masterGain_ = 1.0f;
    float masterCur_ = 0.0f;
    bool wantPlay_ = false;  // Wunsch des Benutzers
    bool rolling_ = false;   // Transport laeuft (auch noch waehrend des Ausblendens)
    std::int64_t position_ = 0;
    std::vector<float> masterL_, masterR_;
    std::vector<float> trackL_, trackR_;
    std::array<TrackStream, kNumTracks> streams_;

    // Rueckkanal zur UI
    std::atomic<std::int64_t> positionOut_{0};
    std::atomic<bool> playingOut_{false};
    std::array<std::atomic<float>, kNumTracks> trackPeak_{};
    std::atomic<float> masterPeak_{0.0f};
    std::atomic<std::uint32_t> underruns_{0};
};
