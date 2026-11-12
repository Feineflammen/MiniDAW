#include "disk/RecordSink.h"

#include <algorithm>

namespace {
constexpr std::size_t kWriteChunk = 8192;  // Frames pro Schreibvorgang
}

std::shared_ptr<RecordSink> RecordSink::create(const std::filesystem::path& path, int channels,
                                               int sampleRate, std::string& error) {
    std::shared_ptr<RecordSink> sink(new RecordSink());
    sink->path_ = path;
    sink->channels_ = std::clamp(channels, 1, 2);
    if (!sink->writer_.open(path, sink->channels_, sampleRate, error)) return nullptr;
    sink->ring_.assign(kCapacity * static_cast<std::size_t>(sink->channels_), 0.0f);
    sink->chunk_.assign(kWriteChunk * static_cast<std::size_t>(sink->channels_), 0.0f);
    return sink;
}

RecordSink::~RecordSink() {
    // Falls die Aufnahme nie sauber beendet wurde: wenigstens eine gueltige Datei hinterlassen.
    if (writer_.isOpen()) writer_.finish();
}

void RecordSink::push(std::int64_t position, const float* left, const float* right, int frames) noexcept {
    if (!started_.load(std::memory_order_relaxed)) {
        startPos_.store(position, std::memory_order_relaxed);
        started_.store(true, std::memory_order_release);
    }

    const std::size_t w = write_.load(std::memory_order_relaxed);
    const std::size_t space = kCapacity - (w - read_.load(std::memory_order_acquire));
    auto n = static_cast<std::size_t>(frames);
    if (n > space) {
        // Der Disk-Thread kommt nicht nach: Der Rest dieses Blocks geht verloren.
        n = space;
        overruns_.fetch_add(1, std::memory_order_relaxed);
    }

    if (channels_ == 1) {
        for (std::size_t i = 0; i < n; ++i) ring_[(w + i) & kMask] = left[i];
    } else {
        for (std::size_t i = 0; i < n; ++i) {
            const std::size_t idx = ((w + i) & kMask) * 2;
            ring_[idx] = left[i];
            ring_[idx + 1] = right[i];
        }
    }
    write_.store(w + n, std::memory_order_release);
    captured_.fetch_add(static_cast<std::int64_t>(n), std::memory_order_relaxed);
}

bool RecordSink::service() {
    if (done_.load(std::memory_order_relaxed)) return false;

    // Reihenfolge wichtig: erst finished_, dann write_. Ist finished_ gesetzt,
    // kommt danach nichts mehr, und write_ ist der endgueltige Stand.
    const bool finished = finished_.load(std::memory_order_acquire);
    const std::size_t r = read_.load(std::memory_order_relaxed);
    const std::size_t avail = write_.load(std::memory_order_acquire) - r;

    if (avail > 0) {
        const std::size_t n = std::min(avail, kWriteChunk);
        const auto ch = static_cast<std::size_t>(channels_);
        for (std::size_t i = 0; i < n; ++i) {
            const std::size_t idx = ((r + i) & kMask) * ch;
            for (std::size_t c = 0; c < ch; ++c) chunk_[i * ch + c] = ring_[idx + c];
        }
        writer_.write(chunk_.data(), static_cast<std::int64_t>(n));
        read_.store(r + n, std::memory_order_release);
        return true;
    }

    if (finished) {
        writer_.finish();
        done_.store(true, std::memory_order_release);
        return true;
    }
    return false;
}
