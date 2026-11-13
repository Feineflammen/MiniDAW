// MiniDAW: Programmstart.
//
// Aufruf ohne Argumente startet die DAW.
// "minidaw --screenshot bild.png [mixer|eingang]" zeichnet die Oberflaeche mit
// Beispieldaten in eine Datei, ohne Fenster und ohne Audiogeraet (fuer
// automatische Pruefungen).

#include "audio/AsioDevice.h"
#include "audio/WasapiDevice.h"
#include "disk/DiskThread.h"
#include "disk/WavFile.h"
#include "engine/Engine.h"
#include "engine/LatencyProbe.h"
#include "engine/OfflineRender.h"
#include "midi/MidiInput.h"
#include "midi/MidiOutput.h"
#include "model/ProjectController.h"
#include "ui/MainView.h"
#include "ui/MainWindow.h"
#include "vst3/PluginManager.h"

#include <windows.h>
#include <objbase.h>
#include <shellapi.h>
#include <shlobj.h>

#include <chrono>
#include <cmath>
#include <cstdlib>
#include <cwchar>
#include <filesystem>
#include <fstream>
#include <map>
#include <memory>
#include <string>
#include <vector>

namespace {

void showError(const std::string& text) {
    const std::wstring wide(text.begin(), text.end());
    MessageBoxW(nullptr, wide.c_str(), L"MiniDAW", MB_OK | MB_ICONERROR);
}

// Erzeugt eine kurze Beispieldatei: Tonfolge mit abklingenden Noten.
std::filesystem::path makeDemoFile(const wchar_t* name, double baseHz, double seconds, int rate) {
    const auto frames = static_cast<std::int64_t>(seconds * rate);
    std::vector<float> data(static_cast<std::size_t>(frames) * 2);
    const double noteLength = 0.5;
    for (std::int64_t i = 0; i < frames; ++i) {
        const double t = static_cast<double>(i) / rate;
        const int note = static_cast<int>(t / noteLength);
        const double inNote = t - note * noteLength;
        const double hz = baseHz * (1.0 + 0.25 * (note % 4));
        const double env = std::exp(-inNote * 5.0) * std::min(1.0, inNote * 200.0);
        const auto s = static_cast<float>(0.4 * env * std::sin(6.283185307179586 * hz * t));
        data[static_cast<std::size_t>(2 * i)] = s;
        data[static_cast<std::size_t>(2 * i + 1)] = s;
    }
    const auto path = std::filesystem::temp_directory_path() / name;
    std::string error;
    writeWav(path, data.data(), frames, 2, rate, WavFormat::Pcm16, error);
    return path;
}

// Plugin-Liste fuer den Screenshot-Modus: nur Namen, es wird nichts geladen.
struct DemoPluginHost final : ui::PluginHost {
    std::vector<std::wstring> pluginNames() override {
        return {L"Beispiel-Piano", L"Beispiel-Orgel", L"Beispiel-Hall", L"Beispiel-Kompressor"};
    }
    std::wstring pluginFolder() override { return L"C:\\Program Files\\Common Files\\VST3"; }
    bool load(int, int, int, std::wstring&) override { return false; }
    void unload(int, int) override {}
    bool openEditor(int, int, std::wstring&) override { return false; }
};

// Plugin fuer den Screenshot-Modus: laesst den Klang unveraendert, belegt nur einen Platz.
struct DemoPlugin final : AudioPlugin {
    explicit DemoPlugin(bool instrument, int latency = 0) : instrument_(instrument), latency_(latency) {}
    bool isInstrument() const noexcept override { return instrument_; }
    int latencyFrames() const noexcept override { return latency_; }
    void process(float*, float*, int, const PluginMidiEvent*, int, const PluginTransport&) noexcept override {}
    bool instrument_;
    int latency_;
};

// mode: leer = Spuren mit Plugin-Auswahl, "mixer" = Mischpult, "eingang" = Eingangsliste.
int runScreenshot(const std::filesystem::path& output, const std::wstring& mode) {
    constexpr int kRate = 44100;
    constexpr int kBlock = 512;

    auto engine = std::make_unique<Engine>();
    engine->prepare(kRate, 0, 2, kBlock);
    ProjectController controller(*engine);
    controller.setSampleRate(kRate);

    const auto drums = makeDemoFile(L"minidaw_demo_a.wav", 110.0, 8.0, kRate);
    const auto bass = makeDemoFile(L"minidaw_demo_b.wav", 55.0, 6.0, kRate);
    const auto lead = makeDemoFile(L"minidaw_demo_c.wav", 440.0, 4.0, kRate);

    std::string message;
    controller.addClip(0, drums, 0, message);
    controller.addClip(1, bass, kRate * 2, message);
    controller.addClip(2, lead, 0, message);
    controller.addClip(2, lead, kRate * 6, message);
    controller.addClip(4, drums, kRate * 9, message);
    controller.setTrackGain(0, 0.5f);
    controller.setTrackPan(1, -0.6f);
    controller.setTrackPan(2, 0.6f);
    controller.setTrackMute(4, true);
    // Soundkarte mit vier Eingaengen: zwei Spuren nehmen gleichzeitig auf.
    controller.setInputChannels(4);
    controller.setTrackAudioInput(0, 0, true);
    controller.setTrackAudioInput(1, 2, false);
    controller.setTrackAudioInput(2, 3, false);
    controller.setTrackAudioInput(7, 4, false);  // gibt es nicht: wird gelb
    controller.setTrackArmed(3, true);
    controller.setTrackArmed(1, true);

    // Beispiel fuer MIDI: ein Clip mit einer kleinen Tonfolge, verschiedene Kanaleinstellungen.
    MidiClipModel midiClip;
    midiClip.name = L"MIDI";
    midiClip.start = kRate * 1;
    midiClip.length = kRate * 6;
    const int pitches[] = {57, 60, 64, 67, 69, 67, 64, 60, 57, 62, 65, 69};
    for (int i = 0; i < 12; ++i)
        midiClip.notes.push_back({static_cast<std::int64_t>(i) * kRate / 2, kRate * 2 / 5,
                                  static_cast<std::uint8_t>(pitches[i]), 100});
    controller.addMidiClip(5, midiClip);
    controller.setTrackInput(5, kTrackInputMidiAll);
    controller.setTrackInput(6, 2);
    controller.setTrackMidiOutput(6, 9);
    controller.setTrackInput(3, kTrackInputMidiAll);

    // Mischpult: ein paar belegte Plaetze, Sends und ein Bus als Ausgang.
    controller.setPlugin(5, 0, std::make_shared<DemoPlugin>(true), L"Beispiel-Piano");
    controller.setPlugin(5, 1, std::make_shared<DemoPlugin>(false), L"Beispiel-EQ");
    controller.setPlugin(1, 1, std::make_shared<DemoPlugin>(false, 512), L"Beispiel-Kompressor");
    controller.setPlugin(kFirstBusStrip, 1, std::make_shared<DemoPlugin>(false), L"Beispiel-Hall");
    controller.setPlugin(kMasterStripIndex, 1, std::make_shared<DemoPlugin>(false, 64), L"Beispiel-Limiter");
    controller.setTrackSend(0, 0, 0.5f);
    controller.setTrackSend(2, 0, 0.3f);
    controller.setTrackSend(5, 0, 0.7f);
    controller.setTrackSend(5, 1, 0.2f);
    controller.setTrackOutput(4, 1);
    controller.setBusGain(1, 0.7f);
    controller.setBusPan(1, 0.3f);

    while (!controller.peaksIdle()) Sleep(10);

    // Ein Stueck abspielen, damit Abspielzeiger und Pegel etwas zeigen.
    std::vector<float> left(kBlock), right(kBlock);
    float* out[2] = {left.data(), right.data()};
    while (engine->serviceDisk()) {}
    engine->process(nullptr, 0, out, 2, kBlock);
    engine->play();
    for (int i = 0; i < 280; ++i) {
        while (engine->serviceDisk()) {}
        engine->process(nullptr, 0, out, 2, kBlock);
    }

    ui::MainView view(controller, *engine);
    view.setDeviceInfo(L"Screenshot-Modus, 44100 Hz");
    view.setMidiInfo(L"Beispiel-Keyboard");
    engine->postMidi(MidiMessage::noteOn(60, 100, 0));
    view.selectClip(1, 0);
    view.openPianoRoll(5, 0);
    DemoPluginHost demoPlugins;
    view.setPluginHost(&demoPlugins);
    if (mode == L"mixer") view.setMixerMode(true);
    else if (mode == L"eingang") view.openInputMenu(2);
    else view.openPluginPicker(1);
    view.tick();

    std::string error;
    const bool ok = ui::renderToPng(view, 1300, 840, output, error);

    std::error_code ec;
    for (const auto& file : {drums, bass, lead}) std::filesystem::remove(file, ec);
    return ok ? 0 : 2;
}

// Einstellungen, die zum Rechner gehoeren und nicht zum Projekt, liegen als
// einfache Textdatei unter %APPDATA%\MiniDAW.
std::filesystem::path settingsPath() {
    std::filesystem::path path;
    PWSTR appData = nullptr;
    if (SUCCEEDED(SHGetKnownFolderPath(FOLDERID_RoamingAppData, 0, nullptr, &appData)) && appData)
        path = std::filesystem::path(appData) / L"MiniDAW" / L"einstellungen.txt";
    if (appData) CoTaskMemFree(appData);
    return path;
}

struct Settings {
    int recordOffsetMs = 0;   // alter Wert ohne Geraetebezug, gilt fuer noch unbekannte Geraete
    std::wstring midiOutput;  // Name des MIDI-Ausgabegeraets, leer = keins
    std::wstring audioOutput; // Kennung des Wiedergabegeraets, leer = Windows-Standard
    std::wstring audioInput;  // Kennung des Aufnahmegeraets, leer = Windows-Standard
    bool noInput = false;
    bool asio = false;        // ASIO statt WASAPI
    std::wstring asioDriver;  // Name des ASIO-Treibers
    // Aufnahme-Latenz (Zusatz in ms) je Kombination aus Wiedergabe- und Aufnahmegeraet.
    std::map<std::wstring, int> latencyByDevice;
};

// Welches Geraet geoeffnet werden soll.
struct AudioChoice {
    bool asio = false;
    std::wstring asioDriver;
    std::wstring output, input;  // WASAPI-Kennungen, leer = Windows-Standard
    bool noInput = false;
};

AudioChoice choiceFrom(const Settings& settings) {
    return {settings.asio, settings.asioDriver, settings.audioOutput, settings.audioInput, settings.noInput};
}

void storeChoice(Settings& settings, const AudioChoice& choice) {
    settings.asio = choice.asio;
    if (choice.asio) {
        settings.asioDriver = choice.asioDriver;
    } else {
        settings.audioOutput = choice.output;
        settings.audioInput = choice.input;
        settings.noInput = choice.noInput;
    }
}

// Oeffnet das gewaehlte Geraet; nullptr bei Fehler (Grund in error).
std::unique_ptr<AudioDevice> openAudio(const AudioChoice& choice, HWND window, std::string& error) {
    if (choice.asio) {
        auto device = std::make_unique<AsioDevice>();
        if (device->open(choice.asioDriver, window, error)) return device;
        return nullptr;
    }
    auto device = std::make_unique<WasapiDevice>();
    if (device->open(choice.output, choice.input, choice.noInput, error)) return device;
    return nullptr;
}

Settings loadSettings() {
    Settings settings;
    const auto path = settingsPath();
    if (path.empty()) return settings;
    std::ifstream in(path);
    std::string line;
    while (std::getline(in, line)) {
        if (!line.empty() && line.back() == '\r') line.pop_back();
        const auto eq = line.find('=');
        if (eq == std::string::npos) continue;
        const std::string key = line.substr(0, eq);
        const std::string value = line.substr(eq + 1);
        if (key == "aufnahme_versatz_ms") settings.recordOffsetMs = std::atoi(value.c_str());
        else if (key == "midi_ausgang") settings.midiOutput = fromUtf8(value);
        else if (key == "audio_ausgang") settings.audioOutput = fromUtf8(value);
        else if (key == "audio_eingang") settings.audioInput = fromUtf8(value);
        else if (key == "kein_eingang") settings.noInput = value == "1";
        else if (key == "audio_art") settings.asio = value == "asio";
        else if (key == "asio_treiber") settings.asioDriver = fromUtf8(value);
        else if (key == "latenz") {
            // latenz=<ms> <Geraeteschluessel>
            const auto space = value.find(' ');
            if (space != std::string::npos)
                settings.latencyByDevice[fromUtf8(value.substr(space + 1))] = std::atoi(value.substr(0, space).c_str());
        }
    }
    return settings;
}

void saveSettings(const Settings& settings) {
    const auto path = settingsPath();
    if (path.empty()) return;
    std::error_code ec;
    std::filesystem::create_directories(path.parent_path(), ec);
    std::ofstream out(path, std::ios::trunc);
    out << "aufnahme_versatz_ms=" << settings.recordOffsetMs << "\n"
        << "midi_ausgang=" << toUtf8(settings.midiOutput) << "\n"
        << "audio_ausgang=" << toUtf8(settings.audioOutput) << "\n"
        << "audio_eingang=" << toUtf8(settings.audioInput) << "\n"
        << "kein_eingang=" << (settings.noInput ? 1 : 0) << "\n"
        << "audio_art=" << (settings.asio ? "asio" : "wasapi") << "\n"
        << "asio_treiber=" << toUtf8(settings.asioDriver) << "\n";
    for (const auto& [key, ms] : settings.latencyByDevice) out << "latenz=" << ms << ' ' << toUtf8(key) << "\n";
}


const wchar_t* const kProjectFilter = L"MiniDAW-Projekt (*.mdaw)\0*.mdaw\0Alle Dateien (*.*)\0*.*\0";
const wchar_t* const kExportFilter =
    L"WAV, 24 Bit (*.wav)\0*.wav\0WAV, 16 Bit (*.wav)\0*.wav\0WAV, 32 Bit Float (*.wav)\0*.wav\0";

// Zahl mit einer Nachkommastelle und deutschem Komma.
std::wstring decimal(double value) {
    wchar_t buffer[32];
    std::swprintf(buffer, 32, L"%.1f", value);
    std::wstring text = buffer;
    for (auto& c : text)
        if (c == L'.') c = L',';
    return text;
}

int runApp(HINSTANCE instance, int showCommand, const std::filesystem::path& initialProject) {
    auto engine = std::make_unique<Engine>();
    DiskThread disk;
    std::string error;
    Settings settings = loadSettings();

    // Zuletzt gewaehlte Geraete oeffnen; fehlt eins (abgesteckt, ASIO-Treiber
    // belegt), die Windows-Standardgeraete. Das Fenster gibt es noch nicht.
    std::wstring startupNotice;
    AudioChoice current = choiceFrom(settings);
    std::unique_ptr<AudioDevice> device = openAudio(current, nullptr, error);
    if (!device) {
        const std::string reason = error;
        current = AudioChoice{};
        device = openAudio(current, nullptr, error);
        if (!device) {
            showError("Audiogeraet nicht verfuegbar: " + error);
            return 1;
        }
        startupNotice = L"Das zuletzt gew\u00E4hlte Audioger\u00E4t fehlt, es l\u00E4uft das Windows-Standardger\u00E4t.";
        if (settings.asio) startupNotice += L" (ASIO: " + fromUtf8(reason) + L")";
    }

    ProjectController controller(*engine);
    controller.setSampleRate(device->sampleRate());
    controller.setInputChannels(device->inputChannels());
    controller.setLatencyProvider([&device] {
        return static_cast<std::int64_t>(device->inputLatencyFrames() + device->outputLatencyFrames());
    });

    // Latenz-Zusatz der aktuellen Geraetekombination, sonst der alte allgemeine Wert.
    const auto latencyFor = [&settings](const AudioDevice& d) {
        const auto it = settings.latencyByDevice.find(d.settingsKey());
        return it != settings.latencyByDevice.end() ? it->second : settings.recordOffsetMs;
    };
    controller.setRecordOffsetMs(latencyFor(*device));
    engine->setOutputLatencyFrames(device->outputLatencyFrames());

    // Aufnahmen landen unter Dokumente\MiniDAW\Aufnahmen.
    PWSTR documents = nullptr;
    if (SUCCEEDED(SHGetKnownFolderPath(FOLDERID_Documents, 0, nullptr, &documents)) && documents) {
        controller.setRecordingFolder(std::filesystem::path(documents) / L"MiniDAW" / L"Aufnahmen");
    }
    if (documents) CoTaskMemFree(documents);

    if (!device->start(*engine, error)) {
        showError("Audio-Start fehlgeschlagen: " + error);
        return 1;
    }
    disk.start(*engine);

    MidiInput midi(*engine);
    midi.open();
    MidiOutput midiOut(*engine);
    if (!settings.midiOutput.empty()) midiOut.select(settings.midiOutput);

    // VST3-Plugins: geladen wird mit Samplerate und groesster Blocklaenge des Audiogeraets.
    vst3::PluginManager plugins(controller, device->sampleRate(), device->bufferFrames());
    controller.setPluginPersistence(&plugins);

    ui::MainView view(controller, *engine);
    // Zeile fuer die Statusleiste: Geraete, Samplerate, Puffer.
    const auto describeDevice = [&device, &current] {
        if (device->sampleRate() <= 0.0) return std::wstring(L"Kein Audioger\u00E4t ge\u00F6ffnet (Men\u00FC Audio)");
        wchar_t info[320];
        if (current.asio)
            std::swprintf(info, 320, L"ASIO: %ls, %.0f Hz, Puffer %.1f ms  \u2022  %d Eing\u00E4nge",
                          device->outputName().c_str(), device->sampleRate(),
                          1000.0 * device->periodFrames() / device->sampleRate(), device->inputChannels());
        else
            std::swprintf(info, 320, L"%ls, %.0f Hz, Puffer %.1f ms  \u2022  Eingang: %ls (%d)",
                          device->outputName().c_str(), device->sampleRate(),
                          1000.0 * device->periodFrames() / device->sampleRate(),
                          device->inputChannels() == 0 ? L"keiner" : device->inputName().c_str(), device->inputChannels());
        std::wstring text = info;
        if (device->inputChannels() == 0 && !device->inputError().empty()) {
            const std::string& reason = device->inputError();  // reines ASCII
            text += L" (" + std::wstring(reason.begin(), reason.end()) + L")";
        }
        return text;
    };
    view.setDeviceInfo(describeDevice());
    if (!startupNotice.empty()) view.showMessage(startupNotice);
    view.setMidiInfo(midi.summary());
    view.setMidiOutInfo(midiOut.current());
    view.setPluginHost(&plugins);
    view.onCycleMidiOut = [&midiOut, &view] {
        midiOut.cycle();
        view.setMidiOutInfo(midiOut.current());
    };

    ui::MainWindow window(view);

    // --- Datei-Befehle ---
    std::wstring shownTitle;
    const auto updateTitle = [&] {
        std::wstring title = controller.projectPath().empty() ? std::wstring(L"Unbenannt")
                                                              : controller.projectPath().stem().wstring();
        if (controller.modified()) title += L" *";
        title += L" \u2013 MiniDAW";
        if (title != shownTitle) {
            shownTitle = title;
            window.setTitle(title);
        }
    };
    const auto saveTo = [&](const std::filesystem::path& path) {
        std::string message;
        if (controller.saveProject(path, message)) {
            view.showMessage(L"Gespeichert: " + path.filename().wstring());
            return true;
        }
        MessageBoxW(window.hwnd(), (L"Speichern fehlgeschlagen.\n\n" + fromUtf8(message)).c_str(), L"MiniDAW",
                    MB_OK | MB_ICONERROR);
        return false;
    };
    const auto saveAs = [&] {
        const std::wstring suggestion =
            controller.projectPath().empty() ? std::wstring(L"Projekt.mdaw") : controller.projectPath().wstring();
        const auto path = ui::askSaveFile(window.hwnd(), L"Projekt speichern", kProjectFilter, L"mdaw", suggestion);
        return !path.empty() && saveTo(path);
    };
    const auto save = [&] { return controller.projectPath().empty() ? saveAs() : saveTo(controller.projectPath()); };
    // Fragt bei ungespeicherten Aenderungen nach. true = weitermachen, false = abbrechen.
    const auto confirmDiscard = [&] {
        if (!controller.modified()) return true;
        const int answer = MessageBoxW(window.hwnd(), L"Das Projekt wurde ge\u00E4ndert. Vorher speichern?",
                                       L"MiniDAW", MB_YESNOCANCEL | MB_ICONQUESTION);
        if (answer == IDYES) return save();
        return answer == IDNO;
    };
    const auto openProject = [&](const std::filesystem::path& path) {
        std::string message;
        if (!controller.loadProject(path, message)) {
            MessageBoxW(window.hwnd(), (L"\u00D6ffnen fehlgeschlagen.\n\n" + fromUtf8(message)).c_str(), L"MiniDAW",
                        MB_OK | MB_ICONERROR);
            return;
        }
        view.projectReplaced();
        if (!message.empty())
            MessageBoxW(window.hwnd(), fromUtf8(message).c_str(), L"MiniDAW", MB_OK | MB_ICONWARNING);
    };
    // --- Latenzmessung ---
    std::unique_ptr<LatencyProbe> probe;
    std::chrono::steady_clock::time_point probeStarted;
    // Sonden, deren Messung nie fertig wurde: Die Engine koennte sie noch
    // anfassen, deshalb werden sie erst beim Beenden freigegeben.
    std::vector<std::unique_ptr<LatencyProbe>> abandonedProbes;

    const auto measureLatency = [&] {
        if (probe) return;
        if (controller.isRecording()) {
            view.showMessage(L"W\u00E4hrend einer Aufnahme l\u00E4sst sich nicht messen.");
            return;
        }
        if (device->inputChannels() == 0) {
            MessageBoxW(window.hwnd(),
                        L"Es ist kein Aufnahmeger\u00E4t ge\u00F6ffnet. Im Men\u00FC Audio ein Aufnahmeger\u00E4t "
                        L"oder einen ASIO-Treiber w\u00E4hlen.",
                        L"Latenz messen", MB_OK | MB_ICONWARNING);
            return;
        }
        const int answer = MessageBoxW(
            window.hwnd(),
            L"So wird gemessen:\n\n"
            L"1.  Ein Kabel von Ausgang 1 oder 2 der Soundkarte in einen Eingang stecken. Beim Behringer "
            L"UMC404HD eignen sich die PLAYBACK OUTPUTS 1/2 auf der R\u00FCckseite.\n\n"
            L"2.  Den MIX-Regler (Direct Monitoring) ganz auf Wiedergabe drehen. Sonst kann die Schleife "
            L"aus Kabel und Mith\u00F6ren pfeifen.\n\n"
            L"3.  Gain des Eingangs niedrig, Lautsprecher und Kopfh\u00F6rer leise: Es ert\u00F6nen sechs "
            L"kurze Sweeps.\n\n"
            L"Die Messung dauert etwa f\u00FCnf Sekunden. Starten?",
            L"Latenz messen", MB_OKCANCEL | MB_ICONINFORMATION);
        if (answer != IDOK) return;

        engine->stop();
        probe = std::make_unique<LatencyProbe>(device->sampleRate());
        if (!engine->startLatencyProbe(probe.get())) {
            probe.reset();
            view.showMessage(L"Die Messung lie\u00DF sich nicht starten.");
            return;
        }
        probeStarted = std::chrono::steady_clock::now();
        view.showMessage(L"Latenzmessung l\u00E4uft \u2026");
    };

    // Einmal pro Bild: Ist die Messung fertig, auswerten und Ergebnis zeigen.
    const auto checkLatencyProbe = [&] {
        if (!probe) return;
        if (!probe->done()) {
            if (std::chrono::steady_clock::now() - probeStarted > std::chrono::seconds(15)) {
                abandonedProbes.push_back(std::move(probe));
                MessageBoxW(window.hwnd(), L"Die Messung ist nicht fertig geworden. L\u00E4uft das Audioger\u00E4t?",
                            L"Latenz messen", MB_OK | MB_ICONERROR);
            }
            return;
        }
        // Vor dem Dialog aus der Variable nehmen: Waehrend der Dialog offen ist, laeuft diese Funktion weiter.
        const std::unique_ptr<LatencyProbe> finished = std::move(probe);
        const LatencyProbe::Result result = finished->analyze();
        if (!result.ok) {
            MessageBoxW(window.hwnd(), (L"Messung fehlgeschlagen.\n\n" + fromUtf8(result.message)).c_str(),
                        L"Latenz messen", MB_OK | MB_ICONWARNING);
            return;
        }
        const double rate = device->sampleRate();
        const double measuredMs = 1000.0 * result.frames / rate;
        const double estimatedMs = 1000.0 * static_cast<double>(controller.estimatedLatencyFrames()) / rate;
        const double appliedMs = 1000.0 * static_cast<double>(controller.appliedLatencyFrames()) / rate;
        std::wstring text = L"Gemessene Latenz: " + decimal(measuredMs) + L" ms (" +
                            std::to_wstring(static_cast<long long>(result.frames)) + L" Samples bei " +
                            std::to_wstring(static_cast<int>(rate)) + L" Hz)\n\n" +
                            L"Vom Treiber gemeldet: " + decimal(estimatedMs) + L" ms\n" +
                            L"Derzeit eingestellt: " + decimal(appliedMs) + L" ms\n" +
                            L"Streuung der " + std::to_wstring(result.validPulses) + L" Einzelmessungen: " +
                            std::to_wstring(result.spread) + L" Samples\n";
        if (!result.message.empty()) text += L"\n" + fromUtf8(result.message) + L"\n";
        text += L"\nDie gemessene Latenz f\u00FCr Aufnahmen \u00FCbernehmen?";
        if (MessageBoxW(window.hwnd(), text.c_str(), L"Latenz messen", MB_YESNO | MB_ICONQUESTION) == IDYES) {
            controller.applyMeasuredLatency(result.frames);
            view.showMessage(L"Aufnahme-Latenz auf " +
                             decimal(1000.0 * static_cast<double>(controller.appliedLatencyFrames()) / rate) +
                             L" ms gesetzt.");
        }
    };

    // --- Audiogeraete waehlen ---
    std::vector<AudioEndpoint> menuOutputs, menuInputs;
    std::vector<std::wstring> menuAsio;
    window.onAudioMenu = [&](HMENU menu) {
        while (GetMenuItemCount(menu) > 0) DeleteMenu(menu, 0, MF_BYPOSITION);
        menuOutputs = WasapiDevice::endpoints(false);
        menuInputs = WasapiDevice::endpoints(true);
        menuAsio = AsioDevice::drivers();
        if (menuOutputs.size() > 199) menuOutputs.resize(199);
        if (menuInputs.size() > 198) menuInputs.resize(198);
        if (menuAsio.size() > 99) menuAsio.resize(99);
        const bool wasapi = !current.asio;

        AppendMenuW(menu, MF_STRING | MF_GRAYED, 0, L"Wiedergabe (WASAPI)");
        for (std::size_t i = 0; i < menuOutputs.size(); ++i)
            AppendMenuW(menu, MF_STRING | (wasapi && menuOutputs[i].id == current.output ? MF_CHECKED : 0),
                        ui::kAudioOutputBase + i, menuOutputs[i].name.c_str());
        AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
        AppendMenuW(menu, MF_STRING | MF_GRAYED, 0, L"Aufnahme (WASAPI)");
        for (std::size_t i = 0; i < menuInputs.size(); ++i)
            AppendMenuW(menu,
                        MF_STRING | (wasapi && device->inputChannels() > 0 && menuInputs[i].id == current.input ? MF_CHECKED : 0),
                        ui::kAudioInputBase + i, menuInputs[i].name.c_str());
        AppendMenuW(menu, MF_STRING | (wasapi && device->inputChannels() == 0 ? MF_CHECKED : 0), ui::kAudioNoInput,
                    L"Kein Eingang");
        AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
        AppendMenuW(menu, MF_STRING | MF_GRAYED, 0, L"ASIO (Wiedergabe und Aufnahme)");
        if (menuAsio.empty()) AppendMenuW(menu, MF_STRING | MF_GRAYED, 0, L"Kein ASIO-Treiber installiert");
        for (std::size_t i = 0; i < menuAsio.size(); ++i)
            AppendMenuW(menu, MF_STRING | (current.asio && menuAsio[i] == current.asioDriver ? MF_CHECKED : 0),
                        ui::kAudioAsioBase + i, menuAsio[i].c_str());
        AppendMenuW(menu, MF_STRING | (device->hasControlPanel() ? 0 : MF_GRAYED), ui::kAudioAsioPanel,
                    L"ASIO-Einstellungen …");
        AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
        AppendMenuW(menu, MF_STRING, ui::kToolsMeasureLatency, L"Latenz messen …");
    };

    // Wechselt das Geraet im laufenden Betrieb: anhalten, altes schliessen (ein
    // ASIO-Treiber darf nur einmal offen sein), neues oeffnen, Plugins bei Bedarf
    // neu laden, weiterlaufen. Schlaegt das Oeffnen fehl, bleibt es beim alten.
    const auto switchAudio = [&](const AudioChoice& wanted) {
        if (controller.isRecording()) {
            view.showMessage(L"Während einer Aufnahme lässt sich das Gerät nicht wechseln.");
            return;
        }
        if (probe) {
            view.showMessage(L"Erst die Latenzmessung abwarten.");
            return;
        }
        const double oldRate = device->sampleRate();
        const AudioChoice previous = current;
        if (oldRate > 0.0) settings.latencyByDevice[device->settingsKey()] = controller.recordOffsetMs();

        engine->stop();
        device->stop();
        disk.stop();
        device->close();
        device.reset();

        std::string openError;
        device = openAudio(wanted, window.hwnd(), openError);
        if (device) {
            current = wanted;
            storeChoice(settings, current);
        } else {
            std::string ignored;
            device = openAudio(previous, window.hwnd(), ignored);
            if (device) {
                current = previous;
            } else {
                current = AudioChoice{};
                device = openAudio(current, window.hwnd(), ignored);
                if (!device) device = std::make_unique<WasapiDevice>();  // nichts offen
            }
            MessageBoxW(window.hwnd(),
                        (L"Das Gerät ließ sich nicht öffnen. Es bleibt beim bisherigen.\n\n" +
                         fromUtf8(openError)).c_str(),
                        L"MiniDAW", MB_OK | MB_ICONWARNING);
        }

        if (device->sampleRate() > 0.0) {
            controller.setSampleRate(device->sampleRate());
            controller.setInputChannels(device->inputChannels());
            controller.setRecordOffsetMs(latencyFor(*device));
            engine->setOutputLatencyFrames(device->outputLatencyFrames());
            const int lostPlugins = plugins.setAudioFormat(device->sampleRate(), device->bufferFrames());
            disk.start(*engine);
            std::string startError;
            if (!device->start(*engine, startError)) showError("Audio-Start fehlgeschlagen: " + startError);
            if (lostPlugins > 0)
                view.showMessage(std::to_wstring(lostPlugins) + L" Plugin(s) ließen sich mit dem neuen Gerät nicht laden.");
            else if (oldRate > 0.0 && std::fabs(device->sampleRate() - oldRate) > 0.5)
                view.showMessage(L"Achtung: Samplerate jetzt " + std::to_wstring(static_cast<int>(device->sampleRate())) +
                                 L" Hz. Vorhandene Clips stimmen zeitlich nicht mehr.");
        }
        view.setDeviceInfo(describeDevice());
    };

    const auto exportWav = [&] {
        if (probe) {
            view.showMessage(L"Erst die Latenzmessung abwarten.");
            return;
        }
        if (controller.isRecording()) {
            view.showMessage(L"W\u00E4hrend einer Aufnahme l\u00E4sst sich nicht exportieren.");
            return;
        }
        const std::int64_t content = controller.contentLength();
        if (content <= 0) {
            view.showMessage(L"Das Projekt ist leer, es gibt nichts zu exportieren.");
            return;
        }
        const std::wstring suggestion =
            (controller.projectPath().empty() ? std::wstring(L"Export") : controller.projectPath().stem().wstring()) + L".wav";
        int filterIndex = 1;
        const auto path = ui::askSaveFile(window.hwnd(), L"Als WAV exportieren", kExportFilter, L"wav", suggestion, &filterIndex);
        if (path.empty()) return;
        const WavFormat format = filterIndex == 2 ? WavFormat::Pcm16 : filterIndex == 3 ? WavFormat::Float32 : WavFormat::Pcm24;

        // Zwei Sekunden anhaengen, damit Hall und ausklingende Noten nicht abgeschnitten werden.
        const auto rate = static_cast<int>(device->sampleRate() + 0.5);
        const std::int64_t frames = content + 2 * static_cast<std::int64_t>(rate);

        // Audiogeraet und Disk-Thread anhalten: Solange exportiert wird, treibt
        // nur dieser Thread die Engine an. MIDI-Geraete bekommen nichts.
        const bool metronome = controller.metronome();
        controller.setMetronome(false);
        device->stop();
        disk.stop();
        midiOut.setMuted(true);

        std::string message;
        const bool ok = renderOffline(*engine, frames, device->bufferFrames(), rate, path, format, message,
                                      [&](double fraction) {
                                          wchar_t title[64];
                                          std::swprintf(title, 64, L"Exportiere \u2026 %d %%", static_cast<int>(fraction * 100.0));
                                          window.setTitle(title);
                                          MSG peek;  // haelt das Fenster fuer Windows "ansprechbar"
                                          PeekMessageW(&peek, nullptr, 0, 0, PM_NOREMOVE);
                                          return true;
                                      });
        shownTitle.clear();

        Sleep(30);  // der MIDI-Thread verwirft noch, was beim Export angefallen ist
        midiOut.setMuted(false);
        controller.setMetronome(metronome);
        disk.start(*engine);
        std::string startError;
        if (!device->start(*engine, startError))
            showError("Das Audiogeraet liess sich nach dem Export nicht wieder starten: " + startError);

        if (ok) view.showMessage(L"Exportiert: " + path.filename().wstring());
        else MessageBoxW(window.hwnd(), (L"Export fehlgeschlagen.\n\n" + fromUtf8(message)).c_str(), L"MiniDAW",
                         MB_OK | MB_ICONERROR);
    };
    window.onCommand = [&](int command) {
        // WASAPI: Ausgabe oder Eingang tauschen, das jeweils andere bleibt wie zuletzt eingestellt.
        AudioChoice wasapi{false, {}, settings.audioOutput, settings.audioInput, settings.noInput};
        if (command >= ui::kAudioOutputBase && command < ui::kAudioOutputBase + static_cast<int>(menuOutputs.size())) {
            wasapi.output = menuOutputs[static_cast<std::size_t>(command - ui::kAudioOutputBase)].id;
            switchAudio(wasapi);
            return;
        }
        if (command >= ui::kAudioInputBase && command < ui::kAudioInputBase + static_cast<int>(menuInputs.size())) {
            wasapi.input = menuInputs[static_cast<std::size_t>(command - ui::kAudioInputBase)].id;
            wasapi.noInput = false;
            switchAudio(wasapi);
            return;
        }
        if (command == ui::kAudioNoInput) {
            wasapi.input.clear();
            wasapi.noInput = true;
            switchAudio(wasapi);
            return;
        }
        if (command >= ui::kAudioAsioBase && command < ui::kAudioAsioBase + static_cast<int>(menuAsio.size())) {
            AudioChoice asio;
            asio.asio = true;
            asio.asioDriver = menuAsio[static_cast<std::size_t>(command - ui::kAudioAsioBase)];
            switchAudio(asio);
            return;
        }
        if (command == ui::kAudioAsioPanel) {
            device->showControlPanel();  // aendert der Benutzer etwas, meldet der Treiber das per Reset
            return;
        }
        switch (command) {
            case ui::kFileNew:
                if (confirmDiscard()) {
                    controller.newProject();
                    view.projectReplaced();
                }
                break;
            case ui::kFileOpen:
                if (confirmDiscard()) {
                    const auto path = ui::askOpenFile(window.hwnd(), L"Projekt \u00F6ffnen", kProjectFilter);
                    if (!path.empty()) openProject(path);
                }
                break;
            case ui::kFileSave: save(); break;
            case ui::kFileSaveAs: saveAs(); break;
            case ui::kFileExport: exportWav(); break;
            case ui::kToolsMeasureLatency: measureLatency(); break;
            default: break;
        }
        updateTitle();
    };
    window.onCloseRequest = confirmDiscard;

    // MIDI-Geraete, die spaeter angesteckt oder abgezogen werden, neu erkennen.
    window.onPeriodic = [&] {
        if (midi.refresh()) view.setMidiInfo(midi.summary());
        updateTitle();
    };
    window.onFrame = [&] {
        plugins.idle();
        checkLatencyProbe();
        // Der ASIO-Treiber will neu gestartet werden (Puffer oder Samplerate im
        // Treiberfenster geaendert). Waehrend einer Aufnahme wird damit gewartet.
        if (device->resetRequested() && !controller.isRecording() && !probe) {
            switchAudio(current);
            view.showMessage(L"Audiotreiber neu gestartet: " + std::to_wstring(device->bufferFrames()) + L" Samples Puffer.");
        }
    };
    if (!window.create(instance, showCommand, error)) {
        showError(error);
        return 1;
    }
    plugins.setOwner(window.hwnd());
    updateTitle();
    if (!initialProject.empty()) {
        openProject(initialProject);
        updateTitle();
    }
    const int result = window.run();
    settings.latencyByDevice[device->settingsKey()] = controller.recordOffsetMs();
    settings.midiOutput = midiOut.current();
    saveSettings(settings);
    midi.close();

    // Reihenfolge: erst den Audio-Thread anhalten, dann eine laufende Aufnahme
    // abschliessen (dafuer muss der Disk-Thread noch laufen), dann den Disk-Thread.
    // Plugins werden erst zerstoert, wenn der Audio-Thread sicher steht.
    view.setPluginHost(nullptr);
    controller.setPluginPersistence(nullptr);
    device->stop();
    controller.finishRecordingOnShutdown();
    device->close();
    disk.stop();
    plugins.shutdown();
    controller.releasePlugins();
    return result;
}

}  // namespace

int WINAPI wWinMain(HINSTANCE instance, HINSTANCE, PWSTR, int showCommand) {
    SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
    if (FAILED(CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED))) {
        showError("COM nicht initialisierbar");
        return 1;
    }

    int result = 0;
    int argc = 0;
    LPWSTR* argv = CommandLineToArgvW(GetCommandLineW(), &argc);
    if (argv && argc >= 3 && std::wcscmp(argv[1], L"--screenshot") == 0) {
        result = runScreenshot(argv[2], argc >= 4 ? std::wstring(argv[3]) : std::wstring());
    } else {
        // Eine Projektdatei als Argument (zum Beispiel per "Oeffnen mit") wird gleich geladen.
        result = runApp(instance, showCommand, (argv && argc >= 2) ? std::filesystem::path(argv[1])
                                                                 : std::filesystem::path());
    }
    if (argv) LocalFree(argv);

    CoUninitialize();
    return result;
}
