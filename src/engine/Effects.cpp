#include "engine/Effects.h"

#include <algorithm>
#include <cmath>
#include <complex>

namespace fx {

namespace {

constexpr double kPi = 3.14159265358979323846;
constexpr int kChunk = 32;  // Parameter werden je 32 Samples nachgefuehrt

double dbToGain(double db) { return std::pow(10.0, db / 20.0); }
double clampHz(double hz, double fs) { return std::clamp(hz, 5.0, fs * 0.49); }

// Glaettung je Abschnitt: Zeitkonstante seconds.
double smoothing(int frames, double fs, double seconds) { return 1.0 - std::exp(-frames / (fs * seconds)); }

// Gleitet value Richtung target; schnappt ein, wenn es fast da ist.
void glide(double& value, double target, double k) {
    value += (target - value) * k;
    if (std::abs(target - value) < 1e-9 * (1.0 + std::abs(target))) value = target;
}

// Schalter weich ein- und ausblenden (10 ms).
void ramp(double& mix, bool on, int frames, double fs) {
    const double step = frames / (fs * 0.01);
    mix = on ? std::min(1.0, mix + step) : std::max(0.0, mix - step);
}

float flushFloat(double v) { return std::abs(v) < 1e-25 ? 0.0f : static_cast<float>(v); }
double sanitize(double v) { return std::isfinite(v) ? std::clamp(v, -64.0, 64.0) : 0.0; }

// Gemeinsame Beschriftungen
const wchar_t* const kHz = L"Hz";
const wchar_t* const kDb = L"dB";
const wchar_t* const kMs = L"ms";
const wchar_t* const kPercent = L"%";

}  // namespace

// --- Biquad --------------------------------------------------------------------

void Biquad::flush() noexcept {
    for (int ch = 0; ch < 2; ++ch) {
        if (std::abs(z1[ch]) < 1e-30) z1[ch] = 0.0;
        if (std::abs(z2[ch]) < 1e-30) z2[ch] = 0.0;
    }
}

double Biquad::magnitude(double hz, double sampleRate) const noexcept {
    const double w = 2.0 * kPi * hz / sampleRate;
    const std::complex<double> z = std::polar(1.0, -w);
    const std::complex<double> num = b0 + b1 * z + b2 * z * z;
    const std::complex<double> den = 1.0 + a1 * z + a2 * z * z;
    return std::abs(num / den);
}

namespace {
void normalize(Biquad& f, double b0, double b1, double b2, double a0, double a1, double a2) {
    f.b0 = b0 / a0;
    f.b1 = b1 / a0;
    f.b2 = b2 / a0;
    f.a1 = a1 / a0;
    f.a2 = a2 / a0;
}
}  // namespace

void Biquad::lowpass(double fs, double hz, double q) {
    const double w = 2.0 * kPi * clampHz(hz, fs) / fs, c = std::cos(w), alpha = std::sin(w) / (2.0 * q);
    normalize(*this, (1.0 - c) / 2.0, 1.0 - c, (1.0 - c) / 2.0, 1.0 + alpha, -2.0 * c, 1.0 - alpha);
}

void Biquad::highpass(double fs, double hz, double q) {
    const double w = 2.0 * kPi * clampHz(hz, fs) / fs, c = std::cos(w), alpha = std::sin(w) / (2.0 * q);
    normalize(*this, (1.0 + c) / 2.0, -(1.0 + c), (1.0 + c) / 2.0, 1.0 + alpha, -2.0 * c, 1.0 - alpha);
}

void Biquad::peak(double fs, double hz, double q, double db) {
    const double A = std::pow(10.0, db / 40.0);
    const double w = 2.0 * kPi * clampHz(hz, fs) / fs, c = std::cos(w), alpha = std::sin(w) / (2.0 * q);
    normalize(*this, 1.0 + alpha * A, -2.0 * c, 1.0 - alpha * A, 1.0 + alpha / A, -2.0 * c, 1.0 - alpha / A);
}

void Biquad::lowShelf(double fs, double hz, double db) {
    const double A = std::pow(10.0, db / 40.0);
    const double w = 2.0 * kPi * clampHz(hz, fs) / fs, c = std::cos(w);
    const double beta = 2.0 * std::sqrt(A) * std::sin(w) * 0.7071067811865476;  // Steilheit 1
    normalize(*this, A * ((A + 1) - (A - 1) * c + beta), 2 * A * ((A - 1) - (A + 1) * c),
              A * ((A + 1) - (A - 1) * c - beta), (A + 1) + (A - 1) * c + beta, -2 * ((A - 1) + (A + 1) * c),
              (A + 1) + (A - 1) * c - beta);
}

void Biquad::highShelf(double fs, double hz, double db) {
    const double A = std::pow(10.0, db / 40.0);
    const double w = 2.0 * kPi * clampHz(hz, fs) / fs, c = std::cos(w);
    const double beta = 2.0 * std::sqrt(A) * std::sin(w) * 0.7071067811865476;
    normalize(*this, A * ((A + 1) + (A - 1) * c + beta), -2 * A * ((A - 1) + (A + 1) * c),
              A * ((A + 1) + (A - 1) * c - beta), (A + 1) - (A - 1) * c + beta, 2 * ((A - 1) - (A + 1) * c),
              (A + 1) - (A - 1) * c - beta);
}

// --- EQ ------------------------------------------------------------------------

const std::array<ParamInfo, EqEffect::kCount> EqEffect::kParams{{
    {"hochpass", L"Hochpass", L"", 0, 1, 0, true, false},
    {"hochpass_hz", L"Frequenz", kHz, 20.0f, 500.0f, 80.0f, false, true},
    {"tiefen_hz", L"Frequenz", kHz, 30.0f, 500.0f, 100.0f, false, true},
    {"tiefen_db", L"Pegel", kDb, -15.0f, 15.0f, 0.0f, false, false},
    {"mitten1_hz", L"Frequenz", kHz, 80.0f, 3000.0f, 400.0f, false, true},
    {"mitten1_db", L"Pegel", kDb, -15.0f, 15.0f, 0.0f, false, false},
    {"mitten1_q", L"Güte (Q)", L"", 0.3f, 8.0f, 1.0f, false, true},
    {"mitten2_hz", L"Frequenz", kHz, 400.0f, 12000.0f, 2500.0f, false, true},
    {"mitten2_db", L"Pegel", kDb, -15.0f, 15.0f, 0.0f, false, false},
    {"mitten2_q", L"Güte (Q)", L"", 0.3f, 8.0f, 1.0f, false, true},
    {"hoehen_hz", L"Frequenz", kHz, 2000.0f, 18000.0f, 8000.0f, false, true},
    {"hoehen_db", L"Pegel", kDb, -15.0f, 15.0f, 0.0f, false, false},
    {"tiefpass", L"Tiefpass", L"", 0, 1, 0, true, false},
    {"tiefpass_hz", L"Frequenz", kHz, 1000.0f, 20000.0f, 16000.0f, false, true},
    {"ausgang_db", L"Ausgang", kDb, -12.0f, 12.0f, 0.0f, false, false},
}};

EqEffect::EqEffect(double sampleRate) : BuiltinEffect("EQ", L"EQ", kParams.data(), kCount, sampleRate) {
    for (int i = 0; i < kCount; ++i) smooth_[static_cast<std::size_t>(i)] = kParams[static_cast<std::size_t>(i)].def;
    design(smooth_.data(), sampleRate_, bands_.data());
    designedFor_ = smooth_;
    gain_ = dbToGain(smooth_[kOutput]);
}

void EqEffect::design(const double* v, double fs, Biquad* bands) noexcept {
    bands[0].highpass(fs, v[kLowCut], 0.7071067811865476);
    bands[1].lowShelf(fs, v[kLowFreq], v[kLowGain]);
    bands[2].peak(fs, v[kMid1Freq], v[kMid1Q], v[kMid1Gain]);
    bands[3].peak(fs, v[kMid2Freq], v[kMid2Q], v[kMid2Gain]);
    bands[4].highShelf(fs, v[kHighFreq], v[kHighGain]);
    bands[5].lowpass(fs, v[kHighCut], 0.7071067811865476);
}

double EqEffect::responseDb(const float* values, double sampleRate, double hz) noexcept {
    double v[kCount];
    for (int i = 0; i < kCount; ++i) v[i] = values[i];
    Biquad bands[6];
    design(v, sampleRate, bands);
    double db = v[kOutput];
    for (int b = 0; b < 6; ++b) {
        if (b == 0 && v[kLowCutOn] < 0.5) continue;
        if (b == 5 && v[kHighCutOn] < 0.5) continue;
        db += 20.0 * std::log10(std::max(1e-9, bands[b].magnitude(hz, sampleRate)));
    }
    return db;
}

void EqEffect::process(float* left, float* right, int numFrames, const PluginMidiEvent*, int,
                       const PluginTransport&) noexcept {
    if (pullParams()) {  // erster Block: gleich die eingestellten Werte
        for (int i = 0; i < kCount; ++i) smooth_[static_cast<std::size_t>(i)] = value(i);
        lowCutMix_ = value(kLowCutOn) >= 0.5f ? 1.0 : 0.0;
        highCutMix_ = value(kHighCutOn) >= 0.5f ? 1.0 : 0.0;
        gain_ = dbToGain(smooth_[kOutput]);
        designedFor_.fill(-1e300);
    }
    for (int start = 0; start < numFrames; start += kChunk) {
        const int frames = std::min(kChunk, numFrames - start);
        const double k = smoothing(frames, sampleRate_, 0.02);
        bool changed = false;
        for (int i = 0; i < kCount; ++i) {
            if (kParams[static_cast<std::size_t>(i)].toggle) continue;
            glide(smooth_[static_cast<std::size_t>(i)], value(i), k);
            if (smooth_[static_cast<std::size_t>(i)] != designedFor_[static_cast<std::size_t>(i)]) changed = true;
        }
        if (changed) {
            design(smooth_.data(), sampleRate_, bands_.data());
            designedFor_ = smooth_;
        }
        const double lowFrom = lowCutMix_, highFrom = highCutMix_, gainFrom = gain_;
        ramp(lowCutMix_, value(kLowCutOn) >= 0.5f, frames, sampleRate_);
        ramp(highCutMix_, value(kHighCutOn) >= 0.5f, frames, sampleRate_);
        gain_ = dbToGain(smooth_[kOutput]);

        for (int i = 0; i < frames; ++i) {
            const double t = static_cast<double>(i + 1) / frames;
            const double lowMix = lowFrom + (lowCutMix_ - lowFrom) * t;
            const double highMix = highFrom + (highCutMix_ - highFrom) * t;
            const double gain = gainFrom + (gain_ - gainFrom) * t;
            float* io[2]{&left[start + i], &right[start + i]};
            for (int ch = 0; ch < 2; ++ch) {
                double x = sanitize(*io[ch]);
                x += (bands_[0].step(ch, x) - x) * lowMix;
                for (int b = 1; b <= 4; ++b) x = bands_[static_cast<std::size_t>(b)].step(ch, x);
                x += (bands_[5].step(ch, x) - x) * highMix;
                *io[ch] = static_cast<float>(x * gain);
            }
        }
        for (Biquad& b : bands_) b.flush();
    }
}

// --- Kompressor ----------------------------------------------------------------

const std::array<ParamInfo, CompressorEffect::kCount> CompressorEffect::kParams{{
    {"schwelle", L"Schwelle", kDb, -60.0f, 0.0f, -20.0f, false, false},
    {"verhaeltnis", L"Verhältnis", L":1", 1.0f, 20.0f, 4.0f, false, true},
    {"attack", L"Attack", kMs, 0.1f, 100.0f, 10.0f, false, true},
    {"release", L"Release", kMs, 10.0f, 2000.0f, 150.0f, false, true},
    {"knie", L"Knie", kDb, 0.0f, 12.0f, 6.0f, false, false},
    {"aufholen", L"Aufholen", kDb, 0.0f, 24.0f, 0.0f, false, false},
    {"auto", L"Automatisch aufholen", L"", 0, 1, 1, true, false},
    {"mischung", L"Mischung", kPercent, 0.0f, 100.0f, 100.0f, false, false},
}};

CompressorEffect::CompressorEffect(double sampleRate)
    : BuiltinEffect("Kompressor", L"Kompressor", kParams.data(), kCount, sampleRate) {}

double CompressorEffect::curve(double in, double threshold, double ratio, double knee) noexcept {
    const double over = in - threshold;
    const double slope = 1.0 / std::max(1.0, ratio) - 1.0;
    if (knee > 0.0 && std::abs(over) * 2.0 <= knee) return in + slope * (over + knee * 0.5) * (over + knee * 0.5) / (2.0 * knee);
    if (over > 0.0) return in + slope * over;
    return in;
}

double CompressorEffect::autoMakeup(double threshold, double ratio) noexcept {
    // Etwa die Haelfte dessen, was ein Signal bei 0 dBFS verliert.
    return -threshold * (1.0 - 1.0 / std::max(1.0, ratio)) * 0.5;
}

void CompressorEffect::process(float* left, float* right, int numFrames, const PluginMidiEvent*, int,
                               const PluginTransport&) noexcept {
    const bool first = pullParams();
    const double threshold = value(kThreshold), ratio = value(kRatio), knee = value(kKnee);
    const double attack = std::exp(-1.0 / (sampleRate_ * value(kAttack) * 0.001));
    const double release = std::exp(-1.0 / (sampleRate_ * value(kRelease) * 0.001));
    const double makeupTarget = value(kMakeup) + (value(kAutoMakeup) >= 0.5f ? autoMakeup(threshold, ratio) : 0.0);
    const double mixTarget = value(kMix) * 0.01;
    double maxReduction = 0.0, maxInput = -120.0;
    if (first) {
        makeup_ = makeupTarget;
        mix_ = mixTarget;
    }

    for (int start = 0; start < numFrames; start += kChunk) {
        const int frames = std::min(kChunk, numFrames - start);
        const double k = smoothing(frames, sampleRate_, 0.02);
        const double makeupFrom = makeup_, mixFrom = mix_;
        glide(makeup_, makeupTarget, k);
        glide(mix_, mixTarget, k);
        for (int i = 0; i < frames; ++i) {
            const double t = static_cast<double>(i + 1) / frames;
            const double makeup = makeupFrom + (makeup_ - makeupFrom) * t;
            const double mix = mixFrom + (mix_ - mixFrom) * t;
            float& l = left[start + i];
            float& r = right[start + i];
            const double dl = sanitize(l), dr = sanitize(r);
            const double level = 20.0 * std::log10(std::max(std::max(std::abs(dl), std::abs(dr)), 1e-9));
            const double wanted = level - curve(level, threshold, ratio, knee);  // Absenkung, >= 0
            // Spitzen sofort halten und mit Release loslassen, dann mit Attack einschwingen.
            peakHold_ = std::max(wanted, release * peakHold_ + (1.0 - release) * wanted);
            smoothed_ = attack * smoothed_ + (1.0 - attack) * peakHold_;
            const double gain = dbToGain(makeup - smoothed_);
            l = static_cast<float>(dl + (dl * gain - dl) * mix);
            r = static_cast<float>(dr + (dr * gain - dr) * mix);
            maxReduction = std::max(maxReduction, smoothed_);
            maxInput = std::max(maxInput, level);
        }
    }
    if (smoothed_ < 1e-12) smoothed_ = 0.0;
    if (peakHold_ < 1e-12) peakHold_ = 0.0;
    reduction_.store(maxReduction, std::memory_order_relaxed);
    input_.store(maxInput, std::memory_order_relaxed);
}

// --- Hall ----------------------------------------------------------------------
//
// Ein Netz aus acht Verzoegerungen, die ueber eine Hadamard-Matrix
// rueckgekoppelt sind (Feedback Delay Network). Davor Vorverzoegerung,
// Tiefenfilter und vier Allpaesse, die den Einschwung dicht machen. Jede
// Leitung hat einen Tiefpass (Daempfung) und eine Verstaerkung, die zur
// eingestellten Nachhallzeit (RT60) passt. Eine leichte Modulation der
// Laengen verhindert metallischen Klang.

namespace {
constexpr double kLineMs[8] = {31.3, 37.7, 41.9, 47.3, 53.9, 59.1, 66.7, 73.1};
constexpr double kDiffuserMs[4] = {4.77, 3.59, 12.73, 9.31};
constexpr double kMaxSizeFactor = 1.65;
constexpr double kModulationMs = 0.3;
double sizeFactor(double percent) { return 0.35 + 1.3 * percent * 0.01; }  // 10 % -> 0,48; 100 % -> 1,65
}  // namespace

const std::array<ParamInfo, ReverbEffect::kCount> ReverbEffect::kParams{{
    {"groesse", L"Größe", kPercent, 10.0f, 100.0f, 60.0f, false, false},
    {"nachhall", L"Nachhall", L"s", 0.2f, 15.0f, 2.0f, false, true},
    {"daempfung", L"Höhen bis", kHz, 1000.0f, 20000.0f, 7000.0f, false, true},
    {"vorverzoegerung", L"Vorverzögerung", kMs, 0.0f, 200.0f, 15.0f, false, false},
    {"tiefen_weg", L"Tiefen weg bis", kHz, 20.0f, 1000.0f, 120.0f, false, true},
    {"breite", L"Breite", kPercent, 0.0f, 100.0f, 100.0f, false, false},
    {"mischung", L"Mischung", kPercent, 0.0f, 100.0f, 30.0f, false, false},
}};

ReverbEffect::ReverbEffect(double sampleRate)
    : BuiltinEffect("Hall", L"Hall", kParams.data(), kCount, sampleRate) {
    const double fs = sampleRate_;
    for (int i = 0; i < kLines; ++i) {
        Line& line = lines_[static_cast<std::size_t>(i)];
        const double longest = (kLineMs[i] * kMaxSizeFactor + kModulationMs * 2.0) * 0.001 * fs;
        line.data.assign(static_cast<std::size_t>(longest) + 8, 0.0f);
        line.length = kLineMs[i] * sizeFactor(kParams[kSize].def) * 0.001 * fs;
    }
    for (int i = 0; i < kDiffusers; ++i)
        diffusers_[static_cast<std::size_t>(i)].data.assign(static_cast<std::size_t>(kDiffuserMs[i] * 0.001 * fs) + 1, 0.0f);
    predelay_.assign(static_cast<std::size_t>(0.2 * fs) + 2, 0.0f);
    size_ = kParams[kSize].def;
    decay_ = kParams[kDecay].def;
    mix_ = kParams[kMix].def * 0.01;
    width_ = kParams[kWidth].def * 0.01;
    updateCoefficients();
}

void ReverbEffect::updateCoefficients() noexcept {
    for (Line& line : lines_) line.gain = std::pow(10.0, -3.0 * line.length / (sampleRate_ * decay_));
}

double ReverbEffect::readLine(const Line& line, double extra) const noexcept {
    const int size = static_cast<int>(line.data.size());
    const double delay = std::clamp(line.length + extra, 1.0, static_cast<double>(size - 2));
    double position = line.pos - delay;
    if (position < 0) position += size;
    const int i0 = static_cast<int>(position);
    const double frac = position - i0;
    const int i1 = i0 + 1 == size ? 0 : i0 + 1;
    return line.data[static_cast<std::size_t>(i0)] * (1.0 - frac) + line.data[static_cast<std::size_t>(i1)] * frac;
}

void ReverbEffect::process(float* left, float* right, int numFrames, const PluginMidiEvent*, int,
                           const PluginTransport&) noexcept {
    if (pullParams()) {
        size_ = value(kSize);
        decay_ = value(kDecay);
        mix_ = value(kMix) * 0.01;
        width_ = value(kWidth) * 0.01;
    }
    const double fs = sampleRate_;
    const int predelaySize = static_cast<int>(predelay_.size());
    const int predelay = std::clamp(static_cast<int>(value(kPredelay) * 0.001 * fs), 0, predelaySize - 1);
    if (lowCutAt_ != value(kLowCut)) {
        lowCut_.highpass(fs, value(kLowCut), 0.7071067811865476);
        lowCutAt_ = value(kLowCut);
    }
    dampCoef_ = std::exp(-2.0 * kPi * value(kDamping) / fs);
    const double modDepth = kModulationMs * 0.001 * fs;
    const double lfoStep = 2.0 * kPi * 0.6 / fs;
    constexpr double kDiffusion = 0.6;
    constexpr double kScale = 0.35355339059327373;  // 1/sqrt(8)
    constexpr double kOutputGain = 1.2;  // ergibt bei 2 s Nachhall etwa -6 dB Energie je Kanal

    for (int start = 0; start < numFrames; start += kChunk) {
        const int frames = std::min(kChunk, numFrames - start);
        const double k = smoothing(frames, fs, 0.1);
        const double mixFrom = mix_, widthFrom = width_;
        glide(size_, value(kSize), k);
        glide(decay_, value(kDecay), k);
        glide(mix_, value(kMix) * 0.01, smoothing(frames, fs, 0.02));
        glide(width_, value(kWidth) * 0.01, smoothing(frames, fs, 0.02));
        const double factor = sizeFactor(size_);
        for (int i = 0; i < kLines; ++i)
            lines_[static_cast<std::size_t>(i)].length = kLineMs[i] * factor * 0.001 * fs;
        updateCoefficients();

        for (int n = 0; n < frames; ++n) {
            const double t = static_cast<double>(n + 1) / frames;
            const double mix = mixFrom + (mix_ - mixFrom) * t;
            const double width = widthFrom + (width_ - widthFrom) * t;
            float& outL = left[start + n];
            float& outR = right[start + n];
            const double dl = sanitize(outL), dr = sanitize(outR);

            // Vorverzoegerung, Tiefen weg, Diffusion
            predelay_[static_cast<std::size_t>(predelayPos_)] = static_cast<float>((dl + dr) * 0.5);
            int read = predelayPos_ - predelay;
            if (read < 0) read += predelaySize;
            predelayPos_ = predelayPos_ + 1 == predelaySize ? 0 : predelayPos_ + 1;
            double x = lowCut_.step(0, predelay_[static_cast<std::size_t>(read)]);
            for (Allpass& ap : diffusers_) {
                const double d = ap.data[static_cast<std::size_t>(ap.pos)];
                const double w = x + kDiffusion * d;
                x = d - kDiffusion * w;
                ap.data[static_cast<std::size_t>(ap.pos)] = flushFloat(w);
                ap.pos = ap.pos + 1 == static_cast<int>(ap.data.size()) ? 0 : ap.pos + 1;
            }

            // Lesen, daempfen, abschwaechen
            double v[kLines];
            double wetL = 0.0, wetR = 0.0;
            lfoPhase_ += lfoStep;
            if (lfoPhase_ > 2.0 * kPi) lfoPhase_ -= 2.0 * kPi;
            for (int i = 0; i < kLines; ++i) {
                Line& line = lines_[static_cast<std::size_t>(i)];
                const double o = readLine(line, modDepth * std::sin(lfoPhase_ + i * 0.785));
                wetL += (i % 2 == 0 ? o : -o);
                wetR += ((i / 2) % 2 == 0 ? o : -o);
                line.damp = (1.0 - dampCoef_) * o + dampCoef_ * line.damp;
                v[i] = line.damp * line.gain;
            }
            // Hadamard 8x8 (schnell, in drei Stufen), orthogonal skaliert
            for (int len = 1; len < kLines; len *= 2)
                for (int i = 0; i < kLines; i += 2 * len)
                    for (int j = i; j < i + len; ++j) {
                        const double a = v[j], b = v[j + len];
                        v[j] = a + b;
                        v[j + len] = a - b;
                    }
            for (int i = 0; i < kLines; ++i) {
                Line& line = lines_[static_cast<std::size_t>(i)];
                line.data[static_cast<std::size_t>(line.pos)] = flushFloat(v[i] * kScale + x * kScale);
                line.pos = line.pos + 1 == static_cast<int>(line.data.size()) ? 0 : line.pos + 1;
            }

            wetL *= kScale * kOutputGain;
            wetR *= kScale * kOutputGain;
            const double mid = (wetL + wetR) * 0.5, side = (wetL - wetR) * 0.5 * width;
            outL = static_cast<float>(dl * (1.0 - mix) + (mid + side) * mix);
            outR = static_cast<float>(dr * (1.0 - mix) + (mid - side) * mix);
        }
        lowCut_.flush();
        for (Line& line : lines_)
            if (std::abs(line.damp) < 1e-30) line.damp = 0.0;
    }
}

// --- Echo ----------------------------------------------------------------------

namespace {
const wchar_t* const kNoteNames[] = {L"1/1",  L"1/2",  L"1/4 punktiert", L"1/4", L"1/8 punktiert",
                                     L"1/8", L"1/8 Triole", L"1/16"};
constexpr double kNoteBeats[] = {4.0, 2.0, 1.5, 1.0, 0.75, 0.5, 1.0 / 3.0, 0.25};
}  // namespace

const std::array<ParamInfo, DelayEffect::kCount> DelayEffect::kParams{{
    {"im_takt", L"Im Takt", L"", 0, 1, 1, true, false},
    {"notenwert", L"Notenwert", L"", 0.0f, 7.0f, 3.0f, false, false, kNoteNames, 8},
    {"zeit", L"Zeit", kMs, 1.0f, 2000.0f, 350.0f, false, true},
    {"rueckkopplung", L"Rückkopplung", kPercent, 0.0f, 95.0f, 35.0f, false, false},
    {"pingpong", L"Ping-Pong", L"", 0, 1, 0, true, false},
    {"tiefen_weg", L"Tiefen weg bis", kHz, 20.0f, 2000.0f, 120.0f, false, true},
    {"hoehen_bis", L"Höhen bis", kHz, 1000.0f, 20000.0f, 6000.0f, false, true},
    {"mischung", L"Mischung", kPercent, 0.0f, 100.0f, 30.0f, false, false},
}};

DelayEffect::DelayEffect(double sampleRate) : BuiltinEffect("Echo", L"Echo", kParams.data(), kCount, sampleRate) {
    const auto size = static_cast<std::size_t>(kMaxSeconds * sampleRate_) + 8;
    buffer_[0].assign(size, 0.0f);
    buffer_[1].assign(size, 0.0f);
}

double DelayEffect::delaySeconds(const float* values, double tempo) noexcept {
    if (values[kSync] >= 0.5f) {
        const int note = std::clamp(static_cast<int>(std::lround(values[kNote])), 0, 7);
        return std::min(kMaxSeconds, kNoteBeats[note] * 60.0 / (tempo > 0.0 ? tempo : 120.0));
    }
    return std::clamp(static_cast<double>(values[kTime]) * 0.001, 0.001, kMaxSeconds);
}

void DelayEffect::process(float* left, float* right, int numFrames, const PluginMidiEvent*, int,
                          const PluginTransport& transport) noexcept {
    const bool first = pullParams();
    const double fs = sampleRate_;
    const double tempo = transport.tempo > 0.0 ? transport.tempo : 120.0;
    tempo_.store(tempo, std::memory_order_relaxed);
    float values[kCount];
    for (int i = 0; i < kCount; ++i) values[i] = value(i);
    const int size = static_cast<int>(buffer_[0].size());
    const double target = std::clamp(delaySeconds(values, tempo) * fs, 1.0, static_cast<double>(size - 4));
    if (first) {
        delay_ = target;
        mix_ = value(kMix) * 0.01;
        feedback_ = value(kFeedback) * 0.01;
        pingPong_ = value(kPingPong) >= 0.5f ? 1.0 : 0.0;
    }
    const double delayGlide = 1.0 - std::exp(-1.0 / (fs * 0.05));
    if (lowCutAt_ != value(kLowCut)) {
        lowCut_.highpass(fs, value(kLowCut), 0.7071067811865476);
        lowCutAt_ = value(kLowCut);
    }
    if (highCutAt_ != value(kHighCut)) {
        highCut_.lowpass(fs, value(kHighCut), 0.7071067811865476);
        highCutAt_ = value(kHighCut);
    }

    for (int start = 0; start < numFrames; start += kChunk) {
        const int frames = std::min(kChunk, numFrames - start);
        const double k = smoothing(frames, fs, 0.02);
        const double mixFrom = mix_, fbFrom = feedback_, ppFrom = pingPong_;
        glide(mix_, value(kMix) * 0.01, k);
        glide(feedback_, value(kFeedback) * 0.01, k);
        ramp(pingPong_, value(kPingPong) >= 0.5f, frames, fs);
        for (int n = 0; n < frames; ++n) {
            const double t = static_cast<double>(n + 1) / frames;
            const double mix = mixFrom + (mix_ - mixFrom) * t;
            const double fb = fbFrom + (feedback_ - fbFrom) * t;
            const double pp = ppFrom + (pingPong_ - ppFrom) * t;
            delay_ += (target - delay_) * delayGlide;  // Zeitwechsel gleitet wie beim Bandecho

            double position = pos_ - delay_;
            if (position < 0) position += size;
            const int i0 = static_cast<int>(position);
            const double frac = position - i0;
            const int i1 = i0 + 1 == size ? 0 : i0 + 1;
            double wet[2];
            for (int ch = 0; ch < 2; ++ch) {
                const auto& b = buffer_[ch];
                const double d = b[static_cast<std::size_t>(i0)] * (1.0 - frac) + b[static_cast<std::size_t>(i1)] * frac;
                wet[ch] = highCut_.step(ch, lowCut_.step(ch, d));
            }
            float& outL = left[start + n];
            float& outR = right[start + n];
            const double dl = sanitize(outL), dr = sanitize(outR);
            // Normal: jeder Kanal koppelt auf sich zurueck. Ping-Pong: Eingang
            // mono nach links, die Rueckkopplung wechselt jedes Mal die Seite.
            const double normalL = dl + fb * wet[0], normalR = dr + fb * wet[1];
            const double pingL = (dl + dr) * 0.5 + fb * wet[1], pingR = fb * wet[0];
            buffer_[0][static_cast<std::size_t>(pos_)] = flushFloat(normalL + (pingL - normalL) * pp);
            buffer_[1][static_cast<std::size_t>(pos_)] = flushFloat(normalR + (pingR - normalR) * pp);
            pos_ = pos_ + 1 == size ? 0 : pos_ + 1;

            outL = static_cast<float>(dl * (1.0 - mix) + wet[0] * mix);
            outR = static_cast<float>(dr * (1.0 - mix) + wet[1] * mix);
        }
        lowCut_.flush();
        highCut_.flush();
    }
}

}  // namespace fx
