#pragma once

#include "midi/MidiMessage.h"

#include <cstdint>
#include <cstring>
#include <vector>

// Vom Benutzer gelernte MIDI-Zuweisungen eines Plugins: Controller -> Parameter.
// Sie werden hinter den Zustand des Plugins gehaengt, mit einer Kennung am
// Ende, so dass ein Zustand ohne Zuweisungen unveraendert bleibt.
//
// Controller: 0..127 = CC, 128 = Aftertouch (Kanaldruck), 129 = Pitchbend
// (dieselben Nummern wie in VST3 fuer IMidiMapping).
namespace midilearn {

constexpr int kAftertouch = 128;
constexpr int kPitchBend = 129;
constexpr int kControllers = 130;
constexpr int kMaxAssignments = 64;

struct Assignment {
    int controller = 0;
    std::uint32_t param = 0;
    bool operator==(const Assignment&) const = default;
};

inline constexpr char kMagic[8] = {'M', 'D', 'A', 'W', 'Z', 'U', 'W', '1'};

// Haengt die Zuweisungen an state an (nichts, wenn es keine gibt).
inline void append(std::vector<char>& state, const std::vector<Assignment>& assignments) {
    if (assignments.empty()) return;
    const auto put = [&](std::uint32_t v, int bytes) {
        for (int i = 0; i < bytes; ++i) state.push_back(static_cast<char>((v >> (8 * i)) & 0xFF));
    };
    for (const Assignment& a : assignments) {
        put(static_cast<std::uint32_t>(a.controller), 2);
        put(a.param, 4);
    }
    put(static_cast<std::uint32_t>(assignments.size()), 4);
    state.insert(state.end(), kMagic, kMagic + 8);
}

// Nimmt angehaengte Zuweisungen von state ab und liefert sie. Ohne Kennung
// bleibt state unveraendert und das Ergebnis ist leer.
inline std::vector<Assignment> extract(std::vector<char>& state) {
    std::vector<Assignment> out;
    if (state.size() < 12 || std::memcmp(state.data() + state.size() - 8, kMagic, 8) != 0) return out;
    const auto get = [&](std::size_t at, int bytes) {
        std::uint32_t v = 0;
        for (int i = 0; i < bytes; ++i)
            v |= static_cast<std::uint32_t>(static_cast<unsigned char>(state[at + static_cast<std::size_t>(i)])) << (8 * i);
        return v;
    };
    const std::uint32_t count = get(state.size() - 12, 4);
    if (count == 0 || count > static_cast<std::uint32_t>(kMaxAssignments)) return out;
    const std::size_t bytes = static_cast<std::size_t>(count) * 6 + 12;
    if (bytes > state.size()) return out;
    const std::size_t start = state.size() - bytes;
    for (std::uint32_t i = 0; i < count; ++i) {
        Assignment a;
        a.controller = static_cast<int>(get(start + i * 6, 2));
        a.param = get(start + i * 6 + 2, 4);
        if (a.controller >= 0 && a.controller < kControllers) out.push_back(a);
    }
    state.resize(start);
    return out;
}

// Name eines Controllers fuer die Anzeige.
inline const wchar_t* controllerName(int controller) { return ::controllerName(controller); }

}  // namespace midilearn
