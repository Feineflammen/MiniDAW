// MiniDAW: Programmstart.
//
// Aufruf ohne Argumente startet die DAW.
// "minidaw --screenshot bild.png" zeichnet die Oberflaeche mit Beispieldaten
// in eine Datei, ohne Fenster und ohne Audiogeraet (fuer automatische Pruefungen).

#include "audio/WasapiDevice.h"
#include "disk/DiskThread.h"
#include "disk/WavFile.h"
#include "engine/Engine.h"
#include "model/ProjectController.h"
#include "ui/MainView.h"
#include "ui/MainWindow.h"

#include <windows.h>
#include <objbase.h>
#include <shellapi.h>
#include <shlobj.h>

#include <cmath>
#include <cstdlib>
#include <cwchar>
#include <filesystem>
#include <fstream>
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

int runScreenshot(const std::filesystem::path& output) {
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
    controller.setInputChannels(2);
    controller.armTrack(3);

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
    view.selectClip(1, 0);
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

void loadSettings(ProjectController& controller) {
    const auto path = settingsPath();
    if (path.empty()) return;
    std::ifstream in(path);
    std::string line;
    while (std::getline(in, line)) {
        const auto eq = line.find('=');
        if (eq == std::string::npos) continue;
        const std::string key = line.substr(0, eq);
        const std::string value = line.substr(eq + 1);
        if (key == "aufnahme_versatz_ms") controller.setRecordOffsetMs(std::atoi(value.c_str()));
    }
}

void saveSettings(const ProjectController& controller) {
    const auto path = settingsPath();
    if (path.empty()) return;
    std::error_code ec;
    std::filesystem::create_directories(path.parent_path(), ec);
    std::ofstream out(path, std::ios::trunc);
    out << "aufnahme_versatz_ms=" << controller.recordOffsetMs() << "\n";
}

int runApp(HINSTANCE instance, int showCommand) {
    auto engine = std::make_unique<Engine>();
    WasapiDevice device;
    DiskThread disk;
    std::string error;

    if (!device.open(error)) {
        showError("Audiogeraet nicht verfuegbar: " + error);
        return 1;
    }

    ProjectController controller(*engine);
    controller.setSampleRate(device.sampleRate());
    controller.setInputChannels(device.inputChannels());
    controller.setLatencyProvider([&device] {
        return static_cast<std::int64_t>(device.inputLatencyFrames() + device.outputLatencyFrames());
    });

    loadSettings(controller);

    // Aufnahmen landen unter Dokumente\MiniDAW\Aufnahmen.
    PWSTR documents = nullptr;
    if (SUCCEEDED(SHGetKnownFolderPath(FOLDERID_Documents, 0, nullptr, &documents)) && documents) {
        controller.setRecordingFolder(std::filesystem::path(documents) / L"MiniDAW" / L"Aufnahmen");
    }
    if (documents) CoTaskMemFree(documents);

    if (!device.start(*engine, error)) {
        showError("Audio-Start fehlgeschlagen: " + error);
        return 1;
    }
    disk.start(*engine);

    ui::MainView view(controller, *engine);
    wchar_t info[160];
    std::swprintf(info, 160, L"%.0f Hz, Puffer %d Frames (%.1f ms), %ls", device.sampleRate(),
                  device.periodFrames(), 1000.0 * device.periodFrames() / device.sampleRate(),
                  device.inputChannels() == 0   ? L"kein Eingang"
                  : device.inputChannels() == 1 ? L"Eingang mono"
                                                : L"Eingang stereo");
    std::wstring deviceInfo = info;
    if (device.inputChannels() == 0 && !device.inputError().empty()) {
        const std::string& reason = device.inputError();  // reines ASCII
        deviceInfo += L" (" + std::wstring(reason.begin(), reason.end()) + L")";
    }
    view.setDeviceInfo(deviceInfo);

    ui::MainWindow window(view);
    if (!window.create(instance, showCommand, error)) {
        showError(error);
        return 1;
    }
    const int result = window.run();
    saveSettings(controller);

    // Reihenfolge: erst den Audio-Thread anhalten, dann eine laufende Aufnahme
    // abschliessen (dafuer muss der Disk-Thread noch laufen), dann den Disk-Thread.
    device.stop();
    controller.finishRecordingOnShutdown();
    disk.stop();
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
        result = runScreenshot(argv[2]);
    } else {
        result = runApp(instance, showCommand);
    }
    if (argv) LocalFree(argv);

    CoUninitialize();
    return result;
}
