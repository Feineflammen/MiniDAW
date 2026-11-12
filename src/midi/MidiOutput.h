#pragma once

#include "engine/Engine.h"

#include <windows.h>
#include <mmsystem.h>

#include <atomic>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

// Sendet die MIDI-Nachrichten der Engine an ein MIDI-Ausgabegeraet (WinMM).
//
// Ein eigener Thread holt die Nachrichten ab und schickt jede zu ihrem
// Zeitstempel los, auf etwa eine Millisekunde genau. Der Audio-Thread ruft
// nie selbst in den MIDI-Treiber.
class MidiOutput {
public:
    explicit MidiOutput(Engine& engine);
    ~MidiOutput();
    MidiOutput(const MidiOutput&) = delete;
    MidiOutput& operator=(const MidiOutput&) = delete;

    // Namen aller vorhandenen Ausgabegeraete.
    static std::vector<std::wstring> devices();

    // Oeffnet das Geraet mit diesem Namen; ein leerer Name schliesst die Ausgabe.
    bool select(const std::wstring& name);
    // Schaltet zum naechsten Geraet weiter (nach dem letzten kommt "keins").
    void cycle();

    // Name des offenen Geraets oder leer.
    std::wstring current();

private:
    void run();

    Engine& engine_;
    std::mutex mutex_;  // schuetzt handle_ und current_
    HMIDIOUT handle_ = nullptr;
    std::wstring current_;
    std::atomic<bool> running_{true};
    std::thread thread_;
};
