#pragma once

#include "disk/WavFile.h"

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <mutex>
#include <vector>

// Ein Audio-Clip auf der Zeitachse. Alle Angaben in Frames.
struct Clip {
    std::shared_ptr<WavFile> source;
    std::int64_t start = 0;         // Position auf der Zeitachse
    std::int64_t sourceOffset = 0;  // erster benutzter Frame in der Datei
    std::int64_t length = 0;
    float gain = 1.0f;
    std::int64_t fadeIn = 0;   // Einblendung am Anfang, in Frames (0 = keine)
    std::int64_t fadeOut = 0;  // Ausblendung am Ende
};

using Playlist = std::vector<Clip>;

// Wiedergabestrom einer Spur. Verbindet drei Threads:
//
//   UI-Thread    setPlaylist()   uebergibt die Clips der Spur
//   Disk-Thread  service()       liest die Dateien vor und fuellt den Ringpuffer
//   Audio-Thread prime(), read() holt fertige Frames, ohne je zu warten
//
// Der Audio-Thread sieht nie Clips oder Dateien, nur fertig gemischtes Stereo.
// Der Strom liefert die Zeitachse fortlaufend: Zwischen den Clips kommt Stille.
//
// Sprung und Clip-Aenderung laufen ueber denselben Weg: Der Disk-Thread setzt
// neu an einer Position an und meldet dem Audio-Thread, bis wohin der alte
// Pufferinhalt zu verwerfen ist ("Flush"). Solange keine passenden Daten da
// sind, liefert read() Stille und holt danach samplegenau wieder auf.
class TrackStream {
public:
    static constexpr std::size_t kCapacity = std::size_t{1} << 16;  // Frames im Ringpuffer
    static constexpr int kChunk = 4096;                             // Frames pro Lesevorgang

    TrackStream();

    // --- UI-Thread ---
    void setPlaylist(Playlist playlist);

    // --- Disk-Thread ---
    // Erledigt hoechstens einen Lesevorgang. true, wenn etwas zu tun war.
    bool service();

    // --- Audio-Thread ---
    bool active() const noexcept { return hasClips_.load(std::memory_order_relaxed); }

    // Bereitet die Wiedergabe ab pos vor, ohne Daten zu entnehmen (bei stehendem Transport).
    void prime(std::int64_t pos) noexcept { sync(pos); }

    // Liefert n Frames ab Zeitachsen-Position pos. Fehlende Daten werden Stille.
    // Rueckgabe: Anzahl der Frames mit echten Daten.
    int read(std::int64_t pos, float* left, float* right, int n) noexcept;

private:
    bool sync(std::int64_t pos) noexcept;
    void renderChunk();

    static constexpr std::size_t kMask = kCapacity - 1;

    std::vector<float> ring_;  // L, R verschachtelt
    std::atomic<std::size_t> write_{0};  // schreibt nur der Disk-Thread
    std::atomic<std::size_t> read_{0};   // schreibt nur der Audio-Thread

    // Sprunganforderung: Audio -> Disk
    std::atomic<std::int64_t> seekPos_{0};
    std::atomic<std::uint32_t> seekGen_{0};
    std::atomic<std::uint32_t> seekDone_{0};  // Disk -> Audio: bis hierhin erledigt

    // Flush: Disk -> Audio
    std::atomic<std::size_t> flushTo_{0};
    std::atomic<std::int64_t> flushPos_{0};
    std::atomic<std::uint32_t> flushGen_{0};
    std::atomic<std::uint32_t> flushAck_{0};  // Audio -> Disk: uebernommen

    std::atomic<std::int64_t> audioPos_{0};  // letzte Position des Audio-Threads
    std::atomic<bool> hasClips_{false};

    // Nur Audio-Thread
    std::int64_t nextPos_ = 0;  // Zeitachsen-Position des naechsten Frames im Puffer
    std::uint32_t seekGenAudio_ = 0;
    std::uint32_t flushSeenAudio_ = 0;

    // Nur Disk-Thread
    Playlist playlist_;
    std::int64_t diskPos_ = 0;
    std::uint32_t seekDoneDisk_ = 0;
    std::uint32_t flushGenDisk_ = 0;
    bool playlistRestart_ = false;
    std::vector<float> chunk_;
    std::vector<float> temp_;

    // Uebergabe UI -> Disk (beide duerfen warten)
    std::mutex mutex_;
    Playlist pending_;
    bool hasPending_ = false;
};
