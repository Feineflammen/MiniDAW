#include "disk/Resampler.h"

#include "disk/WavFile.h"

#include <algorithm>
#include <cmath>
#include <numeric>

namespace {

constexpr double kPi = 3.141592653589793;
constexpr double kBeta = 10.0;             // Kaiser: etwa 100 dB Sperrdaempfung
constexpr int kHalfAtLowerRate = 80;       // halbe Laenge in Frames der kleineren Rate
constexpr double kCutoff = 0.96;           // Mitte des Uebergangs (0,92 .. 1,0 der Nyquist-Frequenz)
constexpr int kMaxPhases = 2048;

double besselI0(double x) {
    double sum = 1.0, term = 1.0;
    for (int k = 1; k < 64; ++k) {
        term *= (x / (2.0 * k)) * (x / (2.0 * k));
        sum += term;
        if (term < sum * 1e-17) break;
    }
    return sum;
}

}  // namespace

Resampler::Resampler(int inRate, int outRate, int channels) : channels_(std::max(1, channels)) {
    inRate = std::max(1, inRate);
    outRate = std::max(1, outRate);
    const std::int64_t g = std::gcd(static_cast<std::int64_t>(inRate), static_cast<std::int64_t>(outRate));
    up_ = outRate / g;
    down_ = inRate / g;
    exact_ = up_ <= kMaxPhases;
    phases_ = exact_ ? static_cast<int>(up_) : kMaxPhases;

    // Grenzfrequenz relativ zur Eingangsrate; beim Herunterrechnen enger.
    const double scale = std::min(1.0, static_cast<double>(up_) / static_cast<double>(down_));
    half_ = static_cast<int>(std::ceil(kHalfAtLowerRate / scale));
    const double fc = 0.5 * kCutoff * scale;  // Zyklen je Eingangsframe
    const int taps = 2 * half_;
    const double i0Beta = besselI0(kBeta);

    // Zeile p: Ausgang liegt um frac = p / phases_ hinter Eingangsframe i0.
    // Wert j gehoert zu Eingangsframe i0 - half_ + 1 + j, Abstand x = frac + half_ - 1 - j.
    table_.assign(static_cast<std::size_t>(phases_ + 1) * static_cast<std::size_t>(taps), 0.0f);
    std::vector<double> row(static_cast<std::size_t>(taps));
    for (int p = 0; p <= phases_; ++p) {
        const double frac = static_cast<double>(p) / phases_;
        double sum = 0.0;
        for (int j = 0; j < taps; ++j) {
            const double x = frac + half_ - 1 - j;
            const double r = x / half_;
            double h = 0.0;
            if (std::fabs(r) < 1.0) {
                const double arg = 2.0 * fc * x;
                const double sinc = std::fabs(arg) < 1e-12 ? 1.0 : std::sin(kPi * arg) / (kPi * arg);
                h = 2.0 * fc * sinc * besselI0(kBeta * std::sqrt(1.0 - r * r)) / i0Beta;
            }
            row[static_cast<std::size_t>(j)] = h;
            sum += h;
        }
        for (int j = 0; j < taps; ++j)  // Gleichanteil genau 1
            table_[static_cast<std::size_t>(p) * taps + j] = static_cast<float>(row[static_cast<std::size_t>(j)] / sum);
    }

    // Vor dem ersten Frame liegt Stille.
    buffer_.assign(static_cast<std::size_t>(half_) * channels_, 0.0f);
    bufferStart_ = -half_;
}

std::int64_t Resampler::outputFrames(std::int64_t inFrames, int inRate, int outRate) noexcept {
    if (inFrames <= 0 || inRate <= 0 || outRate <= 0) return 0;
    const std::int64_t g = std::gcd(static_cast<std::int64_t>(inRate), static_cast<std::int64_t>(outRate));
    const std::int64_t up = outRate / g, down = inRate / g;
    // ceil(inFrames * up / down) ohne Ueberlauf fuer realistische Laengen
    return (inFrames / down) * up + ((inFrames % down) * up + down - 1) / down;
}

std::int64_t Resampler::convertPosition(std::int64_t frames, double fromRate, double toRate) noexcept {
    if (fromRate <= 0.0 || toRate <= 0.0 || fromRate == toRate) return frames;
    return static_cast<std::int64_t>(std::llround(static_cast<double>(frames) * toRate / fromRate));
}

void Resampler::produce(std::int64_t available, std::int64_t limit, std::vector<float>& out) {
    const int taps = 2 * half_;
    const auto ch = static_cast<std::size_t>(channels_);
    std::vector<double> acc(ch);
    while (produced_ < limit) {
        // Zeit des Ausgangsframes in Eingangsframes: produced_ * M / L
        const std::int64_t num = produced_ * down_;
        const std::int64_t i0 = num / up_;
        const std::int64_t rem = num % up_;
        if (i0 + half_ >= available) break;  // noch nicht genug Eingang

        const float* row;
        if (exact_) {
            row = table_.data() + static_cast<std::size_t>(rem) * taps;
        } else {
            const double pos = static_cast<double>(rem) / static_cast<double>(up_) * phases_;
            const int p = std::min(phases_ - 1, static_cast<int>(pos));
            const float t = static_cast<float>(pos - p);
            mixed_.resize(static_cast<std::size_t>(taps));
            const float* a = table_.data() + static_cast<std::size_t>(p) * taps;
            const float* b = a + taps;
            for (int j = 0; j < taps; ++j) mixed_[static_cast<std::size_t>(j)] = a[j] + (b[j] - a[j]) * t;
            row = mixed_.data();
        }

        const std::int64_t first = i0 - half_ + 1;
        const float* src = buffer_.data() + static_cast<std::size_t>(first - bufferStart_) * ch;
        std::fill(acc.begin(), acc.end(), 0.0);
        for (int j = 0; j < taps; ++j) {
            const double w = row[j];
            for (std::size_t c = 0; c < ch; ++c) acc[c] += w * src[static_cast<std::size_t>(j) * ch + c];
        }
        for (std::size_t c = 0; c < ch; ++c) out.push_back(static_cast<float>(acc[c]));
        ++produced_;
    }

    // Nicht mehr gebrauchten Anfang des Puffers abwerfen.
    const std::int64_t needFrom = (produced_ * down_) / up_ - half_ + 1;
    if (needFrom > bufferStart_ + 65536) {
        const auto drop = static_cast<std::size_t>(needFrom - bufferStart_) * ch;
        buffer_.erase(buffer_.begin(), buffer_.begin() + static_cast<std::ptrdiff_t>(drop));
        bufferStart_ = needFrom;
    }
}

void Resampler::push(const float* in, std::int64_t frames, std::vector<float>& out) {
    if (frames <= 0) return;
    buffer_.insert(buffer_.end(), in, in + frames * channels_);
    received_ += frames;
    produce(received_, INT64_MAX, out);
}

void Resampler::finish(std::vector<float>& out) {
    const std::int64_t total = (received_ / down_) * up_ + ((received_ % down_) * up_ + down_ - 1) / down_;
    // Nach dem letzten Frame folgt Stille.
    buffer_.insert(buffer_.end(), static_cast<std::size_t>(half_ + 1) * channels_, 0.0f);
    produce(received_ + half_ + 1, total, out);
}

bool resampleWavFile(const std::filesystem::path& source, const std::filesystem::path& target, int rate,
                     std::string& error, const std::function<bool(double)>& progress) {
    auto wav = WavFile::open(source, error);
    if (!wav) return false;
    const int channels = std::min(2, wav->channels());
    Resampler resampler(wav->sampleRate(), rate, channels);

    const auto temp = std::filesystem::path(target).concat(L".teil");
    WavWriter writer;
    if (!writer.open(temp, channels, rate, error)) return false;

    constexpr int kBlock = 65536;
    std::vector<float> stereo(2 * static_cast<std::size_t>(kBlock));
    std::vector<float> in, out;
    bool ok = true;
    for (std::int64_t pos = 0; pos < wav->frames() && ok; pos += kBlock) {
        const int n = static_cast<int>(std::min<std::int64_t>(kBlock, wav->frames() - pos));
        wav->readStereo(pos, n, stereo.data());
        const float* block = stereo.data();
        if (channels == 1) {
            in.resize(static_cast<std::size_t>(n));
            for (int i = 0; i < n; ++i) in[static_cast<std::size_t>(i)] = stereo[2 * static_cast<std::size_t>(i)];
            block = in.data();
        }
        out.clear();
        resampler.push(block, n, out);
        ok = writer.write(out.data(), static_cast<std::int64_t>(out.size()) / channels);
        if (ok && progress && !progress(static_cast<double>(pos + n) / static_cast<double>(wav->frames()))) {
            error = "Abgebrochen";
            ok = false;
        }
    }
    if (ok) {
        out.clear();
        resampler.finish(out);
        ok = writer.write(out.data(), static_cast<std::int64_t>(out.size()) / channels) && writer.finish();
        if (!ok && error.empty()) error = "Schreibfehler";
    } else {
        writer.finish();
        if (error.empty()) error = "Schreibfehler";
    }
    std::error_code ec;
    if (!ok) {
        std::filesystem::remove(temp, ec);
        return false;
    }
    std::filesystem::rename(temp, target, ec);
    if (ec) {
        std::filesystem::remove(temp, ec);
        error = "Umgerechnete Datei laesst sich nicht ablegen";
        return false;
    }
    return true;
}
