#include "model/ProjectFile.h"

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <sstream>

namespace {

const char* const kMagic = "MiniDAW-Projekt";
constexpr int kVersion = 2;  // 2: Effekt-Plaetze, Sends, Busse

std::string pathToUtf8(const std::filesystem::path& path) {
    const std::u8string u8 = path.generic_u8string();
    return std::string(reinterpret_cast<const char*>(u8.data()), u8.size());
}

std::filesystem::path pathFromUtf8(const std::string& text) {
    return std::filesystem::path(std::u8string(reinterpret_cast<const char8_t*>(text.data()), text.size()));
}

// Pfade im Projektordner relativ ablegen, damit das Projekt samt Ordner umziehen kann.
std::string storedPath(const std::filesystem::path& file, const std::filesystem::path& projectFolder) {
    const std::filesystem::path relative = file.lexically_relative(projectFolder);
    if (!relative.empty() && !relative.native().empty() && relative.begin()->native() != std::filesystem::path("..").native())
        return pathToUtf8(relative);
    return pathToUtf8(file);
}

std::filesystem::path resolvedPath(const std::string& stored, const std::filesystem::path& projectFolder) {
    const std::filesystem::path path = pathFromUtf8(stored);
    return path.is_absolute() ? path : (projectFolder / path).lexically_normal();
}

std::string formatNumber(double value) {
    char buffer[64];
    std::snprintf(buffer, sizeof buffer, "%.9g", value);
    return buffer;
}

std::string toHex(const std::vector<char>& data) {
    static const char kDigits[] = "0123456789abcdef";
    std::string out;
    out.reserve(data.size() * 2);
    for (const char c : data) {
        const auto byte = static_cast<unsigned char>(c);
        out.push_back(kDigits[byte >> 4]);
        out.push_back(kDigits[byte & 15]);
    }
    return out;
}

bool fromHex(const std::string& text, std::vector<char>& out) {
    const auto digit = [](char c) -> int {
        if (c >= '0' && c <= '9') return c - '0';
        if (c >= 'a' && c <= 'f') return c - 'a' + 10;
        if (c >= 'A' && c <= 'F') return c - 'A' + 10;
        return -1;
    };
    out.clear();
    if (text.size() % 2 != 0) return false;
    out.reserve(text.size() / 2);
    for (std::size_t i = 0; i < text.size(); i += 2) {
        const int hi = digit(text[i]), lo = digit(text[i + 1]);
        if (hi < 0 || lo < 0) return false;
        out.push_back(static_cast<char>((hi << 4) | lo));
    }
    return true;
}

std::string inputName(int input) {
    if (input == kTrackInputAudio) return "audio";
    if (input == kTrackInputMidiAll) return "midi-alle";
    return "midi-" + std::to_string(input + 1);
}

int inputFromName(const std::string& name) {
    if (name == "midi-alle") return kTrackInputMidiAll;
    if (name.rfind("midi-", 0) == 0) {
        const int channel = std::atoi(name.c_str() + 5);
        if (channel >= 1 && channel <= 16) return channel - 1;
    }
    return kTrackInputAudio;
}

// Teilt eine Zeile in Schluesselwort und Rest.
void splitLine(const std::string& line, std::string& key, std::string& rest) {
    std::size_t begin = 0;
    while (begin < line.size() && (line[begin] == ' ' || line[begin] == '\t')) ++begin;
    std::size_t end = begin;
    while (end < line.size() && line[end] != ' ' && line[end] != '\t') ++end;
    key = line.substr(begin, end - begin);
    while (end < line.size() && (line[end] == ' ' || line[end] == '\t')) ++end;
    rest = line.substr(end);
}

// "<zahl> <rest>" zerlegen.
bool numberAndRest(const std::string& text, int& number, std::string& rest) {
    char* end = nullptr;
    const long value = std::strtol(text.c_str(), &end, 10);
    if (end == text.c_str()) return false;
    number = static_cast<int>(value);
    while (*end == ' ') ++end;
    rest = end;
    return true;
}

// Liefert den Eintrag fuer Platz index, legt ihn bei Bedarf an.
SavedSlot& slotEntry(std::vector<SavedSlot>& slots, int index) {
    for (SavedSlot& slot : slots)
        if (slot.slot == index) return slot;
    slots.push_back({});
    slots.back().slot = index;
    return slots.back();
}

}  // namespace

std::string toUtf8(const std::wstring& text) {
    std::string out;
    for (std::size_t i = 0; i < text.size(); ++i) {
        auto code = static_cast<std::uint32_t>(text[i]);
        // UTF-16-Ersatzpaare (Windows) zu einem Zeichen zusammenfassen.
        if (sizeof(wchar_t) == 2 && code >= 0xD800 && code <= 0xDBFF && i + 1 < text.size()) {
            const auto low = static_cast<std::uint32_t>(text[i + 1]);
            if (low >= 0xDC00 && low <= 0xDFFF) {
                code = 0x10000 + ((code - 0xD800) << 10) + (low - 0xDC00);
                ++i;
            }
        }
        if (code < 0x80) {
            out.push_back(static_cast<char>(code));
        } else if (code < 0x800) {
            out.push_back(static_cast<char>(0xC0 | (code >> 6)));
            out.push_back(static_cast<char>(0x80 | (code & 0x3F)));
        } else if (code < 0x10000) {
            out.push_back(static_cast<char>(0xE0 | (code >> 12)));
            out.push_back(static_cast<char>(0x80 | ((code >> 6) & 0x3F)));
            out.push_back(static_cast<char>(0x80 | (code & 0x3F)));
        } else {
            out.push_back(static_cast<char>(0xF0 | (code >> 18)));
            out.push_back(static_cast<char>(0x80 | ((code >> 12) & 0x3F)));
            out.push_back(static_cast<char>(0x80 | ((code >> 6) & 0x3F)));
            out.push_back(static_cast<char>(0x80 | (code & 0x3F)));
        }
    }
    return out;
}

std::wstring fromUtf8(const std::string& text) {
    std::wstring out;
    for (std::size_t i = 0; i < text.size();) {
        const auto c = static_cast<unsigned char>(text[i]);
        std::uint32_t code = c;
        std::size_t length = 1;
        const auto cont = [&](std::size_t k) { return static_cast<std::uint32_t>(static_cast<unsigned char>(text[i + k]) & 0x3F); };
        if (c >= 0xF0 && i + 3 < text.size()) {
            code = ((c & 0x07u) << 18) | (cont(1) << 12) | (cont(2) << 6) | cont(3);
            length = 4;
        } else if (c >= 0xE0 && i + 2 < text.size()) {
            code = ((c & 0x0Fu) << 12) | (cont(1) << 6) | cont(2);
            length = 3;
        } else if (c >= 0xC0 && i + 1 < text.size()) {
            code = ((c & 0x1Fu) << 6) | cont(1);
            length = 2;
        }
        if (code >= 0x10000 && sizeof(wchar_t) == 2) {
            code -= 0x10000;
            out.push_back(static_cast<wchar_t>(0xD800 + (code >> 10)));
            out.push_back(static_cast<wchar_t>(0xDC00 + (code & 0x3FF)));
        } else {
            out.push_back(static_cast<wchar_t>(code));
        }
        i += length;
    }
    return out;
}

namespace {

void writeSlots(std::ostringstream& out, const std::vector<SavedSlot>& slots) {
    for (const SavedSlot& slot : slots) {
        if (slot.slot < 0 || slot.file.empty()) continue;
        out << "  slot " << slot.slot << ' ' << pathToUtf8(slot.file) << '\n';
        out << "  slotname " << slot.slot << ' ' << toUtf8(slot.name) << '\n';
        if (!slot.state.empty()) out << "  slotzustand " << slot.slot << ' ' << toHex(slot.state) << '\n';
    }
}

}  // namespace

bool writeProjectFile(const std::filesystem::path& path, const SavedProject& project, std::string& error) {
    const std::filesystem::path folder = path.parent_path();
    std::ostringstream out;
    out << kMagic << ' ' << kVersion << '\n';
    out << "samplerate " << formatNumber(project.sampleRate) << '\n';
    out << "tempo " << formatNumber(project.bpm) << '\n';
    out << "master " << formatNumber(project.masterGain) << '\n';

    for (int t = 0; t < kProjectTracks; ++t) {
        const SavedTrack& track = project.tracks[static_cast<std::size_t>(t)];
        out << "spur " << (t + 1) << '\n';
        out << "  name " << toUtf8(track.name) << '\n';
        out << "  lautstaerke " << formatNumber(track.gain) << '\n';
        out << "  panorama " << formatNumber(track.pan) << '\n';
        out << "  stumm " << (track.mute ? 1 : 0) << '\n';
        out << "  solo " << (track.solo ? 1 : 0) << '\n';
        out << "  eingang " << inputName(track.input) << '\n';
        // Audio-Eingang: "3" fuer Kanal 3 allein, "3-4" fuer das Paar.
        out << "  audioeingang " << (track.audioChannel + 1);
        if (track.audioStereo) out << '-' << (track.audioChannel + 2);
        out << '\n';
        out << "  midiausgang " << (track.midiOutput == kTrackOutputSynth ? std::string("synth") : std::to_string(track.midiOutput + 1)) << '\n';
        out << "  ausgang " << (track.output < 0 ? std::string("master") : "bus " + std::to_string(track.output + 1)) << '\n';
        for (int b = 0; b < kProjectBuses; ++b)
            if (track.sends[static_cast<std::size_t>(b)] > 0.0f)
                out << "  send " << (b + 1) << ' ' << formatNumber(track.sends[static_cast<std::size_t>(b)]) << '\n';
        writeSlots(out, track.slots);
        for (const SavedClip& clip : track.clips) {
            out << "  audioclip " << clip.start << ' ' << clip.offset << ' ' << clip.length << ' '
                << storedPath(clip.file, folder) << '\n';
        }
        for (const MidiClipModel& clip : track.midiClips) {
            out << "  midiclip " << clip.start << ' ' << clip.length << ' ' << toUtf8(clip.name) << '\n';
            for (const MidiNote& note : clip.notes) {
                out << "    note " << note.start << ' ' << note.length << ' ' << static_cast<int>(note.pitch) << ' '
                    << static_cast<int>(note.velocity) << '\n';
            }
        }
    }

    for (int b = 0; b < kProjectBuses; ++b) {
        const SavedBus& bus = project.buses[static_cast<std::size_t>(b)];
        out << "bus " << (b + 1) << '\n';
        out << "  name " << toUtf8(bus.name) << '\n';
        out << "  lautstaerke " << formatNumber(bus.gain) << '\n';
        out << "  panorama " << formatNumber(bus.pan) << '\n';
        out << "  stumm " << (bus.mute ? 1 : 0) << '\n';
        writeSlots(out, bus.slots);
    }
    if (!project.masterSlots.empty()) {
        out << "masterkanal\n";
        writeSlots(out, project.masterSlots);
    }

    // Erst in eine Nachbardatei schreiben und dann umbenennen: Geht beim
    // Schreiben etwas schief, bleibt die bisherige Projektdatei unversehrt.
    std::filesystem::path temp = path;
    temp += ".neu";
    {
        std::ofstream file(temp, std::ios::binary | std::ios::trunc);
        if (!file) { error = "Die Datei lässt sich nicht anlegen."; return false; }
        const std::string text = out.str();
        file.write(text.data(), static_cast<std::streamsize>(text.size()));
        file.flush();
        if (!file) { error = "Fehler beim Schreiben der Datei."; return false; }
    }
    std::error_code ec;
    std::filesystem::rename(temp, path, ec);
    if (ec) {
        std::filesystem::remove(temp, ec);
        error = "Die Projektdatei lässt sich nicht ersetzen.";
        return false;
    }
    return true;
}

bool readProjectFile(const std::filesystem::path& path, SavedProject& project, std::string& error) {
    std::ifstream file(path, std::ios::binary);
    if (!file) { error = "Die Datei lässt sich nicht öffnen."; return false; }

    const std::filesystem::path folder = path.parent_path();
    project = SavedProject{};
    for (int t = 0; t < kProjectTracks; ++t)
        project.tracks[static_cast<std::size_t>(t)].name = L"Spur " + std::to_wstring(t + 1);

    std::string line, key, rest;
    if (!std::getline(file, line)) { error = "Die Datei ist leer."; return false; }
    if (!line.empty() && line.back() == '\r') line.pop_back();
    splitLine(line, key, rest);
    if (key != kMagic) { error = "Das ist keine MiniDAW-Projektdatei."; return false; }
    if (std::atoi(rest.c_str()) > kVersion) {
        error = "Die Datei stammt von einer neueren Programmversion.";
        return false;
    }

    SavedTrack* track = nullptr;
    SavedBus* bus = nullptr;
    std::vector<SavedSlot>* slots = nullptr;  // Plugin-Plaetze des aktuellen Abschnitts
    MidiClipModel* midiClip = nullptr;
    while (std::getline(file, line)) {
        if (!line.empty() && line.back() == '\r') line.pop_back();
        splitLine(line, key, rest);
        if (key.empty()) continue;

        if (key == "samplerate") project.sampleRate = std::strtod(rest.c_str(), nullptr);
        else if (key == "tempo") project.bpm = std::strtod(rest.c_str(), nullptr);
        else if (key == "master") project.masterGain = static_cast<float>(std::strtod(rest.c_str(), nullptr));
        else if (key == "spur" || key == "bus" || key == "masterkanal") {
            // Neuer Abschnitt.
            const int index = std::atoi(rest.c_str());
            track = nullptr;
            bus = nullptr;
            slots = nullptr;
            midiClip = nullptr;
            if (key == "spur" && index >= 1 && index <= kProjectTracks) {
                track = &project.tracks[static_cast<std::size_t>(index - 1)];
                slots = &track->slots;
            } else if (key == "bus" && index >= 1 && index <= kProjectBuses) {
                bus = &project.buses[static_cast<std::size_t>(index - 1)];
                slots = &bus->slots;
            } else if (key == "masterkanal") {
                slots = &project.masterSlots;
            }
        } else if ((key == "slot" || key == "slotname" || key == "slotzustand") && slots) {
            int index = 0;
            std::string value;
            if (!numberAndRest(rest, index, value) || index < 0 || index >= kSlotsPerStrip) continue;
            if (index == 0 && !track) continue;  // Instrumente nur auf Spuren
            SavedSlot& slot = slotEntry(*slots, index);
            if (key == "slot") slot.file = pathFromUtf8(value);
            else if (key == "slotname") slot.name = fromUtf8(value);
            else if (!fromHex(value, slot.state)) slot.state.clear();
        } else if (bus) {
            if (key == "name") bus->name = fromUtf8(rest);
            else if (key == "lautstaerke") bus->gain = static_cast<float>(std::strtod(rest.c_str(), nullptr));
            else if (key == "panorama") bus->pan = static_cast<float>(std::strtod(rest.c_str(), nullptr));
            else if (key == "stumm") bus->mute = rest == "1";
        } else if (!track) {
            continue;  // Unbekanntes oder Zeilen einer ungueltigen Spur ueberspringen
        } else if (key == "name") track->name = fromUtf8(rest);
        else if (key == "lautstaerke") track->gain = static_cast<float>(std::strtod(rest.c_str(), nullptr));
        else if (key == "panorama") track->pan = static_cast<float>(std::strtod(rest.c_str(), nullptr));
        else if (key == "stumm") track->mute = rest == "1";
        else if (key == "solo") track->solo = rest == "1";
        else if (key == "ausgang") {
            const int index = rest.rfind("bus ", 0) == 0 ? std::atoi(rest.c_str() + 4) : 0;
            track->output = (index >= 1 && index <= kProjectBuses) ? index - 1 : -1;
        } else if (key == "send") {
            int index = 0;
            std::string value;
            if (numberAndRest(rest, index, value) && index >= 1 && index <= kProjectBuses)
                track->sends[static_cast<std::size_t>(index - 1)] =
                    std::max(0.0f, static_cast<float>(std::strtod(value.c_str(), nullptr)));
        }
        else if (key == "eingang") track->input = inputFromName(rest);
        else if (key == "audioeingang") {
            const int first = std::atoi(rest.c_str());
            if (first >= 1 && first <= 64) {
                track->audioChannel = first - 1;
                track->audioStereo = rest.find('-') != std::string::npos;
            }
        }
        else if (key == "midiausgang") {
            const int channel = std::atoi(rest.c_str());
            track->midiOutput = (rest != "synth" && channel >= 1 && channel <= 16) ? channel - 1 : kTrackOutputSynth;
        } else if (key == "plugin") {
            slotEntry(track->slots, -1).file = pathFromUtf8(rest);  // Format 1: ein Plugin je Spur
        } else if (key == "pluginname") {
            slotEntry(track->slots, -1).name = fromUtf8(rest);
        } else if (key == "pluginzustand") {
            SavedSlot& slot = slotEntry(track->slots, -1);
            if (!fromHex(rest, slot.state)) slot.state.clear();
        } else if (key == "audioclip") {
            SavedClip clip;
            char* cursor = nullptr;
            clip.start = std::strtoll(rest.c_str(), &cursor, 10);
            clip.offset = std::strtoll(cursor, &cursor, 10);
            clip.length = std::strtoll(cursor, &cursor, 10);
            while (*cursor == ' ') ++cursor;
            clip.file = resolvedPath(cursor, folder);
            if (clip.length > 0 && *cursor) track->clips.push_back(std::move(clip));
            midiClip = nullptr;
        } else if (key == "midiclip") {
            MidiClipModel clip;
            char* cursor = nullptr;
            clip.start = std::strtoll(rest.c_str(), &cursor, 10);
            clip.length = std::strtoll(cursor, &cursor, 10);
            while (*cursor == ' ') ++cursor;
            clip.name = fromUtf8(cursor);
            if (clip.length > 0) {
                track->midiClips.push_back(std::move(clip));
                midiClip = &track->midiClips.back();
            } else {
                midiClip = nullptr;
            }
        } else if (key == "note" && midiClip) {
            MidiNote note;
            char* cursor = nullptr;
            note.start = std::strtoll(rest.c_str(), &cursor, 10);
            note.length = std::strtoll(cursor, &cursor, 10);
            const long pitch = std::strtol(cursor, &cursor, 10);
            const long velocity = std::strtol(cursor, &cursor, 10);
            if (note.length > 0 && pitch >= 0 && pitch <= 127) {
                note.pitch = static_cast<std::uint8_t>(pitch);
                note.velocity = static_cast<std::uint8_t>(velocity < 1 ? 1 : velocity > 127 ? 127 : velocity);
                midiClip->notes.push_back(note);
            }
        }
        // Unbekannte Schluesselwoerter werden uebersprungen (spaetere Erweiterungen).
    }
    return true;
}
