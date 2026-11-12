#include "midi/MidiInput.h"

int MidiInput::open() {
    close();
    const UINT count = midiInGetNumDevs();
    for (UINT i = 0; i < count; ++i) {
        HMIDIIN handle = nullptr;
        if (midiInOpen(&handle, i, reinterpret_cast<DWORD_PTR>(&MidiInput::callback),
                       reinterpret_cast<DWORD_PTR>(this), CALLBACK_FUNCTION) != MMSYSERR_NOERROR)
            continue;
        if (midiInStart(handle) != MMSYSERR_NOERROR) {
            midiInClose(handle);
            continue;
        }
        handles_.push_back(handle);

        MIDIINCAPSW caps{};
        if (midiInGetDevCapsW(i, &caps, sizeof caps) == MMSYSERR_NOERROR) {
            if (!names_.empty()) names_ += L", ";
            names_ += caps.szPname;
        }
    }
    return static_cast<int>(handles_.size());
}

void MidiInput::close() {
    for (HMIDIIN handle : handles_) {
        midiInStop(handle);
        midiInReset(handle);
        midiInClose(handle);
    }
    handles_.clear();
    names_.clear();
}

// Laeuft in einem Thread des MIDI-Treibers.
void CALLBACK MidiInput::callback(HMIDIIN, UINT message, DWORD_PTR instance, DWORD_PTR param1, DWORD_PTR) {
    if (message != MIM_DATA) return;
    auto* self = reinterpret_cast<MidiInput*>(instance);
    const auto status = static_cast<std::uint8_t>(param1 & 0xFF);
    if (status < 0x80 || status >= 0xF0) return;  // nur Kanalnachrichten
    MidiMessage m;
    m.status = status;
    m.data1 = static_cast<std::uint8_t>((param1 >> 8) & 0x7F);
    m.data2 = static_cast<std::uint8_t>((param1 >> 16) & 0x7F);
    self->engine_.postMidi(m);
}
