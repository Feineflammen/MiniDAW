#pragma once

#include "midi/MidiMessage.h"

#include <array>
#include <cstdint>

// Einfacher eingebauter Synthesizer: Saegezahn mit Tiefpass und Huellkurve,
// 16 Stimmen. Er dient als Klangquelle fuer MIDI, solange auf einer Spur kein
// VST-Instrument sitzt. Alles hier laeuft im Audio-Thread.
class Synth {
public:
    static constexpr int kVoices = 16;

    void prepare(double sampleRate) noexcept;
    void handle(MidiMessage message) noexcept;
    void allNotesOff() noexcept;  // laesst alle Stimmen ausklingen
    bool active() const noexcept { return activeVoices_ > 0; }

    // Addiert den Klang auf left und right.
    void render(float* left, float* right, int numFrames) noexcept;

private:
    enum class Stage : std::uint8_t { Off, Attack, Decay, Release };

    struct Voice {
        Stage stage = Stage::Off;
        bool keyDown = false;
        int note = 0;
        float phase = 0.0f;
        float phaseInc = 0.0f;
        float env = 0.0f;
        float level = 0.0f;
        float filter = 0.0f;
        float filterCoef = 0.0f;
        std::uint32_t age = 0;
    };

    void noteOn(int note, int velocity) noexcept;
    void noteOff(int note) noexcept;
    void release(Voice& voice) noexcept;

    std::array<Voice, kVoices> voices_{};
    double sampleRate_ = 44100.0;
    float attackInc_ = 0.0f;
    float decayCoef_ = 0.0f;
    float releaseCoef_ = 0.0f;
    bool sustainPedal_ = false;
    int activeVoices_ = 0;
    std::uint32_t counter_ = 0;
};
