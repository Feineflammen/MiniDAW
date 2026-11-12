#pragma once

#include "disk/WavFile.h"

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <string>
#include <vector>

// Ziel einer laufenden Aufnahme. Verbindet drei Threads:
//
//   Audio-Thread  push(), finish()   legt Eingangsdaten in einen Ringpuffer
//   Disk-Thread   service()          schreibt den Ringpuffer in die WAV-Datei
//   UI-Thread     done(), ...        erfaehrt, wann die Datei fertig ist
class RecordSink {
public:
    static constexpr std::size_t kCapacity = std::size_t{1} << 17;  // Frames im Ringpuffer

    static std::shared_ptr<RecordSink> create(const std::filesystem::path& path, int channels,
                                              int sampleRate, std::string& error);
    ~RecordSink();

    // --- Audio-Thread ---
    // position ist die Zeitachsen-Position des ersten Frames dieses Blocks.
    // Bei einem Kanal wird nur left geschrieben.
    void push(std::int64_t position, const float* left, const float* right, int frames) noexcept;
    void finish() noexcept { finished_.store(true, std::memory_order_release); }

    // --- Disk-Thread ---
    bool service();

    // --- beliebiger Thread ---
    bool started() const noexcept { return started_.load(std::memory_order_acquire); }
    bool done() const noexcept { return done_.load(std::memory_order_acquire); }
    std::int64_t startPosition() const noexcept { return startPos_.load(std::memory_order_relaxed); }
    std::int64_t framesCaptured() const noexcept { return captured_.load(std::memory_order_relaxed); }
    std::uint32_t overruns() const noexcept { return overruns_.load(std::memory_order_relaxed); }
    const std::filesystem::path& path() const noexcept { return path_; }
    int channels() const noexcept { return channels_; }

private:
    RecordSink() = default;

    static constexpr std::size_t kMask = kCapacity - 1;

    std::filesystem::path path_;
    int channels_ = 2;
    WavWriter writer_;            // nur Disk-Thread
    std::vector<float> chunk_;    // nur Disk-Thread
    std::vector<float> ring_;     // verschachtelt, channels_ Werte je Frame
    std::atomic<std::size_t> write_{0};
    std::atomic<std::size_t> read_{0};
    std::atomic<bool> started_{false};
    std::atomic<bool> finished_{false};
    std::atomic<bool> done_{false};
    std::atomic<std::int64_t> startPos_{0};
    std::atomic<std::int64_t> captured_{0};
    std::atomic<std::uint32_t> overruns_{0};
};
