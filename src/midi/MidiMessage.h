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

// Controller-Nummern wie bei VST3 (und model/Project.h): 0..127 Regler (CC),
// 128 Kanaldruck (Aftertouch), 129 Pitchbend (Wert 0..16383, Mitte 8192).
constexpr MidiMessage controlMessage(int controller, int value, int channel = 0) {
    const auto ch = static_cast<std::uint8_t>(channel & 0x0F);
    if (controller == 129)
        return {static_cast<std::uint8_t>(0xE0 | ch), static_cast<std::uint8_t>(value & 0x7F),
                static_cast<std::uint8_t>((value >> 7) & 0x7F)};
    if (controller == 128) return {static_cast<std::uint8_t>(0xD0 | ch), static_cast<std::uint8_t>(value & 0x7F), 0};
    return {static_cast<std::uint8_t>(0xB0 | ch), static_cast<std::uint8_t>(controller & 0x7F),
            static_cast<std::uint8_t>(value & 0x7F)};
}

// Controller einer Nachricht oder -1 (Noten, Kanalbefehle 120..127 usw.).
constexpr int controllerOf(const MidiMessage& m) {
    if (m.type() == 0xB0) return m.data1 < 120 ? m.data1 : -1;
    if (m.type() == 0xD0) return 128;
    if (m.type() == 0xE0) return 129;
    return -1;
}

constexpr int controlValueOf(const MidiMessage& m) {
    if (m.type() == 0xE0) return (m.data2 << 7) | m.data1;
    if (m.type() == 0xD0) return m.data1;
    return m.data2;
}

// Name eines Controllers fuer die Anzeige, nullptr fuer Nummern ohne festen Namen.
constexpr const wchar_t* controllerName(int controller) {
    switch (controller) {
        case 1: return L"Modulation";
        case 2: return L"Breath";
        case 4: return L"Fu\u00DFregler";
        case 7: return L"Lautst\u00E4rke";
        case 10: return L"Panorama";
        case 11: return L"Expression";
        case 64: return L"Sustain";
        case 65: return L"Portamento";
        case 66: return L"Sostenuto";
        case 67: return L"Soft-Pedal";
        case 128: return L"Aftertouch";
        case 129: return L"Pitchbend";
        default: return nullptr;
    }
}

// Ruhewert, auf den beim Anhalten und am Clip-Ende zurueckgestellt wird, damit
// kein Pedal haengt und die Tonhoehe stimmt. -1: nicht zuruecksetzen (z. B.
// Lautstaerke CC 7, deren "richtiger" Wert nicht 0 ist).
constexpr int controlRestValue(int controller) {
    if (controller == 129) return 8192;
    if (controller == 128 || controller == 1 || (controller >= 64 && controller <= 69)) return 0;
    return -1;
}
