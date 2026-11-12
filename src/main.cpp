// Schritt 2: Engine mit Transport und Mixer, bedient ueber die Konsole.
// Vier Spuren spielen Testtoene (A-Dur-Akkord), verteilt im Stereobild.

#include "audio/WasapiDevice.h"
#include "engine/Engine.h"

#include <objbase.h>

#include <array>
#include <cmath>
#include <cstdio>
#include <iostream>
#include <memory>
#include <sstream>
#include <string>

namespace {

void printHelp() {
    std::cout << "Befehle (Spuren zaehlen von 1 bis " << Engine::kNumTracks << "):\n"
                 "  p            Play / Stop\n"
                 "  z            zurueck zum Anfang\n"
                 "  m <spur>     Mute umschalten\n"
                 "  s <spur>     Solo umschalten\n"
                 "  g <spur> <w> Lautstaerke der Spur, 1.0 = 0 dB\n"
                 "  a <spur> <w> Panorama, -1 = links, 1 = rechts\n"
                 "  v <w>        Master-Lautstaerke\n"
                 "  t <spur> <hz> Testton setzen, 0 = aus\n"
                 "  i            Position und Pegel anzeigen\n"
                 "  h            diese Hilfe\n"
                 "  q            beenden\n";
}

double toDb(float linear) {
    return linear > 0.00001f ? 20.0 * std::log10(static_cast<double>(linear)) : -100.0;
}

void printInfo(Engine& engine, double sampleRate) {
    std::printf("%s  Position %.2f s  Master %.1f dB\n",
                engine.isPlaying() ? "Play" : "Stop",
                static_cast<double>(engine.position()) / sampleRate,
                toDb(engine.takeMasterPeak()));
    for (int t = 0; t < Engine::kNumTracks; ++t) {
        const float peak = engine.takeTrackPeak(t);
        if (peak > 0.0f) std::printf("  Spur %2d  %.1f dB\n", t + 1, toDb(peak));
    }
}

}  // namespace

int main() {
    if (FAILED(CoInitializeEx(nullptr, COINIT_MULTITHREADED))) {
        std::cerr << "COM nicht initialisierbar\n";
        return 1;
    }

    int result = 0;
    {
        WasapiDevice device;
        auto engine = std::make_unique<Engine>();
        std::string error;

        // Ausgangszustand: vier Toene, im Stereobild verteilt.
        const float tones[4] = {220.00f, 277.18f, 329.63f, 440.00f};
        const float pans[4] = {-0.8f, -0.3f, 0.3f, 0.8f};
        for (int t = 0; t < 4; ++t) {
            engine->setTrackTestTone(t, tones[t]);
            engine->setTrackPan(t, pans[t]);
        }

        if (!device.open(error) || !device.start(*engine, error)) {
            std::cerr << "Fehler: " << error << "\n";
            result = 1;
        } else {
            std::cout << "Geraet: " << device.sampleRate() << " Hz, " << device.numChannels()
                      << " Kanaele, Periode " << device.periodFrames() << " Frames\n\n";
            printHelp();

            std::array<bool, Engine::kNumTracks> mute{};
            std::array<bool, Engine::kNumTracks> solo{};

            std::string line;
            while (std::cout << "> " && std::getline(std::cin, line)) {
                std::istringstream in(line);
                std::string cmd;
                if (!(in >> cmd)) continue;

                int track = 0;
                float value = 0.0f;
                bool ok = true;

                if (cmd == "q") {
                    break;
                } else if (cmd == "h") {
                    printHelp();
                } else if (cmd == "p") {
                    ok = engine->isPlaying() ? engine->stop() : engine->play();
                } else if (cmd == "z") {
                    ok = engine->locate(0);
                } else if (cmd == "i") {
                    printInfo(*engine, device.sampleRate());
                } else if (cmd == "v" && (in >> value)) {
                    ok = engine->setMasterGain(value);
                } else if (cmd == "m" && (in >> track) && track >= 1 && track <= Engine::kNumTracks) {
                    auto& state = mute[static_cast<std::size_t>(track - 1)];
                    state = !state;
                    ok = engine->setTrackMute(track - 1, state);
                    std::cout << "Spur " << track << (state ? " stumm\n" : " hoerbar\n");
                } else if (cmd == "s" && (in >> track) && track >= 1 && track <= Engine::kNumTracks) {
                    auto& state = solo[static_cast<std::size_t>(track - 1)];
                    state = !state;
                    ok = engine->setTrackSolo(track - 1, state);
                    std::cout << "Spur " << track << (state ? " solo an\n" : " solo aus\n");
                } else if (cmd == "g" && (in >> track >> value)) {
                    ok = engine->setTrackGain(track - 1, value);
                } else if (cmd == "a" && (in >> track >> value)) {
                    ok = engine->setTrackPan(track - 1, value);
                } else if (cmd == "t" && (in >> track >> value)) {
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
        }
    }

    CoUninitialize();
    return result;
}
