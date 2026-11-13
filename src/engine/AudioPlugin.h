#pragma once

#include "midi/MidiMessage.h"

#include <cstdint>

// Ein MIDI-Ereignis innerhalb eines Blocks.
struct PluginMidiEvent {
    int offset = 0;  // Frame innerhalb des Blocks
    MidiMessage message;
};

// Was ein Plugin ueber den Transport wissen darf.
struct PluginTransport {
    bool playing = false;
    std::int64_t position = 0;  // Frames auf der Zeitachse
    double tempo = 120.0;
    double sampleRate = 44100.0;
};

// Ein Instrument oder Effekt auf einer Spur, aus Sicht der Engine.
//
// Die Engine besitzt Plugins nicht: Sie bekommt einen Zeiger, benutzt ihn im
// Audio-Thread und meldet ueber Engine::popReleasedPlugin(), wenn sie ihn
// nicht mehr anfasst. Erst dann darf der Besitzer das Plugin zerstoeren.
class AudioPlugin {
public:
    virtual ~AudioPlugin() = default;

    // Instrument: erzeugt Klang aus MIDI. Sonst Effekt: bearbeitet den Klang der Spur.
    virtual bool isInstrument() const noexcept = 0;

    // Wie viele Frames das Plugin den Klang verzoegert (fuer den Latenzausgleich).
    virtual int latencyFrames() const noexcept { return 0; }

    // Audio-Thread. left/right enthalten den Eingang (beim Instrument Stille)
    // und werden mit dem Ausgang ueberschrieben. Die Ereignisse sind nach
    // offset geordnet. Darf weder warten noch Speicher anfordern.
    virtual void process(float* left, float* right, int numFrames, const PluginMidiEvent* events,
                         int numEvents, const PluginTransport& transport) noexcept = 0;
};
