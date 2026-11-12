#include "midi/MidiOutput.h"

#include <algorithm>
#include <chrono>

MidiOutput::MidiOutput(Engine& engine) : engine_(engine), thread_([this] { run(); }) {}

MidiOutput::~MidiOutput() {
    running_.store(false);
    if (thread_.joinable()) thread_.join();
    select(L"");
}

std::vector<std::wstring> MidiOutput::devices() {
    std::vector<std::wstring> names;
    const UINT count = midiOutGetNumDevs();
    for (UINT i = 0; i < count; ++i) {
        MIDIOUTCAPSW caps{};
        if (midiOutGetDevCapsW(i, &caps, sizeof caps) == MMSYSERR_NOERROR) names.emplace_back(caps.szPname);
        else names.emplace_back(L"MIDI-Ausgang " + std::to_wstring(i + 1));
    }
    return names;
}

bool MidiOutput::select(const std::wstring& name) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (handle_) {
        midiOutReset(handle_);  // alle Noten aus
        midiOutClose(handle_);
        handle_ = nullptr;
    }
    current_.clear();
    if (name.empty()) return true;

    const auto names = devices();
    for (std::size_t i = 0; i < names.size(); ++i) {
        if (names[i] != name) continue;
        if (midiOutOpen(&handle_, static_cast<UINT>(i), 0, 0, CALLBACK_NULL) == MMSYSERR_NOERROR) {
            current_ = name;
            return true;
        }
        handle_ = nullptr;
        return false;
    }
    return false;
}

void MidiOutput::cycle() {
    const auto names = devices();
    const std::wstring now = current();
    // Reihenfolge: keins -> Geraet 1 -> Geraet 2 ... -> keins. Nicht zu oeffnende werden uebersprungen.
    std::size_t next = 0;
    if (!now.empty()) {
        const auto it = std::find(names.begin(), names.end(), now);
        next = it == names.end() ? 0 : static_cast<std::size_t>(it - names.begin()) + 1;
    }
    for (; next < names.size(); ++next)
        if (select(names[next])) return;
    select(L"");
}

std::wstring MidiOutput::current() {
    std::lock_guard<std::mutex> lock(mutex_);
    return current_;
}

void MidiOutput::run() {
    timeBeginPeriod(1);  // Sleep(1) soll wirklich etwa 1 ms dauern
    std::vector<MidiOutEvent> pending;
    pending.reserve(1024);

    while (running_.load(std::memory_order_relaxed)) {
        MidiOutEvent e;
        while (engine_.popMidiOut(e)) pending.push_back(e);

        if (!pending.empty()) {
            const std::int64_t now = std::chrono::duration_cast<std::chrono::microseconds>(
                                         std::chrono::steady_clock::now().time_since_epoch())
                                         .count();
            std::lock_guard<std::mutex> lock(mutex_);
            // Faellige Nachrichten in der Reihenfolge ihres Eintreffens senden.
            for (auto it = pending.begin(); it != pending.end();) {
                if (it->dueMicros > now) { ++it; continue; }
                if (handle_) {
                    const DWORD packed = static_cast<DWORD>(it->message.status) |
                                         (static_cast<DWORD>(it->message.data1) << 8) |
                                         (static_cast<DWORD>(it->message.data2) << 16);
                    midiOutShortMsg(handle_, packed);
                }
                it = pending.erase(it);
            }
        }
        Sleep(1);
    }
    timeEndPeriod(1);
}
