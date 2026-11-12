#include "disk/PeakData.h"

#include <algorithm>
#include <utility>

void buildPeaks(WavFile& wav, PeakData& peaks, const std::atomic<bool>& cancel) {
    constexpr int kBucketsPerRead = 64;
    std::vector<float> buffer(static_cast<std::size_t>(PeakData::kBucket) * kBucketsPerRead * 2);
    const std::int64_t frames = wav.frames();

    for (std::size_t b = 0; b < peaks.count; b += kBucketsPerRead) {
        if (cancel.load(std::memory_order_relaxed)) return;

        const std::size_t buckets = std::min<std::size_t>(kBucketsPerRead, peaks.count - b);
        const std::int64_t first = static_cast<std::int64_t>(b) * PeakData::kBucket;
        wav.readStereo(first, static_cast<int>(buckets) * PeakData::kBucket, buffer.data());

        for (std::size_t i = 0; i < buckets; ++i) {
            const std::int64_t bucketStart = first + static_cast<std::int64_t>(i) * PeakData::kBucket;
            const int valid = static_cast<int>(
                std::min<std::int64_t>(PeakData::kBucket, frames - bucketStart));
            const float* p = buffer.data() + i * PeakData::kBucket * 2;
            float lo = 0.0f, hi = 0.0f;
            for (int k = 0; k < 2 * valid; ++k) {
                lo = std::min(lo, p[k]);
                hi = std::max(hi, p[k]);
            }
            peaks.lo[b + i] = lo;
            peaks.hi[b + i] = hi;
        }
        peaks.done.store(b + buckets, std::memory_order_release);
    }
}

PeakBuilder::PeakBuilder() : thread_([this] { run(); }) {}

PeakBuilder::~PeakBuilder() {
    {
        std::lock_guard<std::mutex> lock(mutex_);
        quit_.store(true);
    }
    wake_.notify_all();
    if (thread_.joinable()) thread_.join();
}

void PeakBuilder::add(std::shared_ptr<WavFile> wav, std::shared_ptr<PeakData> peaks) {
    pending_.fetch_add(1, std::memory_order_release);
    {
        std::lock_guard<std::mutex> lock(mutex_);
        jobs_.push_back({std::move(wav), std::move(peaks)});
    }
    wake_.notify_one();
}

void PeakBuilder::run() {
    for (;;) {
        Job job;
        {
            std::unique_lock<std::mutex> lock(mutex_);
            wake_.wait(lock, [this] { return quit_.load() || !jobs_.empty(); });
            if (quit_.load()) return;
            job = std::move(jobs_.front());
            jobs_.pop_front();
        }
        buildPeaks(*job.wav, *job.peaks, quit_);
        pending_.fetch_sub(1, std::memory_order_release);
    }
}
