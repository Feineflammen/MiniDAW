#include "model/MidiFile.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <deque>
#include <fstream>
#include <map>
#include <utility>

namespace {

// --- Hilfen zum Lesen --------------------------------------------------------

struct Reader {
    const std::uint8_t* p;
    const std::uint8_t* end;

    bool has(std::size_t n) const { return static_cast<std::size_t>(end - p) >= n; }
    bool u8(std::uint8_t& v) {
        if (!has(1)) return false;
        v = *p++;
        return true;
    }
    bool u16(std::uint32_t& v) {
        if (!has(2)) return false;
        v = (std::uint32_t{p[0]} << 8) | p[1];
        p += 2;
        return true;
    }
    bool u32(std::uint32_t& v) {
        if (!has(4)) return false;
        v = (std::uint32_t{p[0]} << 24) | (std::uint32_t{p[1]} << 16) | (std::uint32_t{p[2]} << 8) | p[3];
        p += 4;
        return true;
    }
    // Zahl variabler Laenge, hoechstens vier Bytes.
    bool var(std::uint32_t& v) {
        v = 0;
        for (int i = 0; i < 4; ++i) {
            std::uint8_t b;
            if (!u8(b)) return false;
            v = (v << 7) | (b & 0x7Fu);
            if (!(b & 0x80u)) return true;
        }
        return false;
    }
    bool skip(std::size_t n) {
        if (!has(n)) return false;
        p += n;
        return true;
    }
};

// Text aus der Datei: UTF-8, wenn es gueltiges UTF-8 ist, sonst Latin-1
// (so schreiben es die meisten aelteren Programme).
std::wstring decodeText(const std::uint8_t* data, std::size_t size) {
    while (size > 0 && (data[size - 1] == 0 || data[size - 1] == ' ')) --size;
    std::wstring out;
    bool utf8 = true;
    for (std::size_t i = 0; i < size && utf8;) {
        const std::uint8_t c = data[i];
        int extra = c < 0x80 ? 0 : (c & 0xE0) == 0xC0 ? 1 : (c & 0xF0) == 0xE0 ? 2 : (c & 0xF8) == 0xF0 ? 3 : -1;
        if (extra < 0 || (extra > 0 && i + static_cast<std::size_t>(extra) >= size)) {
            utf8 = false;
            break;
        }
        std::uint32_t cp = extra == 0 ? c : extra == 1 ? (c & 0x1Fu) : extra == 2 ? (c & 0x0Fu) : (c & 0x07u);
        for (int k = 1; k <= extra; ++k) {
            const std::uint8_t cc = data[i + static_cast<std::size_t>(k)];
            if ((cc & 0xC0) != 0x80) {
                utf8 = false;
                break;
            }
            cp = (cp << 6) | (cc & 0x3Fu);
        }
        if (!utf8) break;
        if (cp > 0xFFFF) {  // ausserhalb der Grundebene: als Ersatzpaar (Windows) oder direkt
            if constexpr (sizeof(wchar_t) == 2) {
                cp -= 0x10000;
                out += static_cast<wchar_t>(0xD800 + (cp >> 10));
                out += static_cast<wchar_t>(0xDC00 + (cp & 0x3FF));
            } else {
                out += static_cast<wchar_t>(cp);
            }
        } else {
            out += static_cast<wchar_t>(cp);
        }
        i += static_cast<std::size_t>(extra) + 1;
    }
    if (utf8) return out;
    out.clear();
    for (std::size_t i = 0; i < size; ++i) out += static_cast<wchar_t>(data[i]);
    return out;
}

struct RawNote {
    std::int64_t start = 0;
    std::int64_t end = 0;
    std::uint8_t pitch = 0;
    std::uint8_t velocity = 0;
    int channel = 0;
};

struct RawControl {
    std::int64_t tick = 0;
    int channel = 0;
    int controller = 0;
    int value = 0;
};

struct RawTrack {
    std::wstring name;
    std::vector<RawNote> notes;
    std::vector<RawControl> controls;
};

struct TempoChange {
    std::int64_t tick = 0;
    std::uint32_t microsPerQuarter = 500000;
};

bool parseTrack(Reader r, std::int64_t minLength, RawTrack& track, std::vector<TempoChange>& tempos,
                std::string& error) {
    std::int64_t tick = 0;
    std::uint8_t running = 0;
    bool haveName = false;
    // Offene Noten je Kanal und Tonhoehe; Note-Aus beendet die aelteste.
    std::map<int, std::deque<std::pair<std::int64_t, std::uint8_t>>> open;

    const auto finish = [&](int channel, std::uint8_t pitch, std::int64_t at) {
        auto it = open.find(channel * 128 + pitch);
        if (it == open.end() || it->second.empty()) return;  // Note-Aus ohne Note-An: ignorieren
        const auto [start, velocity] = it->second.front();
        it->second.pop_front();
        RawNote note;
        note.start = start;
        note.end = std::max(at, start + minLength);
        note.pitch = pitch;
        note.velocity = velocity;
        note.channel = channel;
        track.notes.push_back(note);
    };

    while (r.p < r.end) {
        std::uint32_t delta;
        if (!r.var(delta)) break;  // abgeschnittene Spur: das Gelesene behalten
        tick += delta;

        std::uint8_t status;
        if (!r.u8(status)) break;
        if (status < 0x80) {
            if (running == 0) {
                error = "Die Datei ist besch\xC3\xA4" "digt (Daten ohne Status-Byte).";
                return false;
            }
            --r.p;  // Running Status: das Byte gehoert schon zu den Daten
            status = running;
        }

        if (status == 0xFF) {
            std::uint8_t type;
            std::uint32_t length;
            if (!r.u8(type) || !r.var(length) || !r.has(length)) break;
            if (type == 0x2F) break;  // Spurende
            if (type == 0x51 && length >= 3) {
                const std::uint32_t us = (std::uint32_t{r.p[0]} << 16) | (std::uint32_t{r.p[1]} << 8) | r.p[2];
                if (us > 0) tempos.push_back({tick, us});
            } else if (type == 0x03 && !haveName) {
                track.name = decodeText(r.p, length);
                haveName = true;
            }
            r.skip(length);
            continue;
        }
        if (status == 0xF0 || status == 0xF7) {  // SysEx: ueberspringen
            std::uint32_t length;
            if (!r.var(length) || !r.skip(length)) break;
            continue;
        }
        if (status >= 0xF0) {
            error = "Die Datei ist besch\xC3\xA4" "digt (unbekanntes Ereignis).";
            return false;
        }

        running = status;
        const int kind = status & 0xF0;
        const int channel = status & 0x0F;
        std::uint8_t a = 0, b = 0;
        if (!r.u8(a)) break;
        if (kind != 0xC0 && kind != 0xD0 && !r.u8(b)) break;
        a &= 0x7F;
        b &= 0x7F;

        if (kind == 0x90 && b > 0) {
            open[channel * 128 + a].push_back({tick, b});
        } else if (kind == 0x80 || kind == 0x90) {
            finish(channel, a, tick);
        } else if (kind == 0xB0 && a < 120) {  // Regler; 120..127 sind Kanalbefehle
            track.controls.push_back({tick, channel, a, b});
        } else if (kind == 0xD0) {
            track.controls.push_back({tick, channel, 128, a});
        } else if (kind == 0xE0) {
            track.controls.push_back({tick, channel, 129, (b << 7) | a});
        }
    }

    // Noten ohne Ende klingen bis zum Ende der Spur.
    for (auto& [key, starts] : open) {
        while (!starts.empty()) finish(key / 128, static_cast<std::uint8_t>(key % 128), tick);
    }
    return true;
}

// --- Hilfen zum Schreiben ----------------------------------------------------

void put32(std::vector<std::uint8_t>& out, std::uint32_t v) {
    out.push_back(static_cast<std::uint8_t>(v >> 24));
    out.push_back(static_cast<std::uint8_t>(v >> 16));
    out.push_back(static_cast<std::uint8_t>(v >> 8));
    out.push_back(static_cast<std::uint8_t>(v));
}

void putVar(std::vector<std::uint8_t>& out, std::uint64_t v) {
    v = std::min<std::uint64_t>(v, 0x0FFFFFFF);  // groesster darstellbarer Wert
    std::uint8_t bytes[4];
    int n = 0;
    do {
        bytes[n++] = static_cast<std::uint8_t>(v & 0x7F);
        v >>= 7;
    } while (v > 0);
    while (n > 1) out.push_back(static_cast<std::uint8_t>(bytes[--n] | 0x80));
    out.push_back(bytes[0]);
}

// Namen als Latin-1, wenn das reicht (versteht jedes Programm), sonst UTF-8.
std::vector<std::uint8_t> encodeText(const std::wstring& text) {
    std::vector<std::uint8_t> out;
    const bool latin1 = std::all_of(text.begin(), text.end(), [](wchar_t c) { return static_cast<std::uint32_t>(c) <= 0xFF; });
    if (latin1) {
        for (wchar_t c : text) out.push_back(static_cast<std::uint8_t>(c));
        return out;
    }
    for (std::size_t i = 0; i < text.size(); ++i) {
        std::uint32_t cp = static_cast<std::uint32_t>(text[i]);
        if constexpr (sizeof(wchar_t) == 2) {
            if (cp >= 0xD800 && cp < 0xDC00 && i + 1 < text.size()) {
                cp = 0x10000 + ((cp - 0xD800) << 10) + (static_cast<std::uint32_t>(text[i + 1]) - 0xDC00);
                ++i;
            }
        }
        if (cp < 0x80) {
            out.push_back(static_cast<std::uint8_t>(cp));
        } else if (cp < 0x800) {
            out.push_back(static_cast<std::uint8_t>(0xC0 | (cp >> 6)));
            out.push_back(static_cast<std::uint8_t>(0x80 | (cp & 0x3F)));
        } else if (cp < 0x10000) {
            out.push_back(static_cast<std::uint8_t>(0xE0 | (cp >> 12)));
            out.push_back(static_cast<std::uint8_t>(0x80 | ((cp >> 6) & 0x3F)));
            out.push_back(static_cast<std::uint8_t>(0x80 | (cp & 0x3F)));
        } else {
            out.push_back(static_cast<std::uint8_t>(0xF0 | (cp >> 18)));
            out.push_back(static_cast<std::uint8_t>(0x80 | ((cp >> 12) & 0x3F)));
            out.push_back(static_cast<std::uint8_t>(0x80 | ((cp >> 6) & 0x3F)));
            out.push_back(static_cast<std::uint8_t>(0x80 | (cp & 0x3F)));
        }
    }
    return out;
}

void putChunk(std::vector<std::uint8_t>& out, const std::vector<std::uint8_t>& body) {
    out.insert(out.end(), {'M', 'T', 'r', 'k'});
    put32(out, static_cast<std::uint32_t>(body.size()));
    out.insert(out.end(), body.begin(), body.end());
}

}  // namespace

bool parseMidiFile(const std::vector<std::uint8_t>& bytes, MidiFileData& out, std::string& error) {
    out = MidiFileData{};
    Reader r{bytes.data(), bytes.data() + bytes.size()};

    // Manche Dateien (RIFF-MIDI, .rmi) verpacken die eigentliche Datei.
    if (bytes.size() >= 20 && std::memcmp(bytes.data(), "RIFF", 4) == 0 && std::memcmp(bytes.data() + 8, "RMID", 4) == 0) {
        r.skip(12);
        while (r.has(8)) {
            const bool data = std::memcmp(r.p, "data", 4) == 0;
            r.skip(4);
            std::uint32_t size = static_cast<std::uint32_t>(r.p[0]) | (static_cast<std::uint32_t>(r.p[1]) << 8) |
                                 (static_cast<std::uint32_t>(r.p[2]) << 16) | (static_cast<std::uint32_t>(r.p[3]) << 24);
            r.skip(4);
            if (data) {
                r.end = r.p + std::min<std::size_t>(size, static_cast<std::size_t>(r.end - r.p));
                break;
            }
            if (!r.skip(size + (size & 1))) break;
        }
    }

    std::uint32_t headerLength = 0, format = 0, trackCount = 0, division = 0;
    if (!r.has(14) || std::memcmp(r.p, "MThd", 4) != 0) {
        error = "Das ist keine MIDI-Datei.";
        return false;
    }
    r.skip(4);
    r.u32(headerLength);
    if (headerLength < 6 || !r.u16(format) || !r.u16(trackCount) || !r.u16(division) || !r.skip(headerLength - 6)) {
        error = "Der Kopf der MIDI-Datei ist besch\xC3\xA4" "digt.";
        return false;
    }
    if (format > 2) {
        error = "Unbekannter MIDI-Dateityp " + std::to_string(format) + ".";
        return false;
    }
    if (division == 0) {
        error = "Die MIDI-Datei hat keine Zeiteinteilung.";
        return false;
    }
    out.format = static_cast<int>(format);

    // Zeitbasis: Ticks pro Viertel oder (selten) SMPTE-Bilder pro Sekunde.
    const bool smpte = (division & 0x8000) != 0;
    double ticksPerSecond = 0.0;
    std::int64_t ticksPerQuarter = 0;
    if (smpte) {
        const int fps = 256 - static_cast<int>((division >> 8) & 0xFF);
        const int perFrame = static_cast<int>(division & 0xFF);
        ticksPerSecond = (fps == 29 ? 29.97 : fps) * std::max(1, perFrame);
    } else {
        ticksPerQuarter = division;
    }
    // Kuerzeste Note (Note-An und -Aus auf demselben Tick): ein 64tel bzw. 1/64 s.
    const std::int64_t minLength =
        std::max<std::int64_t>(1, smpte ? static_cast<std::int64_t>(ticksPerSecond / 64.0) : ticksPerQuarter / 16);

    std::vector<RawTrack> tracks;
    std::vector<TempoChange> tempos;
    while (r.has(8)) {
        const bool isTrack = std::memcmp(r.p, "MTrk", 4) == 0;
        r.skip(4);
        std::uint32_t length;
        r.u32(length);
        // Manche Programme schreiben falsche Laengen: hoechstens bis zum Dateiende lesen.
        const std::size_t available = std::min<std::size_t>(length, static_cast<std::size_t>(r.end - r.p));
        if (isTrack) {
            RawTrack track;
            if (!parseTrack(Reader{r.p, r.p + available}, minLength, track, tempos, error)) return false;
            tracks.push_back(std::move(track));
        }
        r.skip(available);
    }
    if (tracks.empty()) {
        error = "Die MIDI-Datei enth\xC3\xA4lt keine Spuren.";
        return false;
    }

    // Tempo-Karte: Ticks in Sekunden umrechnen.
    std::stable_sort(tempos.begin(), tempos.end(), [](const TempoChange& a, const TempoChange& b) { return a.tick < b.tick; });
    if (!smpte && !tempos.empty()) out.firstBpm = 60'000'000.0 / tempos.front().microsPerQuarter;
    struct Segment {
        std::int64_t tick;
        double seconds;
        double secondsPerTick;
    };
    std::vector<Segment> segments;
    if (smpte) {
        segments.push_back({0, 0.0, 1.0 / ticksPerSecond});
    } else {
        const auto perTick = [&](std::uint32_t us) { return us / 1e6 / static_cast<double>(ticksPerQuarter); };
        segments.push_back({0, 0.0, perTick(500000)});
        for (const TempoChange& t : tempos) {
            const Segment& last = segments.back();
            const double seconds = last.seconds + static_cast<double>(t.tick - last.tick) * last.secondsPerTick;
            if (t.tick == last.tick) segments.back().secondsPerTick = perTick(t.microsPerQuarter);
            else segments.push_back({t.tick, seconds, perTick(t.microsPerQuarter)});
        }
    }
    const auto toSeconds = [&](std::int64_t tick) {
        auto it = std::upper_bound(segments.begin(), segments.end(), tick,
                                   [](std::int64_t value, const Segment& s) { return value < s.tick; });
        const Segment& s = *(it - 1);
        return s.seconds + static_cast<double>(tick - s.tick) * s.secondsPerTick;
    };

    // Jede Spur nach Kanaelen aufteilen, in der Reihenfolge des ersten Auftretens.
    for (RawTrack& track : tracks) {
        std::sort(track.notes.begin(), track.notes.end(), [](const RawNote& a, const RawNote& b) {
            return a.start != b.start ? a.start < b.start : a.pitch < b.pitch;
        });
        std::vector<int> channels;
        for (const RawNote& n : track.notes)
            if (std::find(channels.begin(), channels.end(), n.channel) == channels.end()) channels.push_back(n.channel);
        // Kanaele nur mit Controllern (z. B. ein Pedal-Spur) kommen hinten dazu.
        for (const RawControl& c : track.controls)
            if (std::find(channels.begin(), channels.end(), c.channel) == channels.end()) channels.push_back(c.channel);
        for (int channel : channels) {
            MidiFilePart part;
            part.name = track.name;
            part.channel = channel;
            part.splitFromTrack = channels.size() > 1;
            for (const RawNote& n : track.notes) {
                if (n.channel != channel) continue;
                MidiFileNote note;
                note.start = toSeconds(n.start);
                note.length = toSeconds(n.end) - note.start;
                note.pitch = n.pitch;
                note.velocity = n.velocity;
                out.end = std::max(out.end, note.start + note.length);
                part.notes.push_back(note);
            }
            for (const RawControl& c : track.controls) {  // in Dateireihenfolge, also schon nach Zeit
                if (c.channel != channel) continue;
                MidiFileControl control;
                control.time = toSeconds(c.tick);
                control.controller = c.controller;
                control.value = c.value;
                out.end = std::max(out.end, control.time);
                part.controls.push_back(control);
            }
            out.parts.push_back(std::move(part));
        }
    }
    const bool anyNotes = std::any_of(out.parts.begin(), out.parts.end(), [](const MidiFilePart& p) { return !p.notes.empty(); });
    if (!anyNotes) {
        error = "Die MIDI-Datei enth\xC3\xA4lt keine Noten.";
        return false;
    }
    return true;
}

bool loadMidiFile(const std::filesystem::path& path, MidiFileData& out, std::string& error) {
    std::ifstream in(path, std::ios::binary);
    if (!in) {
        error = "Die Datei l\xC3\xA4sst sich nicht \xC3\xB6" "ffnen.";
        return false;
    }
    in.seekg(0, std::ios::end);
    const auto size = static_cast<std::streamoff>(in.tellg());
    if (size <= 0 || size > 64 * 1024 * 1024) {
        error = size <= 0 ? "Die Datei ist leer." : "Die Datei ist zu gro\xC3\x9F f\xC3\xBCr eine MIDI-Datei.";
        return false;
    }
    in.seekg(0);
    std::vector<std::uint8_t> bytes(static_cast<std::size_t>(size));
    if (!in.read(reinterpret_cast<char*>(bytes.data()), size)) {
        error = "Die Datei l\xC3\xA4sst sich nicht lesen.";
        return false;
    }
    return parseMidiFile(bytes, out, error);
}

std::vector<std::uint8_t> buildMidiFile(const std::vector<MidiExportTrack>& tracks, double bpm, int ppq) {
    std::vector<std::uint8_t> out{'M', 'T', 'h', 'd', 0, 0, 0, 6, 0, 1};
    const auto count = static_cast<std::uint32_t>(tracks.size() + 1);
    out.push_back(static_cast<std::uint8_t>(count >> 8));
    out.push_back(static_cast<std::uint8_t>(count));
    out.push_back(static_cast<std::uint8_t>(ppq >> 8));
    out.push_back(static_cast<std::uint8_t>(ppq));

    // Tempospur: 4/4 und das Tempo des Projekts.
    {
        const auto us = static_cast<std::uint32_t>(std::llround(60'000'000.0 / std::clamp(bpm, 1.0, 1000.0)));
        const std::vector<std::uint8_t> body{0x00, 0xFF, 0x58, 0x04, 0x04, 0x02, 0x18, 0x08,
                                             0x00, 0xFF, 0x51, 0x03, static_cast<std::uint8_t>(us >> 16),
                                             static_cast<std::uint8_t>(us >> 8), static_cast<std::uint8_t>(us),
                                             0x00, 0xFF, 0x2F, 0x00};
        putChunk(out, body);
    }

    for (const MidiExportTrack& track : tracks) {
        std::vector<std::uint8_t> body;
        const int channel = std::clamp(track.channel, 0, 15);
        if (!track.name.empty()) {
            const auto name = encodeText(track.name);
            body.insert(body.end(), {0x00, 0xFF, 0x03});
            putVar(body, name.size());
            body.insert(body.end(), name.begin(), name.end());
        }
        // Ereignisse: Rang 0 = Note aus, 1 = Controller, 2 = Note an (bei gleichem Tick in dieser Reihenfolge)
        struct Event {
            std::int64_t tick;
            int rank;
            std::uint8_t status, data1, data2;
        };
        std::vector<Event> events;
        events.reserve(track.notes.size() * 2 + track.controls.size());
        const auto ch = static_cast<std::uint8_t>(channel);
        for (const MidiExportNote& n : track.notes) {
            const std::int64_t start = std::max<std::int64_t>(0, n.tick);
            const auto pitch = static_cast<std::uint8_t>(n.pitch & 0x7F);
            events.push_back({start, 2, static_cast<std::uint8_t>(0x90 | ch), pitch,
                              static_cast<std::uint8_t>(std::clamp<int>(n.velocity, 1, 127))});
            events.push_back({start + std::max<std::int64_t>(1, n.length), 0, static_cast<std::uint8_t>(0x80 | ch), pitch, 64});
        }
        for (const MidiExportControl& c : track.controls) {
            const std::int64_t at = std::max<std::int64_t>(0, c.tick);
            if (c.controller == 129) {
                const int v = std::clamp(c.value, 0, 16383);
                events.push_back({at, 1, static_cast<std::uint8_t>(0xE0 | ch), static_cast<std::uint8_t>(v & 0x7F),
                                  static_cast<std::uint8_t>(v >> 7)});
            } else if (c.controller == 128) {
                events.push_back({at, 1, static_cast<std::uint8_t>(0xD0 | ch), static_cast<std::uint8_t>(std::clamp(c.value, 0, 127)), 0});
            } else if (c.controller >= 0 && c.controller < 120) {
                events.push_back({at, 1, static_cast<std::uint8_t>(0xB0 | ch), static_cast<std::uint8_t>(c.controller),
                                  static_cast<std::uint8_t>(std::clamp(c.value, 0, 127))});
            }
        }
        std::stable_sort(events.begin(), events.end(), [](const Event& a, const Event& b) {
            return a.tick != b.tick ? a.tick < b.tick : a.rank < b.rank;
        });
        std::int64_t last = 0;
        for (const Event& e : events) {
            putVar(body, static_cast<std::uint64_t>(e.tick - last));
            last = e.tick;
            body.push_back(e.status);
            body.push_back(e.data1);
            if ((e.status & 0xF0) != 0xD0) body.push_back(e.data2);  // Kanaldruck hat nur ein Datenbyte
        }
        body.insert(body.end(), {0x00, 0xFF, 0x2F, 0x00});
        putChunk(out, body);
    }
    return out;
}

bool saveMidiFile(const std::filesystem::path& path, const std::vector<std::uint8_t>& bytes, std::string& error) {
    std::ofstream file(path, std::ios::binary | std::ios::trunc);
    if (!file || !file.write(reinterpret_cast<const char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()))) {
        error = "Die Datei l\xC3\xA4sst sich nicht schreiben.";
        return false;
    }
    file.close();
    if (!file) {
        error = "Die Datei l\xC3\xA4sst sich nicht vollst\xC3\xA4ndig schreiben.";
        return false;
    }
    return true;
}
