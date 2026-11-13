#pragma once

#include <algorithm>
#include <cmath>

// Kennlinie der Fader: Stellung 0..1 entspricht -60..+6 dB, ganz unten ist stumm.
// Die Automation der Lautstaerke speichert Stellungen, damit Rampen im Ohr
// gleichmaessig klingen und auf dem Bildschirm gerade aussehen.
namespace fader {

constexpr float kMinDb = -60.0f;
constexpr float kMaxDb = 6.0f;

inline float gainToPosition(float gain) {
    if (gain <= 0.0f) return 0.0f;
    const float db = 20.0f * std::log10(gain);
    return std::clamp((db - kMinDb) / (kMaxDb - kMinDb), 0.0f, 1.0f);
}

inline float positionToGain(float position) {
    if (position <= 0.005f) return 0.0f;
    const float db = kMinDb + std::min(position, 1.0f) * (kMaxDb - kMinDb);
    return std::pow(10.0f, db / 20.0f);
}

}  // namespace fader
