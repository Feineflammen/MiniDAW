#pragma once

#include "engine/Engine.h"

#include <windows.h>
#include <mmsystem.h>

#include <string>
#include <vector>

// Oeffnet alle angeschlossenen MIDI-Eingaenge (WinMM) und reicht ihre
// Nachrichten an die Engine weiter.
class MidiInput {
public:
    explicit MidiInput(Engine& engine) : engine_(engine) {}
    ~MidiInput() { close(); }
    MidiInput(const MidiInput&) = delete;
    MidiInput& operator=(const MidiInput&) = delete;

    // Liefert die Anzahl der geoeffneten Geraete.
    int open();
    void close();

    // Namen der geoeffneten Geraete, durch Komma getrennt.
    const std::wstring& names() const { return names_; }

private:
    static void CALLBACK callback(HMIDIIN handle, UINT message, DWORD_PTR instance, DWORD_PTR param1,
                                  DWORD_PTR param2);

    Engine& engine_;
    std::vector<HMIDIIN> handles_;
    std::wstring names_;
};
