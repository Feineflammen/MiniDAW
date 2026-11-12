#pragma once

#include <cstdint>

// Eine kurze MIDI-Nachricht (Note, Controller, Pitchbend ...), ohne SysEx.
struct MidiMessage {
    std::uint8_t status = 0;
    std::uint8_t data1 = 0;
    std::uint8_t data2 = 0;

    static constexpr MidiMessage noteOn(int note, int velocity, int channel = 0) {
        return {static_cast<std::uint8_t>(0x90 | (channel & 0x0F)), static_cast<std::uint8_t>(note & 0x7F),
                static_cast<std::uint8_t>(velocity & 0x7F)};
    }
    static constexpr MidiMessage noteOff(int note, int channel = 0) {
        return {static_cast<std::uint8_t>(0x80 | (channel & 0x0F)), static_cast<std::uint8_t>(note & 0x7F), 0};
    }
    static constexpr MidiMessage allNotesOff(int channel = 0) {
        return {static_cast<std::uint8_t>(0xB0 | (channel & 0x0F)), 123, 0};
    }

    constexpr int type() const { return status & 0xF0; }
    constexpr bool isNoteOn() const { return type() == 0x90 && data2 > 0; }
    constexpr bool isNoteOff() const { return type() == 0x80 || (type() == 0x90 && data2 == 0); }
};
