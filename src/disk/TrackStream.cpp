#include "disk/TrackStream.h"

#include <algorithm>
#include <cstring>
#include <utility>

TrackStream::TrackStream()
    : ring_(kCapacity * 2, 0.0f),
      chunk_(static_cast<std::size_t>(kChunk) * 2, 0.0f),
      temp_(static_cast<std::size_t>(kChunk) * 2, 0.0f) {}

// --- UI-Thread ---------------------------------------------------------------

void TrackStream::setPlaylist(Playlist playlist) {
    std::lock_guard<std::mutex> lock(mutex_);
    pending_ = std::move(playlist);
    hasPending_ = true;
}

// --- Disk-Thread -------------------------------------------------------------

bool TrackStream::service() {
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (hasPending_) {
            playlist_ = std::move(pending_);
            pending_.clear();
            hasPending_ = false;
            playlistRestart_ = true;
            hasClips_.store(!playlist_.empty(), std::memory_order_relaxed);
        }
    }

    const std::uint32_t seekGen = seekGen_.load(std::memory_order_acquire);
    const bool seekRequested = seekGen != seekDoneDisk_;

    if (seekRequested || playlistRestart_) {
        // Erst neu ansetzen, wenn der Audio-Thread den letzten Flush uebernommen hat.
        if (flushGenDisk_ != flushAck_.load(std::memory_order_acquire)) return false;

        diskPos_ = seekRequested ? seekPos_.load(std::memory_order_relaxed)
                                 : audioPos_.load(std::memory_order_relaxed);
        flushTo_.store(write_.load(std::memory_order_relaxed), std::memory_order_relaxed);
        flushPos_.store(diskPos_, std::memory_order_relaxed);
        flushGen_.store(++flushGenDisk_, std::memory_order_release);

        if (seekRequested) {
            seekDoneDisk_ = seekGen;
            seekDone_.store(seekGen, std::memory_order_release);
        }
        playlistRestart_ = false;
    }

    if (playlist_.empty()) return false;

    const std::size_t w = write_.load(std::memory_order_relaxed);
    const std::size_t used = w - read_.load(std::memory_order_acquire);
    if (kCapacity - used < static_cast<std::size_t>(kChunk)) return false;

    renderChunk();

    for (int i = 0; i < kChunk; ++i) {
        const std::size_t idx = (w + static_cast<std::size_t>(i)) & kMask;
        ring_[2 * idx] = chunk_[2 * static_cast<std::size_t>(i)];
        ring_[2 * idx + 1] = chunk_[2 * static_cast<std::size_t>(i) + 1];
    }
    write_.store(w + static_cast<std::size_t>(kChunk), std::memory_order_release);
    diskPos_ += kChunk;
    return true;
}

// Mischt alle Clips, die [diskPos_, diskPos_ + kChunk) beruehren, nach chunk_.
void TrackStream::renderChunk() {
    std::fill(chunk_.begin(), chunk_.end(), 0.0f);
    const std::int64_t blockStart = diskPos_;
    const std::int64_t blockEnd = diskPos_ + kChunk;

    for (const Clip& clip : playlist_) {
        if (!clip.source || clip.length <= 0) continue;
        const std::int64_t a = std::max(blockStart, clip.start);
        const std::int64_t b = std::min(blockEnd, clip.start + clip.length);
        if (a >= b) continue;

        const int count = static_cast<int>(b - a);
        clip.source->readStereo(clip.sourceOffset + (a - clip.start), count, temp_.data());

        float* dst = chunk_.data() + 2 * static_cast<std::size_t>(a - blockStart);
        for (int i = 0; i < 2 * count; ++i) dst[i] += clip.gain * temp_[static_cast<std::size_t>(i)];
    }
}

// --- Audio-Thread ------------------------------------------------------------

// Bringt den Lesezeiger auf Position pos. true, wenn der naechste Frame im
// Puffer genau zu pos gehoert.
bool TrackStream::sync(std::int64_t pos) noexcept {
    audioPos_.store(pos, std::memory_order_relaxed);

    // Hat der Disk-Thread neu angesetzt? Dann alten Pufferinhalt verwerfen.
    const std::uint32_t flushGen = flushGen_.load(std::memory_order_acquire);
    if (flushGen != flushSeenAudio_) {
        read_.store(flushTo_.load(std::memory_order_relaxed), std::memory_order_release);
        nextPos_ = flushPos_.load(std::memory_order_relaxed);
        flushSeenAudio_ = flushGen;
        flushAck_.store(flushGen, std::memory_order_release);
    }

    if (!hasClips_.load(std::memory_order_relaxed)) return false;

    std::int64_t gap = pos - nextPos_;
    if (gap == 0) return true;

    // Der Puffer haengt etwas hinterher (nach einem Aussetzer oder einem
    // Neuansatz): ueberholte Frames wegwerfen, bis er wieder passt.
    if (gap > 0 && gap <= static_cast<std::int64_t>(kCapacity)) {
        const std::size_t r = read_.load(std::memory_order_relaxed);
        const std::size_t avail = write_.load(std::memory_order_acquire) - r;
        const std::size_t drop = std::min(static_cast<std::size_t>(gap), avail);
        read_.store(r + drop, std::memory_order_release);
        nextPos_ += static_cast<std::int64_t>(drop);
        return nextPos_ == pos;  // sonst auf Nachschub warten
    }

    // Echter Sprung: beim Disk-Thread anfordern, falls nicht schon einer unterwegs ist.
    if (seekGenAudio_ == seekDone_.load(std::memory_order_acquire)) {
        seekPos_.store(pos, std::memory_order_relaxed);
        seekGen_.store(++seekGenAudio_, std::memory_order_release);
    }
    return false;
}

int TrackStream::read(std::int64_t pos, float* left, float* right, int n) noexcept {
    int got = 0;
    if (sync(pos)) {
        const std::size_t r = read_.load(std::memory_order_relaxed);
        const std::size_t avail = write_.load(std::memory_order_acquire) - r;
        got = static_cast<int>(std::min(static_cast<std::size_t>(n), avail));
        for (int i = 0; i < got; ++i) {
            const std::size_t idx = (r + static_cast<std::size_t>(i)) & kMask;
            left[i] = ring_[2 * idx];
            right[i] = ring_[2 * idx + 1];
        }
        read_.store(r + static_cast<std::size_t>(got), std::memory_order_release);
        nextPos_ += got;
    }
    if (got < n) {
        const std::size_t bytes = static_cast<std::size_t>(n - got) * sizeof(float);
        std::memset(left + got, 0, bytes);
        std::memset(right + got, 0, bytes);
    }
    return got;
}
