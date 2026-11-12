#include "engine/Synth.h"

#include <algorithm>
#include <cmath>

namespace {

constexpr float kSustain = 0.6f;
constexpr float kLevel = 0.22f;

// Glaettet den Sprung des Saegezahns, damit er nicht klirrt (PolyBLEP).
float polyBlep(float t, float dt) noexcept {
    if (t < dt) {
        t /= dt;
        return t + t - t * t - 1.0f;
    }
    if (t > 1.0f - dt) {
        t = (t - 1.0f) / dt;
        return t * t + t + t + 1.0f;
    }
    return 0.0f;
}

}  // namespace

void Synth::prepare(double sampleRate) noexcept {
    sampleRate_ = sampleRate;
    const auto rate = static_cast<float>(sampleRate);
    attackInc_ = 1.0f / (0.004f * rate);            // 4 ms
    decayCoef_ = std::exp(-1.0f / (0.25f * rate));  // Zeitkonstante 250 ms
    releaseCoef_ = std::exp(-1.0f / (0.06f * rate));
    for (auto& v : voices_) v = Voice{};
    activeVoices_ = 0;
    sustainPedal_ = false;
}

void Synth::handle(MidiMessage m) noexcept {
    if (m.isNoteOn()) {
        noteOn(m.data1, m.data2);
    } else if (m.isNoteOff()) {
        noteOff(m.data1);
    } else if (m.type() == 0xB0) {
        if (m.data1 == 64) {  // Haltepedal
            sustainPedal_ = m.data2 >= 64;
            if (!sustainPedal_)
                for (auto& v : voices_)
                    if (v.stage != Stage::Off && !v.keyDown) release(v);
        } else if (m.data1 == 120 || m.data1 == 123) {
            allNotesOff();
        }
    }
}

void Synth::noteOn(int note, int velocity) noexcept {
    // Freie Stimme suchen, sonst die aelteste uebernehmen.
    Voice* voice = nullptr;
    for (auto& v : voices_)
        if (v.stage == Stage::Off) { voice = &v; break; }
    if (!voice) {
        voice = &voices_[0];
        for (auto& v : voices_)
            if (v.age < voice->age) voice = &v;
    } else {
        ++activeVoices_;
    }

    const double hz = 440.0 * std::pow(2.0, (note - 69) / 12.0);
    const double cutoff = std::min(hz * 5.0 + 300.0, 0.4 * sampleRate_);
    voice->stage = Stage::Attack;
    voice->keyDown = true;
    voice->note = note;
    voice->phase = 0.5f;  // Nulldurchgang des Saegezahns: kein Knack beim Einsatz
    voice->phaseInc = static_cast<float>(hz / sampleRate_);
    voice->env = 0.0f;
    voice->level = kLevel * static_cast<float>(velocity) / 127.0f;
    voice->filter = 0.0f;
    voice->filterCoef = static_cast<float>(1.0 - std::exp(-6.283185307179586 * cutoff / sampleRate_));
    voice->age = ++counter_;
}

void Synth::noteOff(int note) noexcept {
    for (auto& v : voices_) {
        if (v.stage != Stage::Off && v.keyDown && v.note == note) {
            v.keyDown = false;
            if (!sustainPedal_) release(v);
        }
    }
}

void Synth::release(Voice& voice) noexcept {
    if (voice.stage != Stage::Off) voice.stage = Stage::Release;
}

void Synth::allNotesOff() noexcept {
    sustainPedal_ = false;
    for (auto& v : voices_) {
        v.keyDown = false;
        release(v);
    }
}

void Synth::render(float* left, float* right, int numFrames) noexcept {
    if (activeVoices_ == 0) return;

    for (auto& v : voices_) {
        if (v.stage == Stage::Off) continue;

        for (int i = 0; i < numFrames; ++i) {
            switch (v.stage) {
                case Stage::Attack:
                    v.env += attackInc_;
                    if (v.env >= 1.0f) { v.env = 1.0f; v.stage = Stage::Decay; }
                    break;
                case Stage::Decay:
                    v.env = kSustain + (v.env - kSustain) * decayCoef_;
                    break;
                case Stage::Release:
                    v.env *= releaseCoef_;
                    break;
                case Stage::Off:
                    break;
            }
            if (v.stage == Stage::Release && v.env < 0.0001f) {
                v.stage = Stage::Off;
                --activeVoices_;
                break;
            }

            const float saw = 2.0f * v.phase - 1.0f - polyBlep(v.phase, v.phaseInc);
            v.phase += v.phaseInc;
            if (v.phase >= 1.0f) v.phase -= 1.0f;

            v.filter += v.filterCoef * (saw - v.filter);
            const float s = v.filter * v.env * v.level;
            left[i] += s;
            right[i] += s;
        }
    }
}
