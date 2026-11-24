#pragma once

#include <cstdint>
#include <filesystem>
#include <functional>
#include <string>
#include <vector>

// Wandelt die Samplerate um (z. B. 44100 <-> 48000 <-> 96000).
//
// Bandbegrenzte Interpolation mit Kaiser-gefenstertem Sinc, polyphas: Fuer das
// Verhaeltnis L/M (gekuerzt) gibt es L Teilfilter, bei krummen Raten werden
// 2048 Teilfilter linear interpoliert. Durchlass bis 92 % der kleineren
// Nyquist-Frequenz (20,3 kHz bei 44,1 kHz), Sperrbereich ab der Nyquist-
// Frequenz mit mehr als 100 dB Daempfung. Keine Verzoegerung: Ausgangs-Frame n
// liegt zeitlich genau bei Eingangs-Frame n * M / L.
//
// Nicht fuer den Audio-Thread (fordert Speicher an).
class Resampler {
public:
    Resampler(int inRate, int outRate, int channels);

    // Haengt die Ausgabe zu den verschachtelten Eingangsframes an out an.
    void push(const float* in, std::int64_t frames, std::vector<float>& out);
    // Schreibt den Rest, sodass insgesamt outputFrames(eingegangene Frames) entstehen.
    void finish(std::vector<float>& out);

    int channels() const noexcept { return channels_; }
    // Wie viele Ausgangsframes zu inFrames Eingangsframes gehoeren.
    static std::int64_t outputFrames(std::int64_t inFrames, int inRate, int outRate) noexcept;
    // Rechnet eine Position (in Frames) auf eine andere Rate um.
    static std::int64_t convertPosition(std::int64_t frames, double fromRate, double toRate) noexcept;

private:
    void produce(std::int64_t available, std::int64_t limit, std::vector<float>& out);

    int channels_;
    std::int64_t up_ = 1, down_ = 1;  // L und M
    int phases_ = 1;                  // Teilfilter in der Tabelle
    bool exact_ = true;               // Tabelle hat alle L Phasen
    int half_ = 1;                    // halbe Filterlaenge in Eingangsframes
    std::vector<float> table_;        // phases_ + 1 Zeilen zu je 2 * half_ Werten
    std::vector<float> mixed_;        // interpolierte Zeile (krumme Raten)
    std::vector<float> buffer_;       // Eingang ab bufferStart_, verschachtelt
    std::int64_t bufferStart_ = 0;
    std::int64_t received_ = 0;       // Eingangsframes bisher
    std::int64_t produced_ = 0;       // Ausgangsframes bisher
};

// Schreibt eine umgerechnete Kopie einer WAV-Datei (32-Bit-Float, Kanalzahl wie
// das Original, hoechstens Stereo). progress bekommt 0..1 und darf mit false abbrechen.
bool resampleWavFile(const std::filesystem::path& source, const std::filesystem::path& target, int rate,
                     std::string& error, const std::function<bool(double)>& progress = {});
