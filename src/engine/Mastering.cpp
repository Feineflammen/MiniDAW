#include "engine/Mastering.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <limits>
#include <string>

namespace mastering {

namespace {

constexpr double kPi = 3.14159265358979323846;

double dbToGain(double db) { return std::pow(10.0, db / 20.0); }

double lufs(double meanSquare) {
    return meanSquare > 0.0 ? -0.691 + 10.0 * std::log10(meanSquare) : kSilence;
}

double besselI0(double x) {
    double sum = 1.0, term = 1.0;
    for (int k = 1; k < 50; ++k) {
        term *= (x / (2.0 * k)) * (x / (2.0 * k));
        sum += term;
        if (term < 1e-17 * sum) break;
    }
    return sum;
}

double kaiser(double t, double beta) {  // t in -1..1
    const double inner = 1.0 - t * t;
    return inner <= 0.0 ? 0.0 : besselI0(beta * std::sqrt(inner)) / besselI0(beta);
}

double sinc(double x) { return std::abs(x) < 1e-12 ? 1.0 : std::sin(kPi * x) / (kPi * x); }

// Phasen 1/8 .. 7/8 des True-Peak-Interpolators, je 16 Taps. Einmal berechnet.
using PeakTaps = std::array<std::array<double, 2 * TruePeak::kHalf>, 7>;
const PeakTaps& peakTaps() {
    static const PeakTaps taps = [] {
        PeakTaps t{};
        constexpr int half = TruePeak::kHalf;
        for (int phase = 1; phase <= 7; ++phase) {
            const double f = phase / 8.0;
            double sum = 0.0;
            for (int k = 0; k < 2 * half; ++k) {
                const double d = (k - (half - 1)) - f;  // Abstand des Samples c+(k-7) zum Punkt c+f
                const double v = sinc(d) * kaiser(d / half, 6.0);
                t[static_cast<std::size_t>(phase - 1)][static_cast<std::size_t>(k)] = v;
                sum += v;
            }
            for (double& v : t[static_cast<std::size_t>(phase - 1)]) v /= sum;
        }
        return t;
    }();
    return taps;
}

void killDenormal(double& v) {
    if (std::abs(v) < 1e-30) v = 0.0;
}

double sanitize(double v) { return std::isfinite(v) ? std::clamp(v, -64.0, 64.0) : 0.0; }

}  // namespace

float toPosition(int param, float value) noexcept {
    const ParamInfo& p = kParams[static_cast<std::size_t>(param)];
    if (p.toggle) return value >= 0.5f ? 1.0f : 0.0f;
    value = std::clamp(value, p.min, p.max);
    if (p.logScale) return static_cast<float>(std::log(value / p.min) / std::log(p.max / p.min));
    return (value - p.min) / (p.max - p.min);
}

float fromPosition(int param, float position) noexcept {
    const ParamInfo& p = kParams[static_cast<std::size_t>(param)];
    position = std::clamp(position, 0.0f, 1.0f);
    if (p.toggle) return position >= 0.5f ? 1.0f : 0.0f;
    if (p.logScale) return static_cast<float>(p.min * std::pow(p.max / p.min, static_cast<double>(position)));
    return p.min + (p.max - p.min) * position;
}

// --- LoudnessMeter -------------------------------------------------------------

void LoudnessMeter::prepare(double sampleRate) {
    // K-Filter fuer jede Abtastrate aus dem Analog-Vorbild (wie libebur128);
    // bei 48 kHz ergeben sich genau die Koeffizienten aus BS.1770.
    double f0 = 1681.974450955533, gainDb = 3.999843853973347, q = 0.7071752369554196;
    double k = std::tan(kPi * f0 / sampleRate);
    const double vh = std::pow(10.0, gainDb / 20.0);
    const double vb = std::pow(vh, 0.4996667741545416);
    double a0 = 1.0 + k / q + k * k;
    shelf_.b0 = (vh + vb * k / q + k * k) / a0;
    shelf_.b1 = 2.0 * (k * k - vh) / a0;
    shelf_.b2 = (vh - vb * k / q + k * k) / a0;
    shelf_.a1 = 2.0 * (k * k - 1.0) / a0;
    shelf_.a2 = (1.0 - k / q + k * k) / a0;

    f0 = 38.13547087602444;
    q = 0.5003270373238773;
    k = std::tan(kPi * f0 / sampleRate);
    a0 = 1.0 + k / q + k * k;
    highpass_.b0 = 1.0;
    highpass_.b1 = -2.0;
    highpass_.b2 = 1.0;
    highpass_.a1 = 2.0 * (k * k - 1.0) / a0;
    highpass_.a2 = (1.0 - k / q + k * k) / a0;

    subBlockFrames_ = std::max(1, static_cast<int>(std::lround(sampleRate * 0.1)));
    binEnergy_.assign(kBins, 0.0);
    binCount_.assign(kBins, 0);
    reset();
}

void LoudnessMeter::reset() noexcept {
    for (Biquad* f : {&shelf_, &highpass_}) f->z1[0] = f->z1[1] = f->z2[0] = f->z2[1] = 0.0;
    subBlockCount_ = 0;
    subBlockSum_ = 0.0;
    subBlocks_.fill(0.0);
    subBlockIndex_ = 0;
    subBlocksSeen_ = 0;
    momentary_ = shortTerm_ = integrated_ = kSilence;
    std::fill(binEnergy_.begin(), binEnergy_.end(), 0.0);
    std::fill(binCount_.begin(), binCount_.end(), 0u);
    gatedEnergy_ = 0.0;
    gatedBlocks_ = 0;
}

void LoudnessMeter::process(double left, double right) noexcept {
    const double in[2]{left, right};
    double sum = 0.0;
    for (int ch = 0; ch < 2; ++ch) {
        double x = in[ch];
        for (Biquad* f : {&shelf_, &highpass_}) {
            const double y = f->b0 * x + f->z1[ch];
            f->z1[ch] = f->b1 * x - f->a1 * y + f->z2[ch];
            f->z2[ch] = f->b2 * x - f->a2 * y;
            x = y;
        }
        sum += x * x;  // Kanalgewicht 1 fuer links und rechts
    }
    subBlockSum_ += sum;
    if (++subBlockCount_ >= subBlockFrames_) finishSubBlock();
}

void LoudnessMeter::finishSubBlock() noexcept {
    subBlocks_[static_cast<std::size_t>(subBlockIndex_)] = subBlockSum_ / subBlockCount_;
    subBlockIndex_ = (subBlockIndex_ + 1) % static_cast<int>(subBlocks_.size());
    subBlockSum_ = 0.0;
    subBlockCount_ = 0;
    ++subBlocksSeen_;
    for (Biquad* f : {&shelf_, &highpass_})
        for (int ch = 0; ch < 2; ++ch) {
            killDenormal(f->z1[ch]);
            killDenormal(f->z2[ch]);
        }

    const auto mean = [&](int count) {
        double total = 0.0;
        for (int i = 1; i <= count; ++i)
            total += subBlocks_[static_cast<std::size_t>((subBlockIndex_ - i + 30) % 30)];
        return total / count;
    };
    if (subBlocksSeen_ >= 4) {
        // 400-ms-Block, alle 100 ms (75 % Ueberlappung): Momentan und Baustein fuer "integriert".
        const double block = mean(4);
        momentary_ = lufs(block);
        if (momentary_ > -70.0) {
            const int bin = std::clamp(static_cast<int>(std::floor((momentary_ + 70.0) * 100.0)), 0, kBins - 1);
            binEnergy_[static_cast<std::size_t>(bin)] += block;
            ++binCount_[static_cast<std::size_t>(bin)];
            gatedEnergy_ += block;
            ++gatedBlocks_;
            updateIntegrated();
        }
    }
    if (subBlocksSeen_ >= 30) shortTerm_ = lufs(mean(30));
}

void LoudnessMeter::updateIntegrated() noexcept {
    if (gatedBlocks_ == 0) {
        integrated_ = kSilence;
        return;
    }
    // Relatives Gate: 10 LU unter dem Mittel aller Bloecke ueber -70 LUFS.
    const double relative = lufs(gatedEnergy_ / static_cast<double>(gatedBlocks_)) - 10.0;
    const int first = std::clamp(static_cast<int>(std::ceil((relative + 70.0) * 100.0)), 0, kBins);
    double energy = 0.0;
    std::uint64_t count = 0;
    for (int b = first; b < kBins; ++b) {
        energy += binEnergy_[static_cast<std::size_t>(b)];
        count += binCount_[static_cast<std::size_t>(b)];
    }
    integrated_ = count > 0 ? lufs(energy / static_cast<double>(count)) : kSilence;
}

// --- TruePeak ------------------------------------------------------------------

TruePeak::TruePeak() { peakTaps(); }  // Tabelle hier anlegen, nicht im Audio-Thread

void TruePeak::reset() noexcept {
    history_.fill(0.0);
    pos_ = 0;
}

double TruePeak::push(double sample) noexcept {
    constexpr int n = 2 * kHalf;
    history_[static_cast<std::size_t>(pos_)] = sample;
    history_[static_cast<std::size_t>(pos_ + n)] = sample;
    pos_ = (pos_ + 1) % n;
    const double* w = history_.data() + pos_;  // w[0] aeltestes, w[n-1] neuestes Sample
    // c = neuestes - kHalf: w[kHalf-1] ist Sample c, w[kHalf] ist c+1.
    double peak = std::max(std::abs(w[kHalf - 1]), std::abs(w[kHalf]));
    for (const auto& taps : peakTaps()) {
        double v = 0.0;
        for (int k = 0; k < n; ++k) v += w[k] * taps[static_cast<std::size_t>(k)];
        peak = std::max(peak, std::abs(v));
    }
    return peak;
}

// --- Processor -----------------------------------------------------------------

double Processor::Biquad::step(int ch, double x) noexcept {
    const double y = b0 * x + z1[ch];
    z1[ch] = b1 * x - a1 * y + z2[ch];
    z2[ch] = b2 * x - a2 * y;
    return y;
}

void Processor::Biquad::flush() noexcept {
    for (int ch = 0; ch < 2; ++ch) {
        killDenormal(z1[ch]);
        killDenormal(z2[ch]);
    }
}

void Processor::Delay::setup(int delay) {
    length = std::max(1, delay + 1);
    for (auto& d : data) d.assign(static_cast<std::size_t>(length), 0.0);
    pos = 0;
}

void Processor::Delay::clear() noexcept {
    for (auto& d : data) std::fill(d.begin(), d.end(), 0.0);
    pos = 0;
}

void Processor::Delay::push(double& l, double& r) noexcept {
    data[0][static_cast<std::size_t>(pos)] = l;
    data[1][static_cast<std::size_t>(pos)] = r;
    pos = pos + 1 == length ? 0 : pos + 1;
    l = data[0][static_cast<std::size_t>(pos)];
    r = data[1][static_cast<std::size_t>(pos)];
}

namespace {

// RBJ-Kochbuch.
void designHighpass(double& b0, double& b1, double& b2, double& a1, double& a2, double fs, double hz) {
    const double w = 2.0 * kPi * std::clamp(hz, 5.0, fs * 0.45) / fs;
    const double c = std::cos(w), alpha = std::sin(w) / (2.0 * 0.7071067811865476), a0 = 1.0 + alpha;
    b0 = (1.0 + c) / (2.0 * a0);
    b1 = -(1.0 + c) / a0;
    b2 = b0;
    a1 = -2.0 * c / a0;
    a2 = (1.0 - alpha) / a0;
}

void designBell(double& b0, double& b1, double& b2, double& a1, double& a2, double fs, double hz, double q,
                double db) {
    const double w = 2.0 * kPi * std::clamp(hz, 5.0, fs * 0.45) / fs;
    const double c = std::cos(w), alpha = std::sin(w) / (2.0 * q), g = std::pow(10.0, db / 40.0);
    const double a0 = 1.0 + alpha / g;
    b0 = (1.0 + alpha * g) / a0;
    b1 = -2.0 * c / a0;
    b2 = (1.0 - alpha * g) / a0;
    a1 = b1;
    a2 = (1.0 - alpha / g) / a0;
}

void designShelf(double& b0, double& b1, double& b2, double& a1, double& a2, double fs, double hz, double db,
                 bool high) {
    const double A = std::pow(10.0, db / 40.0);
    const double w = 2.0 * kPi * std::clamp(hz, 5.0, fs * 0.45) / fs;
    const double c = std::cos(w), s = std::sin(w), beta = 2.0 * std::sqrt(A) * s * 0.7071067811865476;  // Steilheit S = 1
    double nb0, nb1, nb2, na0, na1, na2;
    if (high) {
        nb0 = A * ((A + 1) + (A - 1) * c + beta);
        nb1 = -2 * A * ((A - 1) + (A + 1) * c);
        nb2 = A * ((A + 1) + (A - 1) * c - beta);
        na0 = (A + 1) - (A - 1) * c + beta;
        na1 = 2 * ((A - 1) - (A + 1) * c);
        na2 = (A + 1) - (A - 1) * c - beta;
    } else {
        nb0 = A * ((A + 1) - (A - 1) * c + beta);
        nb1 = 2 * A * ((A - 1) - (A + 1) * c);
        nb2 = A * ((A + 1) - (A - 1) * c - beta);
        na0 = (A + 1) + (A - 1) * c + beta;
        na1 = -2 * ((A - 1) + (A + 1) * c);
        na2 = (A + 1) + (A - 1) * c - beta;
    }
    b0 = nb0 / na0;
    b1 = nb1 / na0;
    b2 = nb2 / na0;
    a1 = na1 / na0;
    a2 = na2 / na0;
}

}  // namespace

Processor::Processor() {
    for (int i = 0; i < kParamCount; ++i) target_[static_cast<std::size_t>(i)] = kParams[static_cast<std::size_t>(i)].def;
    prepare(48000.0);
}

void Processor::prepare(double sampleRate) {
    fs_ = std::clamp(std::isfinite(sampleRate) ? sampleRate : 48000.0, 8000.0, 384000.0);
    lookahead_ = std::max(1, static_cast<int>(std::lround(fs_ * 0.005)));
    latency_ = Oversampler::kDelay + TruePeak::kDelay + lookahead_ - 1;

    // Halbband-Tiefpass fuer die Ueberabtastung: Grenze bei der halben Rate der Grundrate.
    halfband_.assign(Oversampler::kTaps, 0.0);
    double sum = 0.0;
    for (int k = 0; k < Oversampler::kTaps; ++k) {
        const double d = k - Oversampler::kDelay;
        halfband_[static_cast<std::size_t>(k)] = 0.5 * sinc(0.5 * d) * kaiser(d / (Oversampler::kDelay + 1.0), 8.0);
        sum += halfband_[static_cast<std::size_t>(k)];
    }
    for (double& h : halfband_) h /= sum;

    satDry_.setup(Oversampler::kDelay);
    limiterDelay_.setup(TruePeak::kDelay + lookahead_ - 1);
    dryDelay_.setup(latency_);
    minValue_.assign(static_cast<std::size_t>(lookahead_ + 2), 1.0);
    minIndex_.assign(static_cast<std::size_t>(lookahead_ + 2), 0);
    box_.assign(static_cast<std::size_t>(lookahead_), 1.0);

    compAttack_ = std::exp(-1.0 / (fs_ * 0.025));
    compRelease_ = std::exp(-1.0 / (fs_ * 0.160));
    sideCoef_ = std::exp(-2.0 * kPi * 120.0 / fs_);
    outMeter_.prepare(fs_);
    inMeter_.prepare(fs_);
    reset();
}

void Processor::reset() noexcept {
    for (int i = 0; i < kParamCount; ++i) smooth_[static_cast<std::size_t>(i)] = target_[static_cast<std::size_t>(i)];
    filterAt_.fill(std::numeric_limits<double>::quiet_NaN());
    const auto on = [&](Param p) { return target_[static_cast<std::size_t>(p)] >= 0.5f ? 1.0 : 0.0; };
    mix_ = {on(kEqOn), on(kCompOn), on(kSatOn), on(kToneOn), on(kStereoOn), on(kClipOn), on(kLimitOn), on(kBypass)};
    mixFrom_ = mix_;
    inGain_ = inGainFrom_ = dbToGain(smooth_[kInput]);
    loudGain_ = loudGainFrom_ = dbToGain(smooth_[kLoudness]);
    for (Biquad* f : {&lowCut_, &mud_, &bass_, &air_}) f->z1[0] = f->z1[1] = f->z2[0] = f->z2[1] = 0.0;
    compEnv_ = 0.0;
    for (int ch = 0; ch < 2; ++ch) {
        os_.up[ch].fill(0.0);
        os_.down[ch].fill(0.0);
    }
    os_.upPos = os_.downPos = 0;
    satDry_.clear();
    limiterDelay_.clear();
    dryDelay_.clear();
    sideLow_ = 0.0;
    for (TruePeak& d : detect_) d.reset();
    lastSegment_ = 0.0;
    minHead_ = minSize_ = 0;
    tick_ = 0;
    release_ = 1.0;
    std::fill(box_.begin(), box_.end(), 1.0);
    boxPos_ = 0;
    boxSum_ = static_cast<double>(lookahead_);
    advance(0);
    resetMeters();
}

void Processor::resetMeters() noexcept {
    outMeter_.reset();
    inMeter_.reset();
    for (TruePeak& p : outPeak_) p.reset();
    truePeakMax_ = 0.0;
    blockReduction_ = 0.0;
}

void Processor::setParam(int param, float value) noexcept {
    if (param < 0 || param >= kParamCount || !std::isfinite(value)) return;
    const ParamInfo& p = kParams[static_cast<std::size_t>(param)];
    target_[static_cast<std::size_t>(param)] = p.toggle ? (value >= 0.5f ? 1.0f : 0.0f) : std::clamp(value, p.min, p.max);
}

void Processor::updateFilters() noexcept {
    designHighpass(lowCut_.b0, lowCut_.b1, lowCut_.b2, lowCut_.a1, lowCut_.a2, fs_, smooth_[kLowCut]);
    designBell(mud_.b0, mud_.b1, mud_.b2, mud_.a1, mud_.a2, fs_, 320.0, 0.9, smooth_[kMud]);
    designShelf(bass_.b0, bass_.b1, bass_.b2, bass_.a1, bass_.a2, fs_, 110.0, smooth_[kBass], false);
    designShelf(air_.b0, air_.b1, air_.b2, air_.a1, air_.a2, fs_, 8000.0, smooth_[kAir], true);
    for (Param p : {kLowCut, kMud, kBass, kAir}) filterAt_[p] = smooth_[p];
}

// Zu Beginn jedes Abschnitts: Werte weiterglaetten, Ueberblendungen weiterschieben.
void Processor::advance(int frames) noexcept {
    const double k = frames > 0 ? 1.0 - std::exp(-frames / (fs_ * 0.02)) : 1.0;
    for (int i = 0; i < kParamCount; ++i) {
        const ParamInfo& p = kParams[static_cast<std::size_t>(i)];
        if (p.toggle && i != kMonoBass) continue;
        double& s = smooth_[static_cast<std::size_t>(i)];
        const double t = target_[static_cast<std::size_t>(i)];
        s += (t - s) * k;
        if (std::abs(t - s) < 1e-7) s = t;
    }
    const double step = frames > 0 ? frames / (fs_ * 0.01) : 1.0;  // 10 ms zum Ueberblenden
    const Param toggles[kStages] = {kEqOn, kCompOn, kSatOn, kToneOn, kStereoOn, kClipOn, kLimitOn, kBypass};
    for (int s = 0; s < kStages; ++s) {
        mixFrom_[static_cast<std::size_t>(s)] = mix_[static_cast<std::size_t>(s)];
        const double want = target_[static_cast<std::size_t>(toggles[s])] >= 0.5f ? 1.0 : 0.0;
        double& m = mix_[static_cast<std::size_t>(s)];
        m = want > m ? std::min(want, m + step) : std::max(want, m - step);
    }
    inGainFrom_ = inGain_;
    inGain_ = dbToGain(smooth_[kInput]);
    loudGainFrom_ = loudGain_;
    loudGain_ = dbToGain(smooth_[kLoudness]);
    bool filtersChanged = false;
    for (Param p : {kLowCut, kMud, kBass, kAir})
        if (!(std::abs(filterAt_[p] - smooth_[p]) < 1e-5)) filtersChanged = true;  // auch bei NaN
    if (filtersChanged) updateFilters();
    satDrive_ = dbToGain(smooth_[kDrive]);
    ceiling_ = dbToGain(smooth_[kCeiling]);
    releaseCoef_ = std::exp(-1.0 / (fs_ * smooth_[kRelease] * 0.001));
    for (Biquad* f : {&lowCut_, &mud_, &bass_, &air_}) f->flush();
    killDenormal(compEnv_);
    killDenormal(sideLow_);
}

// Saettigung bei doppelter Abtastrate fuer beide Kanaele. Liefert das um
// Oversampler::kDelay verzoegerte Ergebnis.
void Processor::saturate(double& left, double& right) noexcept {
    constexpr int n = Oversampler::kTaps;
    const double* h = halfband_.data();
    const double drive = satDrive_;
    double* io[2]{&left, &right};
    // Ringpuffer doppelt abgelegt: Nach dem Schreiben an pos liegt das Fenster
    // am Stueck bei buffer[pos+1 .. pos+n], buffer[pos+n] ist der neueste Wert.
    const auto write = [](std::array<double, 2 * n>& buffer, int pos, double value) {
        buffer[static_cast<std::size_t>(pos)] = value;
        buffer[static_cast<std::size_t>(pos + n)] = value;
    };
    const auto convolve = [h](const double* window) {
        double y = 0.0;
        for (int k = 0; k < n; ++k) y += h[k] * window[k];
        return y;
    };
    double out[2]{};
    for (int sub = 0; sub < 2; ++sub) {
        const int upPos = os_.upPos, downPos = os_.downPos;
        for (int ch = 0; ch < 2; ++ch) {
            // Hochtasten: das Sample, dann eine Null; Faktor 2 gleicht die Null aus.
            write(os_.up[ch], upPos, sub == 0 ? 2.0 * *io[ch] : 0.0);
            const double y = convolve(os_.up[ch].data() + upPos + 1);
            // tanh(d*x)/d: leise Signale bleiben gleich laut, Spitzen werden weich gerundet.
            write(os_.down[ch], downPos, std::tanh(drive * y) / drive);
            // Heruntertasten: nur bei geraden Takten, dann passt die Verzoegerung genau.
            if (sub == 0) out[ch] = convolve(os_.down[ch].data() + downPos + 1);
        }
        os_.upPos = upPos + 1 == n ? 0 : upPos + 1;
        os_.downPos = downPos + 1 == n ? 0 : downPos + 1;
    }
    left = out[0];
    right = out[1];
}

void Processor::process(float* left, float* right, int numFrames) noexcept {
    constexpr int kChunk = 32;
    double minGain = 1.0;
    for (int start = 0; start < numFrames; start += kChunk) {
        const int frames = std::min(kChunk, numFrames - start);
        advance(frames);

        const double threshold = smooth_[kThreshold];
        const double ratio = std::max(1.0, smooth_[kRatio]);
        const double slope = 1.0 / ratio - 1.0;
        const double makeup = -threshold * (1.0 - 1.0 / ratio) * 0.5;  // gleicht den Pegelverlust grob aus
        const double width = smooth_[kWidth] * 0.01;
        const double lowWidth = (1.0 - smooth_[kMonoBass]) * std::min(width, 1.0);
        const double satMix = smooth_[kSatMix] * 0.01;
        const double clipDrive = dbToGain(smooth_[kClipDrive]);

        for (int i = 0; i < frames; ++i) {
            const double t = static_cast<double>(i + 1) / frames;
            const auto mixAt = [&](int stage) {
                const double a = mixFrom_[static_cast<std::size_t>(stage)];
                return a + (mix_[static_cast<std::size_t>(stage)] - a) * t;
            };
            float& outL = left[start + i];
            float& outR = right[start + i];
            const double rawL = sanitize(outL), rawR = sanitize(outR);
            inMeter_.process(rawL, rawR);

            const double inGain = inGainFrom_ + (inGain_ - inGainFrom_) * t;
            double l = rawL * inGain, r = rawR * inGain;

            // Korrektur: Tiefenfilter und Mulm-Absenkung
            {
                const double m = mixAt(kStageEq);
                const double el = mud_.step(0, lowCut_.step(0, l));
                const double er = mud_.step(1, lowCut_.step(1, r));
                l += (el - l) * m;
                r += (er - r) * m;
            }

            // Kompressor, beide Kanaele gemeinsam, weiches Knie (6 dB)
            {
                const double level = std::max(std::abs(l), std::abs(r));
                const double coef = level > compEnv_ ? compAttack_ : compRelease_;
                compEnv_ = coef * compEnv_ + (1.0 - coef) * level;
                const double over = 20.0 * std::log10(std::max(compEnv_, 1e-9)) - threshold;
                double reduction = 0.0;
                if (over >= 3.0) reduction = slope * over;
                else if (over > -3.0) reduction = slope * (over + 3.0) * (over + 3.0) / 12.0;
                const double m = mixAt(kStageComp);
                const double g = 1.0 + (dbToGain(reduction + makeup) - 1.0) * m;
                l *= g;
                r *= g;
            }

            // Saettigung (laeuft immer, damit die Verzoegerung gleich bleibt)
            {
                double dl = l, dr = r;
                satDry_.push(dl, dr);
                double wl = l, wr = r;
                saturate(wl, wr);
                const double m = mixAt(kStageSat) * satMix;
                l = dl + (wl - dl) * m;
                r = dr + (wr - dr) * m;
            }

            // Klang
            {
                const double tl = air_.step(0, bass_.step(0, l));
                const double tr = air_.step(1, bass_.step(1, r));
                const double m = mixAt(kStageTone);
                l += (tl - l) * m;
                r += (tr - r) * m;
            }

            // Stereobreite: Seitensignal in tief und hoch geteilt (ergaenzt sich exakt).
            {
                const double mid = (l + r) * 0.5, side = (l - r) * 0.5;
                sideLow_ = sideCoef_ * sideLow_ + (1.0 - sideCoef_) * side;
                const double sideOut = sideLow_ * lowWidth + (side - sideLow_) * width;
                const double m = mixAt(kStageStereo);
                const double s = side + (sideOut - side) * m;
                l = mid + s;
                r = mid - s;
            }

            // Lautheit: Verstaerkung vor Clipper und Limiter
            const double loud = loudGainFrom_ + (loudGain_ - loudGainFrom_) * t;
            l *= loud;
            r *= loud;

            // Soft-Clipper: bis 0,85 linear, darueber weich gegen 1
            {
                const auto clip = [clipDrive](double v) {
                    const double x = v * clipDrive, a = std::abs(x);
                    constexpr double knee = 0.85;
                    const double y = a <= knee ? a : knee + (1.0 - knee) * std::tanh((a - knee) / (1.0 - knee));
                    return std::copysign(y, x);
                };
                const double m = mixAt(kStageClip);
                l += (clip(l) - l) * m;
                r += (clip(r) - r) * m;
            }

            // Limiter: Spitzen (8-fach interpoliert) bestimmen die noetige Absenkung;
            // die Absenkung wird ueber die Vorausschau gehalten und linear eingeblendet,
            // so dass sie beim Eintreffen der Spitze voll wirkt.
            {
                const double segment = std::max(detect_[0].push(l), detect_[1].push(r));
                const double peak = std::max(segment, lastSegment_);
                lastSegment_ = segment;
                const double need = peak > ceiling_ ? ceiling_ / peak : 1.0;

                // gleitendes Minimum ueber lookahead_ Werte
                const int capacity = static_cast<int>(minValue_.size());
                while (minSize_ > 0) {
                    const int back = (minHead_ + minSize_ - 1) % capacity;
                    if (minValue_[static_cast<std::size_t>(back)] < need) break;
                    --minSize_;
                }
                const int slot = (minHead_ + minSize_) % capacity;
                minValue_[static_cast<std::size_t>(slot)] = need;
                minIndex_[static_cast<std::size_t>(slot)] = tick_;
                ++minSize_;
                while (minIndex_[static_cast<std::size_t>(minHead_)] <= tick_ - lookahead_) {
                    minHead_ = (minHead_ + 1) % capacity;
                    --minSize_;
                }
                const double held = minValue_[static_cast<std::size_t>(minHead_)];
                ++tick_;

                // Rueckstellung: sofort nach unten, langsam nach oben (bleibt unter dem Haltewert)
                release_ = held < release_ ? held : held + (release_ - held) * releaseCoef_;

                // gleitender Mittelwert ueber lookahead_ Werte: lineare Rampe
                boxSum_ += release_ - box_[static_cast<std::size_t>(boxPos_)];
                box_[static_cast<std::size_t>(boxPos_)] = release_;
                if (++boxPos_ == lookahead_) {
                    boxPos_ = 0;
                    boxSum_ = 0.0;  // Rundungsfehler nicht anwachsen lassen
                    for (double v : box_) boxSum_ += v;
                }
                const double gain = std::min(1.0, boxSum_ / lookahead_);

                limiterDelay_.push(l, r);
                const double m = mixAt(kStageLimit);
                const double g = 1.0 + (gain - 1.0) * m;
                l *= g;
                r *= g;
                if (m >= 1.0) {  // letzte Sicherung, greift praktisch nie
                    l = std::clamp(l, -ceiling_, ceiling_);
                    r = std::clamp(r, -ceiling_, ceiling_);
                }
                minGain = std::min(minGain, g);
            }

            // Umgehen: das unbearbeitete Signal, gleich verzoegert
            {
                double dl = rawL, dr = rawR;
                dryDelay_.push(dl, dr);
                const double m = mixAt(kStageBypass);
                l += (dl - l) * m;
                r += (dr - r) * m;
            }

            outMeter_.process(l, r);
            truePeakMax_ = std::max(truePeakMax_, std::max(outPeak_[0].push(l), outPeak_[1].push(r)));
            outL = static_cast<float>(sanitize(l));
            outR = static_cast<float>(sanitize(r));
        }
    }
    blockReduction_ = -20.0 * std::log10(std::max(minGain, 1e-6));
}

// --- MasteringEffect -----------------------------------------------------------

MasteringEffect::MasteringEffect(double sampleRate) {
    for (int i = 0; i < kParamCount; ++i)
        params_[static_cast<std::size_t>(i)].store(kParams[static_cast<std::size_t>(i)].def, std::memory_order_relaxed);
    processor_.prepare(sampleRate);
}

float MasteringEffect::param(int param) const noexcept {
    if (param < 0 || param >= kParamCount) return 0.0f;
    return params_[static_cast<std::size_t>(param)].load(std::memory_order_relaxed);
}

void MasteringEffect::setParam(int param, float value) noexcept {
    if (param < 0 || param >= kParamCount || !std::isfinite(value)) return;
    const ParamInfo& p = kParams[static_cast<std::size_t>(param)];
    value = p.toggle ? (value >= 0.5f ? 1.0f : 0.0f) : std::clamp(value, p.min, p.max);
    params_[static_cast<std::size_t>(param)].store(value, std::memory_order_relaxed);
}

void MasteringEffect::process(float* left, float* right, int numFrames, const PluginMidiEvent*, int,
                              const PluginTransport& transport) noexcept {
    // Messung neu beginnen: auf Wunsch und bei jedem Start der Wiedergabe (auch beim Export).
    if (resetRequested_.exchange(false, std::memory_order_relaxed) || (transport.playing && !wasPlaying_))
        processor_.resetMeters();
    wasPlaying_ = transport.playing;
    for (int i = 0; i < kParamCount; ++i)
        processor_.setParam(i, params_[static_cast<std::size_t>(i)].load(std::memory_order_relaxed));
    processor_.process(left, right, numFrames);

    momentary_.store(processor_.output().momentary(), std::memory_order_relaxed);
    shortTerm_.store(processor_.output().shortTerm(), std::memory_order_relaxed);
    integrated_.store(processor_.output().integrated(), std::memory_order_relaxed);
    inputIntegrated_.store(processor_.input().integrated(), std::memory_order_relaxed);
    const double tp = processor_.truePeakMax();
    truePeak_.store(tp > 0.0 ? 20.0 * std::log10(tp) : kSilence, std::memory_order_relaxed);
    reduction_.store(processor_.gainReductionDb(), std::memory_order_relaxed);
}

MasteringEffect::Meters MasteringEffect::meters() const noexcept {
    Meters m;
    m.momentary = momentary_.load(std::memory_order_relaxed);
    m.shortTerm = shortTerm_.load(std::memory_order_relaxed);
    m.integrated = integrated_.load(std::memory_order_relaxed);
    m.inputIntegrated = inputIntegrated_.load(std::memory_order_relaxed);
    m.truePeakDb = truePeak_.load(std::memory_order_relaxed);
    m.reductionDb = reduction_.load(std::memory_order_relaxed);
    return m;
}

std::vector<char> MasteringEffect::saveState() const {
    std::string text = "mastering 1\n";
    for (int i = 0; i < kParamCount; ++i) {
        char line[96];
        std::snprintf(line, sizeof line, "%s=%.6g\n", kParams[static_cast<std::size_t>(i)].key, static_cast<double>(param(i)));
        text += line;
    }
    return std::vector<char>(text.begin(), text.end());
}

void MasteringEffect::loadState(const std::vector<char>& state) {
    const std::string text(state.begin(), state.end());
    std::size_t start = 0;
    while (start < text.size()) {
        std::size_t end = text.find('\n', start);
        if (end == std::string::npos) end = text.size();
        const std::string line = text.substr(start, end - start);
        start = end + 1;
        const std::size_t eq = line.find('=');
        if (eq == std::string::npos) continue;
        const std::string key = line.substr(0, eq);
        for (int i = 0; i < kParamCount; ++i) {
            if (key != kParams[static_cast<std::size_t>(i)].key) continue;
            char* rest = nullptr;
            const double value = std::strtod(line.c_str() + eq + 1, &rest);
            if (rest != line.c_str() + eq + 1) setParam(i, static_cast<float>(value));
            break;
        }
    }
}

}  // namespace mastering
