#pragma once

#include <algorithm>
#include <cstddef>
#include <vector>

// Feste Verzoegerung in Stereo fuer den Latenzausgleich. Der Speicher wird in
// prepare() angelegt; process() fordert nichts an und wartet nie.
class DelayLine {
public:
    static constexpr int kCapacity = 1 << 15;  // groesste Verzoegerung: 32767 Frames

    void prepare() {
        buffer_.assign(2 * static_cast<std::size_t>(kCapacity), 0.0f);
        pos_ = 0;
    }

    int delay() const noexcept { return delay_; }

    // Neue Verzoegerung. Der alte Inhalt wird verworfen (Audio-Thread, selten).
    void setDelay(int frames) noexcept {
        frames = std::clamp(frames, 0, kCapacity - 1);
        if (frames == delay_) return;
        delay_ = frames;
        std::fill(buffer_.begin(), buffer_.end(), 0.0f);
    }

    void process(float* left, float* right, int numFrames) noexcept {
        if (delay_ == 0 || buffer_.empty()) return;
        constexpr std::size_t kMask = static_cast<std::size_t>(kCapacity) - 1;
        for (int i = 0; i < numFrames; ++i) {
            const std::size_t w = (pos_ & kMask) * 2;
            const std::size_t r = ((pos_ - static_cast<std::size_t>(delay_)) & kMask) * 2;
            buffer_[w] = left[i];
            buffer_[w + 1] = right[i];
            left[i] = buffer_[r];
            right[i] = buffer_[r + 1];
            ++pos_;
        }
    }

private:
    std::vector<float> buffer_;
    std::size_t pos_ = 0;
    int delay_ = 0;
};
