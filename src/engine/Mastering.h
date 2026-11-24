#pragma once

#include "engine/AudioPlugin.h"
#include "engine/BuiltinEffect.h"

#include <array>
#include <atomic>
#include <cstdint>
#include <vector>

// Eingebauter Mastering-Effekt fuer die Master-Spur.
//
// Kette: Eingang, Korrektur-EQ, Kompressor, Saettigung (2-fach ueberabgetastet),
// Klang-EQ, Stereobreite, Lautheit, Soft-Clipper, Limiter mit Vorausschau und
// True-Peak-Erkennung. Dazu Lautheitsmessung nach ITU-R BS.1770 (Momentan,
// Kurzzeit, integriert mit Gating) und True-Peak-Anzeige.
//
// Der Ablauf geht auf "MasterForge" zurueck, einen Entwurf von ChatGPT; Limiter,
// Messung, Saettigung und Stereo sind hier neu geschrieben.
namespace mastering {

enum Param : int {
    kInput,
    kEqOn, kLowCut, kMud,
    kCompOn, kThreshold, kRatio,
    kSatOn, kDrive, kSatMix,
    kToneOn, kBass, kAir,
    kStereoOn, kWidth, kMonoBass,
    kClipOn, kClipDrive,
    kLimitOn, kLoudness, kCeiling, kRelease,
    kBypass,
    kParamCount
};

using ParamInfo = fx::ParamInfo;

inline constexpr std::array<ParamInfo, kParamCount> kParams{{
    {"eingang", L"Eingang", L"dB", -12.0f, 12.0f, 0.0f, false, false},
    {"korrektur", L"Korrektur", L"", 0, 1, 1, true, false},
    {"tiefenfilter", L"Tiefenfilter", L"Hz", 10.0f, 80.0f, 25.0f, false, true},
    {"mulm", L"Mulm (320 Hz)", L"dB", -6.0f, 0.0f, -1.0f, false, false},
    {"kompressor", L"Kompressor", L"", 0, 1, 1, true, false},
    {"schwelle", L"Schwelle", L"dB", -36.0f, 0.0f, -18.0f, false, false},
    {"verhaeltnis", L"Verhältnis", L":1", 1.0f, 6.0f, 2.0f, false, false},
    {"saettigung", L"Sättigung", L"", 0, 1, 1, true, false},
    {"drive", L"Drive", L"dB", 0.0f, 18.0f, 1.5f, false, false},
    {"mischung", L"Mischung", L"%", 0.0f, 100.0f, 20.0f, false, false},
    {"klang", L"Klang", L"", 0, 1, 1, true, false},
    {"bass", L"Bass (110 Hz)", L"dB", -6.0f, 6.0f, 0.0f, false, false},
    {"hoehen", L"Höhen (8 kHz)", L"dB", -6.0f, 6.0f, 0.0f, false, false},
    {"stereo", L"Stereo", L"", 0, 1, 1, true, false},
    {"breite", L"Breite", L"%", 0.0f, 150.0f, 100.0f, false, false},
    {"bassmono", L"Bass mono (unter 120 Hz)", L"", 0, 1, 0, true, false},
    {"clipper", L"Clipper", L"", 0, 1, 1, true, false},
    {"clipdrive", L"Clip-Drive", L"dB", 0.0f, 12.0f, 0.0f, false, false},
    {"limiter", L"Limiter", L"", 0, 1, 1, true, false},
    {"lautheit", L"Lautheit", L"dB", -12.0f, 18.0f, 0.0f, false, false},
    {"grenze", L"Grenze", L"dBTP", -6.0f, -0.1f, -1.0f, false, false},
    {"rueckstellzeit", L"Rückstellzeit", L"ms", 10.0f, 1000.0f, 100.0f, false, true},
    {"umgehen", L"Umgehen", L"", 0, 1, 0, true, false},
}};

// Wert 0..1 eines Reglers zu seinem Wert und zurueck.
float toPosition(int param, float value) noexcept;
float fromPosition(int param, float position) noexcept;

constexpr double kSilence = -1e9;  // Lautheit/Pegel, wenn nichts gemessen wurde

// --- Bausteine (oeffentlich, damit sie sich einzeln pruefen lassen) ----------

// Lautheit nach ITU-R BS.1770-4 / EBU R128 fuer ein Stereosignal.
class LoudnessMeter {
public:
    void prepare(double sampleRate);
    void reset() noexcept;
    void process(double left, double right) noexcept;

    // LUFS; kSilence, solange nichts gemessen wurde.
    double momentary() const noexcept { return momentary_; }   // 400 ms
    double shortTerm() const noexcept { return shortTerm_; }   // 3 s
    double integrated() const noexcept { return integrated_; } // seit reset(), mit Gating

private:
    struct Biquad {
        double b0 = 1, b1 = 0, b2 = 0, a1 = 0, a2 = 0;
        double z1[2]{}, z2[2]{};
    };
    void finishSubBlock() noexcept;
    void updateIntegrated() noexcept;

    Biquad shelf_, highpass_;
    int subBlockFrames_ = 4800;
    int subBlockCount_ = 0;     // Frames im laufenden 100-ms-Abschnitt
    double subBlockSum_ = 0.0;  // Summe der gewichteten Quadrate
    std::array<double, 30> subBlocks_{};  // die letzten 3 s in 100-ms-Abschnitten
    int subBlockIndex_ = 0;
    int subBlocksSeen_ = 0;
    double momentary_ = kSilence, shortTerm_ = kSilence, integrated_ = kSilence;

    // Integriert: Histogramm der 400-ms-Bloecke in 0,01-LU-Schritten von -70 bis +5 LUFS.
    static constexpr int kBins = 7500;
    std::vector<double> binEnergy_;
    std::vector<std::uint32_t> binCount_;
    double gatedEnergy_ = 0.0;     // Summe aller Bloecke ueber -70 LUFS
    std::uint64_t gatedBlocks_ = 0;
};

// Spitzenwert zwischen den Samples, 8-fach interpoliert. Liefert zu jedem
// neuen Sample den hoechsten Betrag im Abschnitt von Sample c bis c+1, wobei
// c um kDelay Samples zurueckliegt.
class TruePeak {
public:
    static constexpr int kHalf = 16;  // Filterlaenge je Phase: 2 * kHalf
    static constexpr int kDelay = kHalf;
    TruePeak();
    void reset() noexcept;
    double push(double sample) noexcept;
    // Ueberabtastung passend zur Rate: 8-fach bis 64 kHz, 4-fach bis 128 kHz,
    // darueber 2-fach. So wird immer mit etwa 384 kHz gemessen (BS.1770 verlangt
    // bei 48 kHz mindestens 4-fach), hohe Raten kosten nicht unnoetig Rechenzeit.
    void setSampleRate(double sampleRate) noexcept;
    int factor() const noexcept { return 8 / step_; }

private:
    std::array<double, 4 * kHalf> history_{};  // jedes Sample doppelt: das Fenster liegt am Stueck
    int pos_ = 0;
    int step_ = 1;  // jede wievielte der 7 Zwischenphasen berechnet wird
};

// Der Effekt ohne Plugin-Huelle: Parameter in Anzeigewerten, Audio in Bloecken.
class Processor {
public:
    Processor();
    void prepare(double sampleRate);
    void reset() noexcept;
    void setParam(int param, float value) noexcept;
    float param(int param) const noexcept { return target_[static_cast<std::size_t>(param)]; }
    void process(float* left, float* right, int numFrames) noexcept;

    int latency() const noexcept { return latency_; }
    double sampleRate() const noexcept { return fs_; }

    // Messwerte
    const LoudnessMeter& output() const noexcept { return outMeter_; }
    const LoudnessMeter& input() const noexcept { return inMeter_; }
    double truePeakMax() const noexcept { return truePeakMax_; }      // Betrag, seit resetMeters()
    double gainReductionDb() const noexcept { return blockReduction_; } // staerkste Absenkung im letzten Block
    void resetMeters() noexcept;

private:
    struct Biquad {
        double b0 = 1, b1 = 0, b2 = 0, a1 = 0, a2 = 0;
        double z1[2]{}, z2[2]{};
        double step(int ch, double x) noexcept;
        void flush() noexcept;
    };
    // 2-fach ueberabgetastete Saettigung mit Halbband-Filtern.
    struct Oversampler {
        static constexpr int kTaps = 47;
        static constexpr int kDelay = (kTaps - 1) / 2;  // in Samples der Grundrate
        std::array<double, 2 * kTaps> up[2]{}, down[2]{};  // doppelt abgelegt wie bei TruePeak
        int upPos = 0, downPos = 0;
    };
    struct Delay {
        std::vector<double> data[2];
        int pos = 0, length = 1;
        void setup(int delay);
        void clear() noexcept;
        // Schreibt den neuen Wert und liefert den um delay verzoegerten.
        void push(double& l, double& r) noexcept;
    };

    void updateFilters() noexcept;
    void advance(int frames) noexcept;
    void saturate(double& left, double& right) noexcept;

    double fs_ = 48000.0;
    int lookahead_ = 240;
    int latency_ = 0;
    std::array<float, kParamCount> target_{};
    std::array<double, kParamCount> smooth_{};   // geglaettete Anzeigewerte
    std::array<double, kParamCount> filterAt_{}; // Werte, fuer die die Filter berechnet sind

    // Ueberblendungen 0..1 je Stufe (vermeidet Knacken beim Ein- und Ausschalten)
    enum Stage { kStageEq, kStageComp, kStageSat, kStageTone, kStageStereo, kStageClip, kStageLimit, kStageBypass, kStages };
    std::array<double, kStages> mix_{}, mixFrom_{};
    // Verstaerkungen am Anfang und Ende des laufenden Abschnitts (linear gerampt)
    double inGain_ = 1, inGainFrom_ = 1, loudGain_ = 1, loudGainFrom_ = 1;

    Biquad lowCut_, mud_, bass_, air_;
    double compEnv_ = 0, compAttack_ = 0, compRelease_ = 0;
    double satDrive_ = 1;
    Oversampler os_;
    std::vector<double> halfband_;
    Delay satDry_;
    double sideLow_ = 0, sideCoef_ = 0;

    // Limiter
    TruePeak detect_[2];
    double lastSegment_ = 0;
    std::vector<double> minValue_;
    std::vector<std::int64_t> minIndex_;
    int minHead_ = 0, minSize_ = 0;
    std::int64_t tick_ = 0;
    double release_ = 1, releaseCoef_ = 0;
    std::vector<double> box_;
    int boxPos_ = 0;
    double boxSum_ = 0;
    Delay limiterDelay_, dryDelay_;
    double ceiling_ = 1;

    // Messung
    LoudnessMeter outMeter_, inMeter_;
    TruePeak outPeak_[2];
    double truePeakMax_ = 0;
    double blockReduction_ = 0;
};

// --- Als Plugin fuer die Engine ------------------------------------------------

// Parameter und Speichern kommen aus fx::BuiltinEffect. Messwerte gehen
// atomar vom Audio-Thread zur Oberflaeche.
class MasteringEffect final : public fx::BuiltinEffect {
public:
    explicit MasteringEffect(double sampleRate);

    int latencyFrames() const noexcept override { return processor_.latency(); }
    void process(float* left, float* right, int numFrames, const PluginMidiEvent* events, int numEvents,
                 const PluginTransport& transport) noexcept override;

    void resetMeters() noexcept { resetRequested_.store(true, std::memory_order_relaxed); }

    struct Meters {
        double momentary = kSilence, shortTerm = kSilence, integrated = kSilence, inputIntegrated = kSilence;
        double truePeakDb = kSilence;  // hoechster True Peak seit dem Zuruecksetzen
        double reductionDb = 0;        // aktuelle Absenkung des Limiters
    };
    Meters meters() const noexcept;

private:
    Processor processor_;
    std::atomic<bool> resetRequested_{false};
    bool wasPlaying_ = false;
    std::atomic<double> momentary_{kSilence}, shortTerm_{kSilence}, integrated_{kSilence},
        inputIntegrated_{kSilence}, truePeak_{kSilence}, reduction_{0};
};

}  // namespace mastering
