// Schritt 3: WAV-Dateien als Clips abspielen, bedient ueber die Konsole.

#include "audio/WasapiDevice.h"
#include "disk/DiskThread.h"
#include "disk/WavFile.h"
#include "engine/Engine.h"

#include <fcntl.h>
#include <io.h>
#include <objbase.h>

#include <array>
#include <cmath>
#include <cstdio>
#include <filesystem>
#include <iostream>
#include <memory>
#include <sstream>
#include <string>

namespace {

void printHelp() {
    std::cout << "Befehle (Spuren zaehlen von 1 bis " << Engine::kNumTracks << "):\n"
                 "  l <spur> <datei>  WAV-Datei auf die Spur legen (Datei ins Fenster ziehen geht)\n"
                 "  l <spur>          Spur leeren\n"
                 "  p                 Play / Stop\n"
                 "  z                 zurueck zum Anfang\n"
                 "  j <sekunden>      zu einer Position springen\n"
                 "  m <spur>          Mute umschalten\n"
                 "  s <spur>          Solo umschalten\n"
                 "  g <spur> <wert>   Lautstaerke der Spur, 1.0 = 0 dB\n"
                 "  a <spur> <wert>   Panorama, -1 = links, 1 = rechts\n"
                 "  v <wert>          Master-Lautstaerke\n"
                 "  t <spur> <hz>     Testton setzen, 0 = aus\n"
                 "  i                 Position, Pegel und Aussetzer anzeigen\n"
                 "  h                 diese Hilfe\n"
                 "  q                 beenden\n";
}

double toDb(float linear) {
    return linear > 0.00001f ? 20.0 * std::log10(static_cast<double>(linear)) : -100.0;
}

void printInfo(Engine& engine, double sampleRate) {
    std::printf("%s  Position %.2f s  Master %.1f dB  Aussetzer %u\n",
                engine.isPlaying() ? "Play" : "Stop",
                static_cast<double>(engine.position()) / sampleRate,
                toDb(engine.takeMasterPeak()),
                engine.underruns());
    for (int t = 0; t < Engine::kNumTracks; ++t) {
        const float peak = engine.takeTrackPeak(t);
        if (peak > 0.0f) std::printf("  Spur %2d  %.1f dB\n", t + 1, toDb(peak));
    }
}

// Entfernt Leerraum und umschliessende Anfuehrungszeichen.
std::wstring cleanPath(std::wstring text) {
    const auto isSpace = [](wchar_t c) { return c == L' ' || c == L'\t' || c == L'\r' || c == L'\n'; };
    while (!text.empty() && isSpace(text.back())) text.pop_back();
    std::size_t first = 0;
    while (first < text.size() && isSpace(text[first])) ++first;
    text.erase(0, first);
    if (text.size() >= 2 && text.front() == L'"' && text.back() == L'"')
        text = text.substr(1, text.size() - 2);
    return text;
}

bool loadFile(Engine& engine, int track, const std::wstring& pathText, double deviceRate) {
    if (pathText.empty()) {
        std::cout << "Spur " << track << " geleert.\n";
        return engine.setTrackPlaylist(track - 1, {});
    }

    std::string error;
    auto wav = WavFile::open(std::filesystem::path(pathText), error);
    if (!wav) {
        std::cout << "Laden fehlgeschlagen: " << error << "\n";
        return true;
    }

    std::printf("Spur %d: %d Hz, %d Bit%s, %d Kanal/Kanaele, %.2f s\n", track, wav->sampleRate(),
                wav->bitsPerSample(), wav->isFloat() ? " Float" : "", wav->channels(),
                static_cast<double>(wav->frames()) / wav->sampleRate());
    if (std::fabs(wav->sampleRate() - deviceRate) > 0.5) {
        std::printf("Achtung: Das Geraet laeuft mit %.0f Hz. Die Datei klingt deshalb zu %s.\n"
                    "Eine Umrechnung der Samplerate gibt es noch nicht.\n",
                    deviceRate, wav->sampleRate() < deviceRate ? "hoch und schnell" : "tief und langsam");
    }

    const std::int64_t frames = wav->frames();
    return engine.setTrackPlaylist(track - 1, {Clip{std::move(wav), 0, 0, frames, 1.0f}});
}

}  // namespace

int main() {
    if (FAILED(CoInitializeEx(nullptr, COINIT_MULTITHREADED))) {
        std::cerr << "COM nicht initialisierbar\n";
        return 1;
    }

    // Eingabe als UTF-16 lesen, damit Dateipfade mit Umlauten funktionieren.
    _setmode(_fileno(stdin), _O_U16TEXT);

    int result = 0;
    {
        WasapiDevice device;
        DiskThread disk;
        auto engine = std::make_unique<Engine>();
        std::string error;

        if (!device.open(error) || !device.start(*engine, error)) {
            std::cerr << "Fehler: " << error << "\n";
            result = 1;
        } else {
            disk.start(*engine);
            const double rate = device.sampleRate();
            std::cout << "Geraet: " << rate << " Hz, " << device.numChannels()
                      << " Kanaele, Periode " << device.periodFrames() << " Frames\n\n";
            printHelp();

            std::array<bool, Engine::kNumTracks> mute{};
            std::array<bool, Engine::kNumTracks> solo{};

            std::wstring line;
            while (std::cout << "> " << std::flush && std::getline(std::wcin, line)) {
                std::wistringstream in(line);
                std::wstring cmd;
                if (!(in >> cmd)) continue;

                int track = 0;
                float value = 0.0f;
                bool ok = true;
                const auto trackValid = [&] { return track >= 1 && track <= Engine::kNumTracks; };

                if (cmd == L"q") {
                    break;
                } else if (cmd == L"h") {
                    printHelp();
                } else if (cmd == L"p") {
                    ok = engine->isPlaying() ? engine->stop() : engine->play();
                } else if (cmd == L"z") {
                    ok = engine->locate(0);
                } else if (cmd == L"j" && (in >> value)) {
                    ok = engine->locate(static_cast<std::int64_t>(static_cast<double>(value) * rate));
                } else if (cmd == L"i") {
                    printInfo(*engine, rate);
                } else if (cmd == L"l" && (in >> track)) {
                    std::wstring rest;
                    std::getline(in, rest);
                    ok = trackValid() && loadFile(*engine, track, cleanPath(rest), rate);
                } else if (cmd == L"v" && (in >> value)) {
                    ok = engine->setMasterGain(value);
                } else if (cmd == L"m" && (in >> track) && trackValid()) {
                    auto& state = mute[static_cast<std::size_t>(track - 1)];
                    state = !state;
                    ok = engine->setTrackMute(track - 1, state);
                    std::cout << "Spur " << track << (state ? " stumm\n" : " hoerbar\n");
                } else if (cmd == L"s" && (in >> track) && trackValid()) {
                    auto& state = solo[static_cast<std::size_t>(track - 1)];
                    state = !state;
                    ok = engine->setTrackSolo(track - 1, state);
                    std::cout << "Spur " << track << (state ? " solo an\n" : " solo aus\n");
                } else if (cmd == L"g" && (in >> track >> value)) {
                    ok = engine->setTrackGain(track - 1, value);
                } else if (cmd == L"a" && (in >> track >> value)) {
                    ok = engine->setTrackPan(track - 1, value);
                } else if (cmd == L"t" && (in >> track >> value)) {
                    ok = engine->setTrackTestTone(track - 1, value);
                } else {
                    std::cout << "Unbekannter Befehl, h zeigt die Hilfe.\n";
                }

                if (!ok) std::cout << "Befehl nicht angenommen (Spurnummer pruefen).\n";
                if (device.failed()) {
                    std::cerr << "Der Audio-Thread wurde durch einen Geraetefehler beendet.\n";
                    result = 1;
                    break;
                }
            }
            device.stop();
            disk.stop();
        }
    }

    CoUninitialize();
    return result;
}
