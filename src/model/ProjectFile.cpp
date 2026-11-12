#include "model/ProjectFile.h"

#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <sstream>

namespace {

const char* const kMagic = "MiniDAW-Projekt";
constexpr int kVersion = 1;

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
        out << "  midiausgang " << (track.midiOutput == kTrackOutputSynth ? std::string("synth") : std::to_string(track.midiOutput + 1)) << '\n';
        if (!track.plugin.empty()) {
            out << "  plugin " << pathToUtf8(track.plugin) << '\n';
            out << "  pluginname " << toUtf8(track.pluginName) << '\n';
            if (!track.pluginState.empty()) out << "  pluginzustand " << toHex(track.pluginState) << '\n';
        }
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
    MidiClipModel* midiClip = nullptr;
    while (std::getline(file, line)) {
        if (!line.empty() && line.back() == '\r') line.pop_back();
        splitLine(line, key, rest);
        if (key.empty()) continue;

        if (key == "samplerate") project.sampleRate = std::strtod(rest.c_str(), nullptr);
        else if (key == "tempo") project.bpm = std::strtod(rest.c_str(), nullptr);
        else if (key == "master") project.masterGain = static_cast<float>(std::strtod(rest.c_str(), nullptr));
        else if (key == "spur") {
            const int index = std::atoi(rest.c_str());
            track = (index >= 1 && index <= kProjectTracks) ? &project.tracks[static_cast<std::size_t>(index - 1)] : nullptr;
            midiClip = nullptr;
        } else if (!track) {
            continue;  // Unbekanntes oder Zeilen einer ungueltigen Spur ueberspringen
        } else if (key == "name") track->name = fromUtf8(rest);
        else if (key == "lautstaerke") track->gain = static_cast<float>(std::strtod(rest.c_str(), nullptr));
        else if (key == "panorama") track->pan = static_cast<float>(std::strtod(rest.c_str(), nullptr));
        else if (key == "stumm") track->mute = rest == "1";
        else if (key == "solo") track->solo = rest == "1";
        else if (key == "eingang") track->input = inputFromName(rest);
        else if (key == "midiausgang") {
            const int channel = std::atoi(rest.c_str());
            track->midiOutput = (rest != "synth" && channel >= 1 && channel <= 16) ? channel - 1 : kTrackOutputSynth;
        } else if (key == "plugin") track->plugin = pathFromUtf8(rest);
        else if (key == "pluginname") track->pluginName = fromUtf8(rest);
        else if (key == "pluginzustand") {
            if (!fromHex(rest, track->pluginState)) track->pluginState.clear();
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
