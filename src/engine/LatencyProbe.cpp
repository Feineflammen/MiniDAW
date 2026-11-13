#include "engine/LatencyProbe.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <string>

namespace {
constexpr double kPi = 3.14159265358979323846;
constexpr float kLevel = 0.5f;  // -6 dB
}  // namespace

LatencyProbe::LatencyProbe(double sampleRate) : sampleRate_(sampleRate) {
    // Sweep von 300 Hz bis 10 kHz (hoechstens 40 % der Samplerate), 512 Frames
    // lang, mit weichen Raendern. Ein Sweep laesst sich auch bei Rauschen und
    // verbogenem Frequenzgang genau wiederfinden.
    constexpr int kLength = 512;
    const double f0 = 300.0;
    const double f1 = std::min(10000.0, 0.4 * sampleRate);
    const double duration = kLength / sampleRate;
    sweep_.resize(kLength);
    for (int i = 0; i < kLength; ++i) {
        const double t = i / sampleRate;
        const double phase = 2.0 * kPi * (f0 * t + 0.5 * (f1 - f0) / duration * t * t);
        const double window = 0.5 - 0.5 * std::cos(2.0 * kPi * i / (kLength - 1));
        sweep_[static_cast<std::size_t>(i)] = static_cast<float>(kLevel * window * std::sin(phase));
    }

    maxLag_ = static_cast<int>(0.45 * sampleRate);  // bis 450 ms messbar
    lead_ = static_cast<std::int64_t>(0.25 * sampleRate);
    spacing_ = maxLag_ + kLength + static_cast<std::int64_t>(0.2 * sampleRate);
    total_ = lead_ + (kPulses - 1) * spacing_ + maxLag_ + kLength + static_cast<std::int64_t>(0.05 * sampleRate);
    captureL_.assign(static_cast<std::size_t>(total_), 0.0f);
    captureR_.assign(static_cast<std::size_t>(total_), 0.0f);
}

void LatencyProbe::process(const float* inL, const float* inR, float* outL, float* outR, int numFrames) noexcept {
    if (done_.load(std::memory_order_relaxed)) return;
    const auto length = static_cast<std::int64_t>(sweep_.size());

    for (int i = 0; i < numFrames; ++i) {
        const std::int64_t f = frame_ + i;
        float s = 0.0f;
        if (f >= lead_ && f < total_) {
            const std::int64_t k = (f - lead_) / spacing_;
            const std::int64_t offset = (f - lead_) - k * spacing_;
            if (k < kPulses && offset < length) s = sweep_[static_cast<std::size_t>(offset)];
        }
        outL[i] = s;
        if (outR) outR[i] = s;
        if (f < total_) {
            captureL_[static_cast<std::size_t>(f)] = inL ? inL[i] : 0.0f;
            captureR_[static_cast<std::size_t>(f)] = inR ? inR[i] : (inL ? inL[i] : 0.0f);
        }
    }
    frame_ += numFrames;

    // Muss die letzte Anweisung sein: Danach darf der UI-Thread die Sonde freigeben.
    if (frame_ >= total_) done_.store(true, std::memory_order_release);
}

LatencyProbe::Result LatencyProbe::analyze() const {
    Result result;
    if (!done()) {
        result.message = "Die Messung ist nicht fertig geworden.";
        return result;
    }

    // Den Kanal mit dem staerkeren Signal nehmen: Das Kabel steckt in irgendeinem Eingang.
    float peakL = 0.0f, peakR = 0.0f;
    for (std::size_t i = 0; i < captureL_.size(); ++i) {
        peakL = std::max(peakL, std::fabs(captureL_[i]));
        peakR = std::max(peakR, std::fabs(captureR_[i]));
    }
    const std::vector<float>& x = peakL >= peakR ? captureL_ : captureR_;
    result.inputPeak = std::max(peakL, peakR);
    if (result.inputPeak < 0.001f) {
        result.message =
            "Am Eingang kommt nichts an. Steckt das Kabel vom Ausgang im Eingang, ist der Gain aufgedreht "
            "und ist in Windows die richtige Soundkarte als Aufnahmegerät eingestellt?";
        return result;
    }

    // Laufende Energiesumme fuer die Normierung der Korrelation.
    std::vector<double> energy(x.size() + 1, 0.0);
    for (std::size_t i = 0; i < x.size(); ++i)
        energy[i + 1] = energy[i] + static_cast<double>(x[i]) * static_cast<double>(x[i]);
    double sweepEnergy = 0.0;
    for (const float s : sweep_) sweepEnergy += static_cast<double>(s) * static_cast<double>(s);

    const auto length = static_cast<std::int64_t>(sweep_.size());
    std::vector<int> lags;
    for (int k = 0; k < kPulses; ++k) {
        const std::int64_t start = lead_ + k * spacing_;
        double best = 0.0;
        int bestLag = -1;
        for (int lag = 0; lag <= maxLag_; ++lag) {
            const std::int64_t base = start + lag;
            if (base + length > static_cast<std::int64_t>(x.size())) break;
            double c = 0.0;
            const float* segment = x.data() + base;
            for (std::int64_t n = 0; n < length; ++n) c += static_cast<double>(sweep_[static_cast<std::size_t>(n)]) * segment[n];
            if (std::fabs(c) > best) {  // Betrag: manche Geraete drehen die Polaritaet um
                best = std::fabs(c);
                bestLag = lag;
            }
        }
        if (bestLag < 0) continue;
        const std::int64_t base = start + bestLag;
        const double segmentEnergy = energy[static_cast<std::size_t>(base + length)] - energy[static_cast<std::size_t>(base)];
        const double rho = segmentEnergy > 0.0 ? best / std::sqrt(sweepEnergy * segmentEnergy) : 0.0;
        if (rho >= 0.3) lags.push_back(bestLag);  // 1.0 waere eine perfekte Kopie
    }

    result.validPulses = static_cast<int>(lags.size());
    if (lags.size() < 4) {
        char text[300];
        std::snprintf(text, sizeof text,
                      "Das Testsignal wurde nur %d von %d Mal sicher erkannt. Pegel prüfen (Gain etwas höher, "
                      "aber ohne Übersteuerung) und den MIX-Regler der Soundkarte ganz auf Wiedergabe drehen.",
                      result.validPulses, kPulses);
        result.message = text;
        return result;
    }

    std::sort(lags.begin(), lags.end());
    result.spread = lags.back() - lags.front();
    const double spreadMs = 1000.0 * result.spread / sampleRate_;

    // Bis 3 ms Streuung gibt es ein Ergebnis. Ueber Kabel und mit einem Geraet
    // fuer Ein- und Ausgang sind es 0 bis 3 Samples; durch die Luft oder mit zwei
    // Geraeten (zwei Uhren) streut es etwas mehr.
    if (spreadMs > 3.0) {
        char text[400];
        std::snprintf(text, sizeof text,
                      "Die Einzelmessungen weichen um %d Samples (%.1f ms) voneinander ab. Das ist zu viel für "
                      "ein verlässliches Ergebnis. Geräusche im Raum, ein zu leises Signal oder zwei Geräte "
                      "mit eigener Uhr können die Ursache sein.",
                      result.spread, spreadMs);
        result.message = text;
        return result;
    }

    result.frames = lags[lags.size() / 2];  // Median
    result.ok = true;
    std::string notes;
    if (result.spread > 3) {
        char text[400];
        std::snprintf(text, sizeof text,
                      "Hinweis: Die Einzelmessungen streuen um %.1f ms. Das ist typisch, wenn über Lautsprecher und "
                      "Mikrofon oder mit zwei verschiedenen Geräten gemessen wird. Laufen Ein- und Ausgang über "
                      "verschiedene Geräte, kann sich die Latenz mit der Zeit leicht verschieben.",
                      spreadMs);
        notes = text;
    }
    if (result.inputPeak > 0.98f) {
        if (!notes.empty()) notes += "\n";
        notes += "Hinweis: Der Eingang war übersteuert. Das Ergebnis ist trotzdem verwertbar.";
    }
    result.message = notes;
    return result;
}
