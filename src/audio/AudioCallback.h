#pragma once

// Schnittstelle zwischen Audiogerät und Engine.
// process() läuft im Audio-Thread: keine Allokation, keine Locks, kein I/O.
class AudioCallback {
public:
    virtual ~AudioCallback() = default;

    // Wird vor dem Start einmal aus dem aufrufenden Thread gerufen.
    // maxFrames ist die größte Blocklänge, die process() je bekommt.
    virtual void prepare(double sampleRate, int numChannels, int maxFrames) = 0;

    // out[kanal][frame], 32-Bit-Float, nicht verschachtelt.
    // Die Puffer sind beim Aufruf mit Stille gefüllt.
    virtual void process(float* const* out, int numChannels, int numFrames) noexcept = 0;
};
