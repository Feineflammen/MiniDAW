#pragma once

// Schnittstelle zwischen Audiogeraet und Engine.
// process() laeuft im Audio-Thread: keine Allokation, keine Locks, kein I/O.
class AudioCallback {
public:
    virtual ~AudioCallback() = default;

    // Wird vor dem Start einmal aus dem aufrufenden Thread gerufen.
    // maxFrames ist die groesste Blocklaenge, die process() je bekommt.
    // numInputs ist 0, wenn es keinen Eingang gibt.
    virtual void prepare(double sampleRate, int numInputs, int numOutputs, int maxFrames) = 0;

    // in[kanal][frame] und out[kanal][frame], 32-Bit-Float, nicht verschachtelt.
    // Die Ausgangspuffer sind beim Aufruf mit Stille gefuellt.
    // Ohne Eingang ist numInputs 0 und in darf nullptr sein.
    virtual void process(const float* const* in, int numInputs, float* const* out, int numOutputs,
                         int numFrames) noexcept = 0;
};
