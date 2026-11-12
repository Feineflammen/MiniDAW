#pragma once

#include "disk/WavFile.h"

#include <atomic>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <memory>
#include <mutex>
#include <thread>
#include <vector>

// Uebersichtsdaten fuer die Wellenform-Anzeige: kleinster und groesster Wert
// je Abschnitt von kBucket Frames (beide Kanaele zusammengefasst).
//
// Wird von einem Hintergrund-Thread gefuellt. Die Oberflaeche darf jederzeit
// die ersten done Eintraege lesen und sieht die Wellenform so nach und nach.
struct PeakData {
    static constexpr int kBucket = 256;

    explicit PeakData(std::int64_t frames)
        : count(static_cast<std::size_t>((frames + kBucket - 1) / kBucket)), lo(count, 0.0f), hi(count, 0.0f) {}

    bool complete() const noexcept { return done.load(std::memory_order_acquire) == count; }

    const std::size_t count;
    std::vector<float> lo, hi;
    std::atomic<std::size_t> done{0};
};

// Berechnet die Uebersichtsdaten einer Datei. Bricht ab, sobald cancel gesetzt ist.
void buildPeaks(WavFile& wav, PeakData& peaks, const std::atomic<bool>& cancel);

// Arbeitet Auftraege zum Berechnen von Uebersichtsdaten in einem eigenen Thread ab.
class PeakBuilder {
public:
    PeakBuilder();
    ~PeakBuilder();
    PeakBuilder(const PeakBuilder&) = delete;
    PeakBuilder& operator=(const PeakBuilder&) = delete;

    void add(std::shared_ptr<WavFile> wav, std::shared_ptr<PeakData> peaks);
    bool idle() const noexcept { return pending_.load(std::memory_order_acquire) == 0; }

private:
    struct Job {
        std::shared_ptr<WavFile> wav;
        std::shared_ptr<PeakData> peaks;
    };

    void run();

    std::mutex mutex_;
    std::condition_variable wake_;
    std::deque<Job> jobs_;
    std::atomic<bool> quit_{false};
    std::atomic<int> pending_{0};
    std::thread thread_;
};
