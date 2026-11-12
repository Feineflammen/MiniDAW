#include "midi/MidiInput.h"

int MidiInput::open() {
    close();
    const UINT count = midiInGetNumDevs();
    knownDevices_ = count;
    summary_.clear();

    for (UINT i = 0; i < count; ++i) {
        MIDIINCAPSW caps{};
        std::wstring name = L"MIDI-Ger\u00E4t " + std::to_wstring(i + 1);
        if (midiInGetDevCapsW(i, &caps, sizeof caps) == MMSYSERR_NOERROR) name = caps.szPname;

        HMIDIIN handle = nullptr;
        const MMRESULT result = midiInOpen(&handle, i, reinterpret_cast<DWORD_PTR>(&MidiInput::callback),
                                           reinterpret_cast<DWORD_PTR>(this), CALLBACK_FUNCTION);
        if (result == MMSYSERR_NOERROR && midiInStart(handle) == MMSYSERR_NOERROR) {
            handles_.push_back(handle);
        } else {
            if (result == MMSYSERR_NOERROR) midiInClose(handle);
            // Der haeufigste Grund: Ein anderes Programm hat das Geraet geoeffnet.
            name += result == MMSYSERR_ALLOCATED ? L" (von anderem Programm belegt)" : L" (nicht zu \u00F6ffnen)";
        }
        if (!summary_.empty()) summary_ += L", ";
        summary_ += name;
    }
    if (summary_.empty()) summary_ = L"kein MIDI-Ger\u00E4t";
    return static_cast<int>(handles_.size());
}

void MidiInput::close() {
    for (HMIDIIN handle : handles_) {
        midiInStop(handle);
        midiInReset(handle);
        midiInClose(handle);
    }
    handles_.clear();
}

bool MidiInput::refresh() {
    if (midiInGetNumDevs() == knownDevices_) return false;
    open();
    return true;
}

// Laeuft in einem Thread des MIDI-Treibers.
void CALLBACK MidiInput::callback(HMIDIIN, UINT message, DWORD_PTR instance, DWORD_PTR param1, DWORD_PTR) {
    auto* self = reinterpret_cast<MidiInput*>(instance);
    if (message == MIM_ERROR || message == MIM_LONGERROR) {
        self->engine_.reportMidiError();
        return;
    }
    if (message != MIM_DATA) return;
    const auto status = static_cast<std::uint8_t>(param1 & 0xFF);
    if (status < 0x80) { self->engine_.reportMidiError(); return; }
    if (status >= 0xF0) return;  // Systemnachrichten (Clock, Active Sensing) ignorieren
    MidiMessage m;
    m.status = status;
    m.data1 = static_cast<std::uint8_t>((param1 >> 8) & 0x7F);
    m.data2 = static_cast<std::uint8_t>((param1 >> 16) & 0x7F);
    self->engine_.postMidi(m);
}
