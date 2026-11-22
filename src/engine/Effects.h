#pragma once

#include "engine/BuiltinEffect.h"

#include <array>
#include <atomic>
#include <vector>

// Eingebaute Effekte fuer die Kanaele: EQ, Kompressor, Hall, Echo.
namespace fx {

// Biquad (zweipoliges Filter) fuer zwei Kanaele, Koeffizienten nach dem RBJ-Kochbuch.
struct Biquad {
    double b0 = 1, b1 = 0, b2 = 0, a1 = 0, a2 = 0;
    double z1[2]{}, z2[2]{};

    double step(int ch, double x) noexcept {
        const double y = b0 * x + z1[ch];
        z1[ch] = b1 * x - a1 * y + z2[ch];
        z2[ch] = b2 * x - a2 * y;
        return y;
    }
    void reset() noexcept { z1[0] = z1[1] = z2[0] = z2[1] = 0.0; }
    void flush() noexcept;  // sehr kleine Zustaende auf 0 (gegen langsame Denormals)
    // Betrag des Frequenzgangs bei hz (fuer die Kurve im Fenster).
    double magnitude(double hz, double sampleRate) const noexcept;

    void lowpass(double fs, double hz, double q);
    void highpass(double fs, double hz, double q);
    void peak(double fs, double hz, double q, double db);
    void lowShelf(double fs, double hz, double db);
    void highShelf(double fs, double hz, double db);
};

// --- EQ ----------------------------------------------------------------------

class EqEffect final : public BuiltinEffect {
public:
    enum Param : int {
        kLowCutOn, kLowCut,
        kLowFreq, kLowGain,
        kMid1Freq, kMid1Gain, kMid1Q,
        kMid2Freq, kMid2Gain, kMid2Q,
        kHighFreq, kHighGain,
        kHighCutOn, kHighCut,
        kOutput,
        kCount
    };
    static const std::array<ParamInfo, kCount> kParams;

    explicit EqEffect(double sampleRate);
    void process(float* left, float* right, int numFrames, const PluginMidiEvent*, int,
                 const PluginTransport&) noexcept override;

    // Frequenzgang in dB fuer die angegebenen Werte (eine Zahl je Parameter).
    static double responseDb(const float* values, double sampleRate, double hz) noexcept;

private:
    static void design(const double* values, double fs, Biquad* bands) noexcept;  // 6 Filter
    std::array<double, kCount> smooth_{};
    std::array<double, kCount> designedFor_{};
    std::array<Biquad, 6> bands_;  // Hochpass, Tiefen, Mitten 1, Mitten 2, Hoehen, Tiefpass
    double lowCutMix_ = 0, highCutMix_ = 0, gain_ = 1;
};

// --- Kompressor --------------------------------------------------------------

class CompressorEffect final : public BuiltinEffect {
public:
    enum Param : int { kThreshold, kRatio, kAttack, kRelease, kKnee, kMakeup, kAutoMakeup, kMix, kCount };
    static const std::array<ParamInfo, kCount> kParams;

    explicit CompressorEffect(double sampleRate);
    void process(float* left, float* right, int numFrames, const PluginMidiEvent*, int,
                 const PluginTransport&) noexcept override;

    // Kennlinie: Ausgangspegel in dB zu einem Eingangspegel (ohne Aufholen).
    static double curve(double inputDb, double threshold, double ratio, double knee) noexcept;
    // Automatisches Aufholen in dB fuer diese Einstellung.
    static double autoMakeup(double threshold, double ratio) noexcept;

    // Messwerte fuer das Fenster
    double reductionDb() const noexcept { return reduction_.load(std::memory_order_relaxed); }
    double inputDb() const noexcept { return input_.load(std::memory_order_relaxed); }

private:
    double peakHold_ = 0, smoothed_ = 0;  // Absenkung in dB (positiv)
    double makeup_ = 0, mix_ = 1;
    std::atomic<double> reduction_{0}, input_{-120};
};

// --- Hall --------------------------------------------------------------------

class ReverbEffect final : public BuiltinEffect {
public:
    enum Param : int { kSize, kDecay, kDamping, kPredelay, kLowCut, kWidth, kMix, kCount };
    static const std::array<ParamInfo, kCount> kParams;

    explicit ReverbEffect(double sampleRate);
    void process(float* left, float* right, int numFrames, const PluginMidiEvent*, int,
                 const PluginTransport&) noexcept override;

private:
    static constexpr int kLines = 8;
    static constexpr int kDiffusers = 4;
    struct Line {
        std::vector<float> data;
        int pos = 0;
        double length = 1;  // aktuelle Laenge in Samples (gleitet bei Groessenaenderung)
        double damp = 0;    // Zustand des Tiefpasses
        double gain = 0.5;
    };
    struct Allpass {
        std::vector<float> data;
        int pos = 0;
    };
    double readLine(const Line& line, double extra) const noexcept;
    void updateCoefficients() noexcept;

    std::array<Line, kLines> lines_;
    std::array<Allpass, kDiffusers> diffusers_;
    std::vector<float> predelay_;
    int predelayPos_ = 0;
    Biquad lowCut_;
    double size_ = 0.6, mix_ = 0.3, width_ = 1, decay_ = 2, dampCoef_ = 0, lowCutAt_ = -1;
    double lfoPhase_ = 0;
};

// --- Echo --------------------------------------------------------------------

class DelayEffect final : public BuiltinEffect {
public:
    enum Param : int { kSync, kNote, kTime, kFeedback, kPingPong, kLowCut, kHighCut, kMix, kCount };
    static const std::array<ParamInfo, kCount> kParams;
    static constexpr double kMaxSeconds = 4.0;

    explicit DelayEffect(double sampleRate);
    void process(float* left, float* right, int numFrames, const PluginMidiEvent*, int,
                 const PluginTransport& transport) noexcept override;

    // Verzoegerung in Sekunden fuer diese Werte und dieses Tempo.
    static double delaySeconds(const float* values, double tempo) noexcept;
    double lastTempo() const noexcept { return tempo_.load(std::memory_order_relaxed); }

private:
    std::vector<float> buffer_[2];
    int pos_ = 0;
    double delay_ = 0;  // aktuelle Verzoegerung in Samples (gleitet)
    Biquad lowCut_, highCut_;
    double lowCutAt_ = -1, highCutAt_ = -1, mix_ = 0.3, feedback_ = 0.35, pingPong_ = 0;
    std::atomic<double> tempo_{120.0};
};

}  // namespace fx
