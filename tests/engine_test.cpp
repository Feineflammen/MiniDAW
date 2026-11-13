// Offline-Tests der Engine. Laufen ohne Audiogeraet, also auch auf GitHub.

#include "disk/DiskThread.h"
#include "disk/WavFile.h"
#include "engine/Engine.h"
#include "engine/OfflineRender.h"
#include "model/ProjectController.h"
#include "util/SpscQueue.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdlib>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <functional>
#include <string>
#include <memory>
#include <thread>
#include <tuple>
#include <utility>
#include <vector>

namespace {

int failures = 0;

#define CHECK(cond)                                                              \
    do {                                                                         \
        if (!(cond)) {                                                           \
            ++failures;                                                          \
            std::printf("FEHLER %s:%d  %s\n", __FILE__, __LINE__, #cond);        \
        }                                                                        \
    } while (0)

constexpr double kRate = 48000.0;
constexpr int kBlock = 256;

// Kleine Testumgebung: rendert Bloecke und merkt sich Kennwerte.
struct Rig {
    std::unique_ptr<Engine> engine = std::make_unique<Engine>();
    std::vector<float> l, r;
    float lastL = 0.0f, lastR = 0.0f;
    float maxJump = 0.0f;  // groesster Sprung zwischen zwei Samples, ueber alle Bloecke

    explicit Rig(int maxFrames = kBlock) { engine->prepare(kRate, 2, 2, maxFrames); }

    // Eingangssignal fuer den naechsten render()-Aufruf; leer = kein Eingang.
    std::vector<float> inL, inR;

    // Spielt den Disk-Thread: liest vor, bis nichts mehr zu tun ist.
    void service() {
        while (engine->serviceDisk()) {}
    }

    // Rendert einen Block und liefert {Spitze links, Spitze rechts}.
    std::pair<float, float> render(int frames = kBlock) {
        l.assign(static_cast<std::size_t>(frames), 0.0f);
        r.assign(static_cast<std::size_t>(frames), 0.0f);
        float* out[2] = {l.data(), r.data()};
        if (inL.size() >= static_cast<std::size_t>(frames) && inR.size() >= static_cast<std::size_t>(frames)) {
            const float* in[2] = {inL.data(), inR.data()};
            engine->process(in, 2, out, 2, frames);
        } else {
            engine->process(nullptr, 0, out, 2, frames);
        }

        float peakL = 0.0f, peakR = 0.0f;
        for (int i = 0; i < frames; ++i) {
            const float a = l[static_cast<std::size_t>(i)];
            const float b = r[static_cast<std::size_t>(i)];
            peakL = std::max(peakL, std::fabs(a));
            peakR = std::max(peakR, std::fabs(b));
            maxJump = std::max(maxJump, std::max(std::fabs(a - lastL), std::fabs(b - lastR)));
            lastL = a;
            lastR = b;
        }
        return {peakL, peakR};
    }
};

void testStoppedIsSilent() {
    Rig rig;
    rig.engine->setTrackTestTone(0, 220.0f);
    const auto [pl, pr] = rig.render();
    CHECK(pl == 0.0f && pr == 0.0f);
    CHECK(rig.engine->position() == 0);
    CHECK(!rig.engine->isPlaying());
}

void testPlayAdvancesPosition() {
    Rig rig;
    rig.engine->setTrackTestTone(0, 220.0f);
    rig.engine->play();
    float peak = 0.0f;
    for (int i = 0; i < 10; ++i) peak = std::max(peak, rig.render().first);
    CHECK(rig.engine->position() == 10 * kBlock);
    CHECK(rig.engine->isPlaying());
    CHECK(peak > 0.05f);
    CHECK(rig.engine->takeMasterPeak() > 0.05f);
    CHECK(rig.engine->takeTrackPeak(0) > 0.05f);
    CHECK(rig.engine->takeTrackPeak(1) == 0.0f);
}

void testStopFadesAndHolds() {
    Rig rig;
    rig.engine->setTrackTestTone(0, 220.0f);
    rig.engine->play();
    for (int i = 0; i < 4; ++i) rig.render();
    rig.engine->stop();
    rig.render();  // Ausblenden
    const auto posAfterFade = rig.engine->position();
    const auto [pl, pr] = rig.render();
    CHECK(pl == 0.0f && pr == 0.0f);
    CHECK(rig.engine->position() == posAfterFade);
    CHECK(!rig.engine->isPlaying());
}

void testLocate() {
    Rig rig;
    rig.engine->locate(96000);
    rig.render();
    CHECK(rig.engine->position() == 96000);
    rig.engine->play();
    rig.render();
    CHECK(rig.engine->position() == 96000 + kBlock);
}

void testPan() {
    Rig rig;
    rig.engine->setTrackTestTone(0, 220.0f);
    rig.engine->setTrackPan(0, -1.0f);
    rig.engine->play();
    rig.render();  // Ueberblendung abwarten
    rig.render();
    auto [pl, pr] = rig.render();
    CHECK(pl > 0.05f);
    CHECK(pr == 0.0f);

    rig.engine->setTrackPan(0, 1.0f);
    rig.render();
    std::tie(pl, pr) = rig.render();
    CHECK(pl == 0.0f);
    CHECK(pr > 0.05f);

    // Mitte: beide Seiten gleich laut, je -3 dB.
    rig.engine->setTrackPan(0, 0.0f);
    rig.render();
    float peakL = 0.0f, peakR = 0.0f;
    for (int i = 0; i < 4; ++i) {
        std::tie(pl, pr) = rig.render();
        peakL = std::max(peakL, pl);
        peakR = std::max(peakR, pr);
    }
    CHECK(std::fabs(peakL - peakR) < 1e-4f);
    CHECK(std::fabs(peakL - Engine::kTestToneLevel * 0.70710678f) < 0.005f);
}

void testMuteAndSolo() {
    Rig rig;
    rig.engine->setTrackTestTone(0, 220.0f);
    rig.engine->setTrackPan(0, -1.0f);
    rig.engine->setTrackTestTone(1, 330.0f);
    rig.engine->setTrackPan(1, 1.0f);
    rig.engine->play();
    rig.render();
    auto [pl, pr] = rig.render();
    CHECK(pl > 0.05f && pr > 0.05f);

    rig.engine->setTrackMute(0, true);
    rig.render();
    std::tie(pl, pr) = rig.render();
    CHECK(pl == 0.0f && pr > 0.05f);

    rig.engine->setTrackMute(0, false);
    rig.engine->setTrackSolo(0, true);
    rig.render();
    std::tie(pl, pr) = rig.render();
    CHECK(pl > 0.05f && pr == 0.0f);

    // Mute gewinnt gegen Solo.
    rig.engine->setTrackMute(0, true);
    rig.render();
    std::tie(pl, pr) = rig.render();
    CHECK(pl == 0.0f && pr == 0.0f);
}

void testNoClicks() {
    Rig rig;
    rig.engine->setTrackTestTone(0, 220.0f);
    rig.engine->play();
    for (int i = 0; i < 3; ++i) rig.render();
    rig.engine->setTrackMute(0, true);
    for (int i = 0; i < 3; ++i) rig.render();
    rig.engine->setTrackMute(0, false);
    rig.engine->setTrackGain(0, 0.3f);
    rig.engine->setMasterGain(0.5f);
    for (int i = 0; i < 3; ++i) rig.render();
    rig.engine->setMasterGain(1.0f);
    rig.engine->setTrackGain(0, 1.0f);
    for (int i = 0; i < 3; ++i) rig.render();
    rig.engine->stop();
    for (int i = 0; i < 3; ++i) rig.render();

    // Ein 220-Hz-Sinus aendert sich pro Sample hoechstens um Pegel * 2*pi*f/fs,
    // dazu kommt die Ueberblendung mit hoechstens Pegel/Blocklaenge.
    const float natural = Engine::kTestToneLevel * static_cast<float>(6.283185307 * 220.0 / kRate);
    const float ramp = Engine::kTestToneLevel / static_cast<float>(kBlock);
    CHECK(rig.maxJump < (natural + ramp) * 1.5f);
}

void testLargeBlock() {
    Rig rig(kBlock);
    rig.engine->setTrackTestTone(0, 220.0f);
    rig.engine->play();
    const auto [pl, pr] = rig.render(1000);  // groesser als maxFrames
    CHECK(pl > 0.05f && pr > 0.05f);
    CHECK(rig.engine->position() == 1000);
}

void testInvalidTrack() {
    Rig rig;
    CHECK(!rig.engine->setTrackGain(-1, 1.0f));
    CHECK(!rig.engine->setTrackGain(Engine::kNumTracks, 1.0f));
    CHECK(rig.engine->setTrackGain(Engine::kNumTracks - 1, 1.0f));
}

void testQueueTwoThreads() {
    auto queue = std::make_unique<SpscQueue<int, 256>>();
    constexpr int kCount = 500000;
    bool inOrder = true;

    std::thread reader([&] {
        int expected = 0;
        int value = 0;
        while (expected < kCount) {
            if (queue->pop(value)) {
                if (value != expected) inOrder = false;
                ++expected;
            } else {
                std::this_thread::yield();
            }
        }
    });
    for (int i = 0; i < kCount; ++i) {
        while (!queue->push(i)) std::this_thread::yield();
    }
    reader.join();
    CHECK(inOrder);

    int dummy = 0;
    CHECK(!queue->pop(dummy));
    for (int i = 0; i < 256; ++i) CHECK(queue->push(i));
    CHECK(!queue->push(0));  // voll
}

// --- WAV-Dateien und Clip-Wiedergabe -----------------------------------------

std::filesystem::path tempFile(const char* name) {
    return std::filesystem::temp_directory_path() / name;
}

void testWavRoundTrip() {
    struct Case { WavFormat format; int channels; float tolerance; const char* name; };
    const Case cases[] = {
        {WavFormat::Pcm16, 1, 1.0f / 16000.0f, "minidaw_test_16m.wav"},
        {WavFormat::Pcm16, 2, 1.0f / 16000.0f, "minidaw_test_16s.wav"},
        {WavFormat::Pcm24, 2, 1.0f / 4000000.0f, "minidaw_test_24s.wav"},
        {WavFormat::Float32, 1, 0.0f, "minidaw_test_f32m.wav"},
        {WavFormat::Float32, 2, 0.0f, "minidaw_test_f32s.wav"},
    };
    constexpr int kFrames = 1001;

    for (const Case& c : cases) {
        std::vector<float> data(static_cast<std::size_t>(kFrames * c.channels));
        for (int i = 0; i < kFrames; ++i)
            for (int ch = 0; ch < c.channels; ++ch)
                data[static_cast<std::size_t>(i * c.channels + ch)] =
                    static_cast<float>(std::sin(0.05 * i + ch)) * 0.9f;

        const auto path = tempFile(c.name);
        std::string error;
        CHECK(writeWav(path, data.data(), kFrames, c.channels, 44100, c.format, error));

        auto wav = WavFile::open(path, error);
        CHECK(wav != nullptr);
        if (!wav) continue;
        CHECK(wav->frames() == kFrames);
        CHECK(wav->channels() == c.channels);
        CHECK(wav->sampleRate() == 44100);

        // Absichtlich ueber beide Dateigrenzen hinaus lesen.
        constexpr int kLead = 10;
        std::vector<float> out(static_cast<std::size_t>(2 * (kFrames + 2 * kLead)), 1.0f);
        wav->readStereo(-kLead, kFrames + 2 * kLead, out.data());

        bool ok = true;
        for (int i = -kLead; i < kFrames + kLead; ++i) {
            const std::size_t o = static_cast<std::size_t>(2 * (i + kLead));
            float expL = 0.0f, expR = 0.0f;
            if (i >= 0 && i < kFrames) {
                expL = data[static_cast<std::size_t>(i * c.channels)];
                expR = data[static_cast<std::size_t>(i * c.channels + (c.channels > 1 ? 1 : 0))];
            }
            if (std::fabs(out[o] - expL) > c.tolerance || std::fabs(out[o + 1] - expR) > c.tolerance)
                ok = false;
        }
        CHECK(ok);
        wav.reset();
        std::filesystem::remove(path);
    }

    std::string error;
    CHECK(WavFile::open(tempFile("minidaw_gibt_es_nicht.wav"), error) == nullptr);
}

// Inhalt der Testdatei: fuer jeden Frame eindeutig berechenbar.
constexpr std::int64_t kSourceFrames = 300000;
float sourceL(std::int64_t i) { return 0.5f * static_cast<float>(std::sin(0.01 * static_cast<double>(i))); }
float sourceR(std::int64_t i) { return 0.25f * static_cast<float>(std::cos(0.013 * static_cast<double>(i))); }

std::shared_ptr<WavFile> makeSource() {
    std::vector<float> data(static_cast<std::size_t>(kSourceFrames * 2));
    for (std::int64_t i = 0; i < kSourceFrames; ++i) {
        data[static_cast<std::size_t>(2 * i)] = sourceL(i);
        data[static_cast<std::size_t>(2 * i + 1)] = sourceR(i);
    }
    const auto path = tempFile("minidaw_test_source.wav");
    std::string error;
    CHECK(writeWav(path, data.data(), kSourceFrames, 2, 48000, WavFormat::Float32, error));
    auto wav = WavFile::open(path, error);
    CHECK(wav != nullptr);
    return wav;
}

// Liefert zu einer Zeitachsen-Position den Frame der Quelldatei oder -1 fuer Stille.
using Expected = std::function<std::int64_t(std::int64_t)>;

struct BlockResult {
    bool valid = true;    // jedes Sample ist entweder richtig oder Stille
    bool exact = true;    // jedes Sample ist richtig
};

// Rendert einen Block ab Position start und vergleicht ihn mit der Erwartung.
BlockResult renderAndCompare(Rig& rig, std::int64_t start, const Expected& expected) {
    const float g = std::cos(0.7853981633974483f);  // Panorama Mitte
    rig.render();
    BlockResult result;
    for (int i = 0; i < kBlock; ++i) {
        const std::int64_t src = expected(start + i);
        const float expL = src >= 0 ? sourceL(src) * g : 0.0f;
        const float expR = src >= 0 ? sourceR(src) * g : 0.0f;
        const float l = rig.l[static_cast<std::size_t>(i)];
        const float r = rig.r[static_cast<std::size_t>(i)];
        const bool match = std::fabs(l - expL) < 1e-5f && std::fabs(r - expR) < 1e-5f;
        const bool silent = l == 0.0f && r == 0.0f;
        if (!match) result.exact = false;
        if (!match && !silent) result.valid = false;
    }
    return result;
}

void testClipPlayback() {
    auto source = makeSource();
    if (!source) return;
    Rig rig;
    // Clip beginnt bei 1000, benutzt die Datei ab Frame 500 und ist 5000 Frames lang.
    rig.engine->setTrackPlaylist(0, {Clip{source, 1000, 500, 5000, 1.0f}});
    const Expected expected = [](std::int64_t p) -> std::int64_t {
        return (p >= 1000 && p < 6000) ? 500 + (p - 1000) : -1;
    };

    rig.service();
    rig.render();  // Transport steht, Strom wird vorbereitet
    rig.engine->play();

    bool allExact = true;
    std::int64_t pos = 0;
    for (int b = 0; b < 30; ++b, pos += kBlock) {
        rig.service();
        if (!renderAndCompare(rig, pos, expected).exact) allExact = false;
    }
    CHECK(allExact);
    CHECK(rig.engine->underruns() == 0);
    CHECK(rig.engine->takeTrackPeak(0) > 0.1f);
}

void testLocateWhilePlaying() {
    auto source = makeSource();
    if (!source) return;
    Rig rig;
    rig.engine->setTrackPlaylist(0, {Clip{source, 0, 0, kSourceFrames, 1.0f}});
    const Expected expected = [](std::int64_t p) -> std::int64_t {
        return (p >= 0 && p < kSourceFrames) ? p : -1;
    };

    rig.service();
    rig.render();
    rig.engine->play();
    rig.render();  // Einblenden, nicht verglichen
    std::int64_t pos = kBlock;
    for (int b = 0; b < 4; ++b, pos += kBlock) {
        rig.service();
        CHECK(renderAndCompare(rig, pos, expected).exact);
    }

    for (const std::int64_t target : {std::int64_t{100000}, std::int64_t{50}, std::int64_t{250000}}) {
        rig.engine->locate(target);
        pos = target;
        bool allValid = true;
        int exactBlocks = 0;
        for (int b = 0; b < 6; ++b, pos += kBlock) {
            const BlockResult r = renderAndCompare(rig, pos, expected);
            if (!r.valid) allValid = false;
            if (r.exact) ++exactBlocks;
            rig.service();
        }
        CHECK(allValid);          // nie falsches Material, hoechstens kurz Stille
        CHECK(exactBlocks >= 4);  // spaetestens nach zwei Bloecken wieder da
    }
}

void testUnderrunRecovery() {
    auto source = makeSource();
    if (!source) return;
    Rig rig;
    rig.engine->setTrackPlaylist(0, {Clip{source, 0, 0, kSourceFrames, 1.0f}});
    const Expected expected = [](std::int64_t p) -> std::int64_t {
        return (p >= 0 && p < kSourceFrames) ? p : -1;
    };

    rig.service();
    rig.render();
    rig.engine->play();
    rig.render();
    std::int64_t pos = kBlock;

    // Der Disk-Thread "haengt": so lange spielen, bis der Puffer leer ist.
    bool allValid = true;
    for (int b = 0; b < 270; ++b, pos += kBlock)
        if (!renderAndCompare(rig, pos, expected).valid) allValid = false;
    CHECK(allValid);
    CHECK(rig.engine->underruns() > 0);

    // Der Disk-Thread laeuft wieder: Die Wiedergabe muss samplegenau zurueckkommen.
    int exactBlocks = 0;
    for (int b = 0; b < 6; ++b, pos += kBlock) {
        rig.service();
        const BlockResult r = renderAndCompare(rig, pos, expected);
        if (!r.valid) allValid = false;
        if (r.exact) ++exactBlocks;
    }
    CHECK(allValid);
    CHECK(exactBlocks >= 4);
}

void testPlaylistChangeWhilePlaying() {
    auto source = makeSource();
    if (!source) return;
    Rig rig;
    rig.engine->setTrackPlaylist(0, {Clip{source, 0, 0, kSourceFrames, 1.0f}});
    Expected expected = [](std::int64_t p) -> std::int64_t { return p < kSourceFrames ? p : -1; };

    rig.service();
    rig.render();
    rig.engine->play();
    rig.render();
    std::int64_t pos = kBlock;
    for (int b = 0; b < 8; ++b, pos += kBlock) {
        rig.service();
        CHECK(renderAndCompare(rig, pos, expected).exact);
    }

    // Clip waehrend der Wiedergabe ersetzen: gleiche Datei, anderer Ausschnitt.
    rig.engine->setTrackPlaylist(0, {Clip{source, 0, 1000, 100000, 1.0f}});
    expected = [](std::int64_t p) -> std::int64_t { return p < 100000 ? p + 1000 : -1; };

    bool allValid = true;
    int exactBlocks = 0;
    for (int b = 0; b < 6; ++b, pos += kBlock) {
        rig.service();
        const BlockResult r = renderAndCompare(rig, pos, expected);
        if (!r.valid) allValid = false;
        if (r.exact) ++exactBlocks;
    }
    CHECK(allValid);
    CHECK(exactBlocks >= 4);

    // Alle Clips entfernen: Die Spur wird still.
    rig.engine->setTrackPlaylist(0, {});
    rig.service();
    rig.render();
    const auto [pl, pr] = rig.render();
    CHECK(pl == 0.0f && pr == 0.0f);
}

// Echter Disk-Thread neben dem rendernden Thread, dazu viele Spruenge.
// Geprueft wird, dass nie falsches Material kommt (hoechstens Stille).
void testThreadedSeeking() {
    auto source = makeSource();
    if (!source) return;
    Rig rig;
    rig.engine->setTrackPlaylist(0, {Clip{source, 0, 0, kSourceFrames, 1.0f}});
    const Expected expected = [](std::int64_t p) -> std::int64_t {
        return (p >= 0 && p < kSourceFrames) ? p : -1;
    };

    DiskThread disk;
    disk.start(*rig.engine);
    rig.engine->play();
    rig.render();
    std::int64_t pos = kBlock;

    bool allValid = true;
    int exactBlocks = 0;
    std::uint32_t random = 12345;
    for (int b = 0; b < 1500; ++b, pos += kBlock) {
        const auto blockStart = std::chrono::steady_clock::now();
        if (b % 100 == 99) {
            random = random * 1664525u + 1013904223u;
            pos = static_cast<std::int64_t>(random % 280000u);
            rig.engine->locate(pos);
        }
        const BlockResult r = renderAndCompare(rig, pos, expected);
        if (!r.valid) allValid = false;
        if (r.exact) ++exactBlocks;
        // Etwa im Takt eines echten Audiogeraets rendern (ohne ungenaues sleep).
        while (std::chrono::steady_clock::now() - blockStart < std::chrono::microseconds(300))
            std::this_thread::yield();
    }
    disk.stop();
    CHECK(allValid);
    CHECK(exactBlocks > 200);
}

// --- Uebersichtsdaten und Projektmodell --------------------------------------

void testPeakData() {
    auto source = makeSource();
    if (!source) return;
    PeakData peaks(source->frames());
    CHECK(peaks.count == static_cast<std::size_t>((kSourceFrames + 255) / 256));

    std::atomic<bool> cancel{false};
    buildPeaks(*source, peaks, cancel);
    CHECK(peaks.complete());

    // Stichproben gegen direkt berechnete Werte, einschliesslich des letzten (kurzen) Abschnitts.
    bool ok = true;
    for (const std::size_t b : {std::size_t{0}, std::size_t{7}, std::size_t{500}, peaks.count - 1}) {
        float lo = 0.0f, hi = 0.0f;
        for (std::int64_t i = static_cast<std::int64_t>(b) * 256;
             i < std::min<std::int64_t>(kSourceFrames, static_cast<std::int64_t>(b + 1) * 256); ++i) {
            lo = std::min({lo, sourceL(i), sourceR(i)});
            hi = std::max({hi, sourceL(i), sourceR(i)});
        }
        if (std::fabs(peaks.lo[b] - lo) > 1e-6f || std::fabs(peaks.hi[b] - hi) > 1e-6f) ok = false;
    }
    CHECK(ok);
}

void testProjectController() {
    makeSource();  // legt die Testdatei an
    const auto path = tempFile("minidaw_test_source.wav");

    Rig rig;
    ProjectController controller(*rig.engine);
    controller.setSampleRate(kRate);

    std::string message;
    CHECK(!controller.addClip(0, tempFile("minidaw_gibt_es_nicht.wav"), 0, message));
    CHECK(!message.empty());
    CHECK(!controller.addClip(99, path, 0, message));

    CHECK(controller.addClip(0, path, 1024, message));
    CHECK(message.empty());  // Samplerate passt, kein Hinweis
    CHECK(controller.addClip(0, path, 400000, message));
    CHECK(controller.project().tracks[0].clips.size() == 2);
    CHECK(controller.projectLength() == 400000 + kSourceFrames);
    // Dieselbe Datei wird nur einmal geoeffnet.
    CHECK(controller.project().tracks[0].clips[0].file == controller.project().tracks[0].clips[1].file);

    // Der erste Clip muss so klingen, wie das Modell es sagt.
    Expected expected = [](std::int64_t p) -> std::int64_t {
        return (p >= 1024 && p < 1024 + kSourceFrames) ? p - 1024 : -1;
    };
    rig.service();
    rig.render();
    rig.engine->play();
    rig.render();
    std::int64_t pos = kBlock;
    bool allExact = true;
    for (int b = 0; b < 12; ++b, pos += kBlock) {
        rig.service();
        if (!renderAndCompare(rig, pos, expected).exact) allExact = false;
    }
    CHECK(allExact);

    // Auf eine andere Spur verschieben: Modell und Klang folgen.
    const int index = controller.moveClip(0, 0, 3, 2048);
    CHECK(index == 0);
    CHECK(controller.project().tracks[0].clips.size() == 1);
    CHECK(controller.project().tracks[3].clips.size() == 1);
    CHECK(controller.project().tracks[3].clips[0].start == 2048);
    CHECK(controller.moveClip(0, 5, 1, 0) == -1);

    expected = [](std::int64_t p) -> std::int64_t { return p >= 2048 ? p - 2048 : -1; };
    int exactBlocks = 0;
    bool allValid = true;
    for (int b = 0; b < 8; ++b, pos += kBlock) {
        rig.service();
        const BlockResult r = renderAndCompare(rig, pos, expected);
        if (!r.valid) allValid = false;
        if (r.exact) ++exactBlocks;
    }
    CHECK(allValid);
    CHECK(exactBlocks >= 5);

    controller.setTrackMute(3, true);
    CHECK(controller.project().tracks[3].mute);
    rig.service();
    rig.render();
    const auto [pl, pr] = rig.render();
    CHECK(pl == 0.0f && pr == 0.0f);

    CHECK(controller.removeClip(3, 0));
    CHECK(!controller.removeClip(3, 0));
    CHECK(controller.project().tracks[3].clips.empty());

    while (!controller.peaksIdle()) std::this_thread::yield();
    CHECK(controller.project().tracks[0].clips[0].file->peaks->complete());
}

// --- Aufnahme ----------------------------------------------------------------

// Eingangssignal: fuer jeden Frame seit Testbeginn eindeutig berechenbar.
float inputL(std::int64_t i) { return 0.4f * static_cast<float>(std::sin(0.02 * static_cast<double>(i))); }
float inputR(std::int64_t i) { return 0.3f * static_cast<float>(std::cos(0.017 * static_cast<double>(i))); }

// Rendert einen Block mit Eingangssignal ab dem laufenden Zaehler.
void renderWithInput(Rig& rig, std::int64_t& counter) {
    rig.inL.resize(kBlock);
    rig.inR.resize(kBlock);
    for (int i = 0; i < kBlock; ++i) {
        rig.inL[static_cast<std::size_t>(i)] = inputL(counter + i);
        rig.inR[static_cast<std::size_t>(i)] = inputR(counter + i);
    }
    rig.render();
    counter += kBlock;
}

void testRecording() {
    const auto path = tempFile("minidaw_test_record.wav");
    std::string error;
    auto sink = RecordSink::create(path, 2, 48000, error);
    CHECK(sink != nullptr);
    if (!sink) return;

    Rig rig;
    std::int64_t counter = 0;
    rig.engine->setArmedTrack(2);
    rig.engine->locate(5000);
    renderWithInput(rig, counter);  // Transport steht noch
    CHECK(!rig.engine->isRecording());
    CHECK(rig.engine->takeInputPeak() > 0.2f);  // Eingangspegel wird schon angezeigt

    const std::int64_t firstRecorded = counter;
    CHECK(rig.engine->record(sink));
    constexpr int kBlocks = 40;
    for (int b = 0; b < kBlocks; ++b) {
        renderWithInput(rig, counter);
        if (b % 3 == 0) rig.service();
    }
    CHECK(rig.engine->isRecording());
    CHECK(rig.engine->isPlaying());
    CHECK(!sink->done());

    rig.engine->stop();
    renderWithInput(rig, counter);  // Ausblend-Block, wird nicht mehr aufgenommen
    renderWithInput(rig, counter);
    CHECK(!rig.engine->isRecording());
    rig.service();
    CHECK(sink->done());
    CHECK(sink->startPosition() == 5000);
    CHECK(sink->framesCaptured() == kBlocks * kBlock);
    CHECK(sink->overruns() == 0);
    rig.engine->releaseRecordSink();

    // Die Datei muss genau das Eingangssignal der aufgenommenen Bloecke enthalten.
    auto wav = WavFile::open(path, error);
    CHECK(wav != nullptr);
    if (wav) {
        CHECK(wav->frames() == kBlocks * kBlock);
        CHECK(wav->channels() == 2);
        std::vector<float> data(static_cast<std::size_t>(2 * wav->frames()));
        wav->readStereo(0, static_cast<int>(wav->frames()), data.data());
        bool ok = true;
        for (std::int64_t i = 0; i < wav->frames(); ++i) {
            if (data[static_cast<std::size_t>(2 * i)] != inputL(firstRecorded + i) ||
                data[static_cast<std::size_t>(2 * i + 1)] != inputR(firstRecorded + i))
                ok = false;
        }
        CHECK(ok);
    }
    wav.reset();
    sink.reset();
    std::filesystem::remove(path);
}

void testInputMonitor() {
    Rig rig;
    std::int64_t counter = 0;
    rig.engine->setArmedTrack(1);
    rig.engine->setTrackPan(1, -1.0f);  // ganz links: Verstaerkung links 1, rechts 0
    renderWithInput(rig, counter);
    // Ohne Mithoeren bleibt der Ausgang still.
    bool silent = true;
    for (int i = 0; i < kBlock; ++i)
        if (rig.l[static_cast<std::size_t>(i)] != 0.0f || rig.r[static_cast<std::size_t>(i)] != 0.0f) silent = false;
    CHECK(silent);

    rig.engine->setInputMonitor(true);
    renderWithInput(rig, counter);
    const std::int64_t blockStart = counter;
    renderWithInput(rig, counter);
    bool ok = true;
    for (int i = 0; i < kBlock; ++i) {
        if (std::fabs(rig.l[static_cast<std::size_t>(i)] - inputL(blockStart + i)) > 1e-6f) ok = false;
        if (rig.r[static_cast<std::size_t>(i)] != 0.0f) ok = false;
    }
    CHECK(ok);  // Eingang laeuft bei stehendem Transport durch Fader und Panorama
    CHECK(rig.engine->position() == 0);

    rig.engine->setArmedTrack(-1);
    renderWithInput(rig, counter);
    silent = true;
    for (int i = 0; i < kBlock; ++i)
        if (rig.l[static_cast<std::size_t>(i)] != 0.0f) silent = false;
    CHECK(silent);
}

void testControllerRecording() {
    const auto folder = tempFile("minidaw_test_aufnahmen");
    std::error_code ec;
    std::filesystem::remove_all(folder, ec);

    Rig rig;
    ProjectController controller(*rig.engine);
    controller.setSampleRate(kRate);
    std::int64_t counter = 0;
    std::string message;

    // Voraussetzungen werden geprueft und verstaendlich gemeldet.
    CHECK(!controller.startRecording(message));  // kein Eingang
    CHECK(!message.empty());
    controller.setInputChannels(2);
    CHECK(!controller.startRecording(message));  // keine Spur scharf
    controller.armTrack(4);
    CHECK(!controller.startRecording(message));  // kein Ordner
    controller.setRecordingFolder(folder);
    controller.setLatencyProvider([] { return std::int64_t{1000}; });

    // Erste Aufnahme ab Position 0: Der Clip wuerde vor dem Nullpunkt beginnen
    // und wird vorn um die Latenz gekuerzt.
    CHECK(controller.startRecording(message));
    CHECK(controller.isRecording());
    CHECK(!controller.startRecording(message));  // laeuft schon
    for (int b = 0; b < 20; ++b) {
        renderWithInput(rig, counter);
        rig.service();
    }
    std::int64_t start = -1, frames = -1;
    CHECK(controller.recordProgress(start, frames));
    CHECK(start == 0 && frames == 20 * kBlock);
    CHECK(!controller.poll(message));  // noch nicht beendet

    rig.engine->stop();
    renderWithInput(rig, counter);
    renderWithInput(rig, counter);
    rig.service();
    CHECK(controller.poll(message));
    CHECK(!controller.isRecording());
    const auto& clips = controller.project().tracks[4].clips;
    CHECK(clips.size() == 1);
    if (clips.size() == 1) {
        CHECK(clips[0].start == 0);
        CHECK(clips[0].offset == 1000);
        CHECK(clips[0].length == 20 * kBlock - 1000);
        CHECK(std::filesystem::exists(clips[0].file->path));
    }

    // Zweite Aufnahme ab Position 50000: Der Clip liegt um die Latenz frueher.
    rig.engine->locate(50000);
    renderWithInput(rig, counter);
    CHECK(controller.startRecording(message));
    for (int b = 0; b < 10; ++b) {
        renderWithInput(rig, counter);
        rig.service();
    }
    rig.engine->stop();
    renderWithInput(rig, counter);
    rig.service();
    CHECK(controller.poll(message));
    CHECK(clips.size() == 2);
    if (clips.size() == 2) {
        CHECK(clips[1].start == 49000);
        CHECK(clips[1].offset == 0);
        CHECK(clips[1].length == 10 * kBlock);
        CHECK(clips[1].file != clips[0].file);
    }

    // Der einstellbare Zusatz verschiebt weiter: 1000 Frames + 10 ms (480 Frames).
    controller.setRecordOffsetMs(10);
    CHECK(controller.estimatedLatencyFrames() == 1000);
    CHECK(controller.appliedLatencyFrames() == 1480);
    rig.engine->locate(200000);
    renderWithInput(rig, counter);
    CHECK(controller.startRecording(message));
    for (int b = 0; b < 4; ++b) {
        renderWithInput(rig, counter);
        rig.service();
    }
    rig.engine->stop();
    renderWithInput(rig, counter);
    rig.service();
    CHECK(controller.poll(message));
    CHECK(clips.size() == 3);
    if (clips.size() == 3) CHECK(clips[2].start == 200000 - 1480);
    controller.setRecordOffsetMs(-5000);  // wird begrenzt, Latenz nie negativ
    CHECK(controller.recordOffsetMs() == -500);
    CHECK(controller.appliedLatencyFrames() == 0);
    controller.setRecordOffsetMs(0);

    // Beenden waehrend einer laufenden Aufnahme hinterlaesst einen Clip.
    CHECK(controller.startRecording(message));
    for (int b = 0; b < 5; ++b) renderWithInput(rig, counter);
    DiskThread disk;
    disk.start(*rig.engine);
    controller.finishRecordingOnShutdown();
    disk.stop();
    CHECK(!controller.isRecording());
    CHECK(clips.size() == 4);

    while (!controller.peaksIdle()) std::this_thread::yield();
}

// --- MIDI und Synthesizer ----------------------------------------------------

// Zaehlt die Nulldurchgaenge nach oben und liefert daraus die Grundfrequenz.
double measureFrequency(const std::vector<float>& signal, double rate) {
    int crossings = 0;
    std::size_t first = 0, last = 0;
    for (std::size_t i = 1; i < signal.size(); ++i) {
        if (signal[i - 1] <= 0.0f && signal[i] > 0.0f) {
            if (crossings == 0) first = i;
            last = i;
            ++crossings;
        }
    }
    if (crossings < 2) return 0.0;
    return (crossings - 1) * rate / static_cast<double>(last - first);
}

void testSynth() {
    Synth synth;
    synth.prepare(kRate);
    CHECK(!synth.active());

    std::vector<float> l(48000, 0.0f), r(48000, 0.0f);
    synth.handle(MidiMessage::noteOn(69, 100));  // A4 = 440 Hz
    CHECK(synth.active());
    synth.render(l.data(), r.data(), 48000);
    const double hz = measureFrequency(l, kRate);
    CHECK(std::fabs(hz - 440.0) < 1.0);
    float peak = 0.0f, maxJump = 0.0f;
    for (std::size_t i = 0; i < l.size(); ++i) {
        peak = std::max(peak, std::fabs(l[i]));
        if (i > 0) maxJump = std::max(maxJump, std::fabs(l[i] - l[i - 1]));
        if (l[i] != r[i]) peak = 99.0f;
    }
    CHECK(peak > 0.05f && peak < 0.5f);
    CHECK(maxJump < 0.1f);  // kein Knack beim Einsatz

    // Nach dem Loslassen klingt die Note aus, die Stimme wird frei.
    synth.handle(MidiMessage::noteOff(69));
    std::fill(l.begin(), l.end(), 0.0f);
    std::fill(r.begin(), r.end(), 0.0f);
    synth.render(l.data(), r.data(), 48000);
    CHECK(!synth.active());
    CHECK(l.back() == 0.0f);

    // Mehr Noten als Stimmen: Die aeltesten werden uebernommen, nichts stuerzt ab.
    for (int n = 0; n < 40; ++n) synth.handle(MidiMessage::noteOn(40 + n, 90));
    synth.render(l.data(), r.data(), 256);
    CHECK(synth.active());

    // Haltepedal: Losgelassene Noten klingen weiter, bis das Pedal hochkommt.
    synth.handle(MidiMessage::allNotesOff());
    synth.render(l.data(), r.data(), 48000);
    CHECK(!synth.active());
    synth.handle({0xB0, 64, 127});
    synth.handle(MidiMessage::noteOn(60, 100));
    synth.handle(MidiMessage::noteOff(60));
    synth.render(l.data(), r.data(), 48000);
    CHECK(synth.active());
    synth.handle({0xB0, 64, 0});
    synth.render(l.data(), r.data(), 48000);
    CHECK(!synth.active());
}

void testLiveMidi() {
    Rig rig;
    // Ohne scharfgeschaltete Spur verpuffen die Noten.
    rig.engine->postMidi(MidiMessage::noteOn(60, 100));
    auto [pl, pr] = rig.render();
    CHECK(pl == 0.0f && pr == 0.0f);

    // Mit scharfgeschalteter Spur klingt die Note, auch bei stehendem Transport,
    // und laeuft durch Panorama und Pegelanzeige der Spur.
    // Eine Spur mit Audio-Eingang ignoriert MIDI.
    rig.engine->setArmedTrack(3);
    rig.engine->postMidi(MidiMessage::noteOn(60, 100));
    rig.render();
    std::tie(pl, pr) = rig.render();
    CHECK(pl == 0.0f && pr == 0.0f);

    rig.engine->setTrackMidiInput(3, Engine::kInputMidiAll);
    rig.engine->setTrackMidiInput(5, Engine::kInputMidiAll);
    rig.engine->setTrackPan(3, 1.0f);
    rig.engine->postMidi(MidiMessage::noteOn(60, 100));
    rig.render();
    std::tie(pl, pr) = rig.render();
    CHECK(pl == 0.0f && pr > 0.02f);
    CHECK(rig.engine->position() == 0);
    CHECK(rig.engine->takeTrackPeak(3) > 0.02f);

    // Spurwechsel laesst die gehaltene Note ausklingen.
    rig.engine->setArmedTrack(5);
    for (int b = 0; b < 200; ++b) rig.render();
    std::tie(pl, pr) = rig.render();
    CHECK(pl == 0.0f && pr == 0.0f);

    // Aus einem zweiten Thread senden (wie der MIDI-Treiber).
    std::thread sender([&] {
        for (int n = 0; n < 50; ++n) {
            rig.engine->postMidi(MidiMessage::noteOn(48 + n % 12, 80));
            rig.engine->postMidi(MidiMessage::noteOff(48 + n % 12));
        }
        rig.engine->postMidi(MidiMessage::noteOn(72, 100));
    });
    for (int b = 0; b < 100; ++b) rig.render();
    sender.join();
    rig.render();
    std::tie(pl, pr) = rig.render();
    CHECK(pl > 0.01f && pr > 0.01f);  // die letzte Note wird noch gehalten
}

// Holt alle MIDI-Nachrichten ab, die die Engine an das MIDI-Geraet schicken will.
std::vector<MidiMessage> drainMidiOut(Engine& engine) {
    std::vector<MidiMessage> out;
    MidiOutEvent e;
    while (engine.popMidiOut(e)) out.push_back(e.message);
    return out;
}

void testMidiChannels() {
    Rig rig;
    rig.engine->setArmedTrack(0);
    rig.engine->setTrackMidiInput(0, 4);  // nur Kanal 5 (gezaehlt ab 0)

    // Falscher Kanal: nichts. Richtiger Kanal: Ton. Rechnertastatur: immer.
    rig.engine->postMidi(MidiMessage::noteOn(60, 100, 0));
    rig.render();
    auto [pl, pr] = rig.render();
    CHECK(pl == 0.0f);
    rig.engine->postMidi(MidiMessage::noteOn(60, 100, 4));
    rig.render();
    std::tie(pl, pr) = rig.render();
    CHECK(pl > 0.01f);
    rig.engine->postMidi(MidiMessage::noteOff(60, 4));
    for (int b = 0; b < 200; ++b) rig.render();
    std::tie(pl, pr) = rig.render();
    CHECK(pl == 0.0f);
    rig.engine->postMidi(MidiMessage::noteOn(62, 100, 0), true);
    rig.render();
    std::tie(pl, pr) = rig.render();
    CHECK(pl > 0.01f);
    rig.engine->postMidi(MidiMessage::noteOff(62, 0), true);
    for (int b = 0; b < 200; ++b) rig.render();

    // Ausgang auf Kanal 10: Der Synthesizer schweigt, die Nachricht geht mit
    // umgeschriebenem Kanal an das MIDI-Geraet.
    drainMidiOut(*rig.engine);
    rig.engine->setTrackMidiOutput(0, 9);
    rig.render();
    drainMidiOut(*rig.engine);  // Verstummen-Nachrichten vom Umschalten
    rig.engine->postMidi(MidiMessage::noteOn(64, 90, 4));
    rig.render();
    std::tie(pl, pr) = rig.render();
    CHECK(pl == 0.0f && pr == 0.0f);
    auto sent = drainMidiOut(*rig.engine);
    CHECK(sent.size() == 1);
    if (sent.size() == 1) {
        CHECK(sent[0].status == 0x99);  // Note an, Kanal 10
        CHECK(sent[0].data1 == 64 && sent[0].data2 == 90);
    }

    // Spurwechsel schickt dem Geraet "alle Noten aus" auf dem Kanal der Spur.
    rig.engine->setArmedTrack(-1);
    rig.render();
    sent = drainMidiOut(*rig.engine);
    bool sawAllOff = false;
    for (const auto& m : sent)
        if (m.status == 0xB9 && m.data1 == 123) sawAllOff = true;
    CHECK(sawAllOff);
}

void testMidiClipPlayback() {
    Rig rig;
    ProjectController controller(*rig.engine);
    controller.setSampleRate(kRate);

    // Clip ab Frame 1000 mit zwei Noten; die zweite ragt ueber das Clip-Ende hinaus.
    MidiClipModel clip;
    clip.start = 1000;
    clip.length = 4000;
    clip.notes = {{100, 1000, 69, 100}, {3000, 5000, 72, 80}};
    CHECK(controller.addMidiClip(2, clip) == 0);
    CHECK(controller.addMidiClip(2, MidiClipModel{}) == -1);  // Laenge 0

    // Wiedergabe ueber den eingebauten Synthesizer: Der Einsatz ist samplegenau.
    rig.engine->play();
    std::vector<float> all;
    for (int b = 0; b < 200; ++b) {
        rig.render();
        all.insert(all.end(), rig.l.begin(), rig.l.end());
    }
    std::int64_t firstSound = -1;
    for (std::size_t i = 0; i < all.size(); ++i)
        if (all[i] != 0.0f) { firstSound = static_cast<std::int64_t>(i); break; }
    // Die Note beginnt bei 1100; das erste Sample ist der Nulldurchgang, hoerbar ab dem zweiten.
    CHECK(firstSound >= 1100 && firstSound <= 1102);
    // Nach Clip-Ende (5000) plus Ausklang ist es still.
    bool silentTail = true;
    for (std::size_t i = 40000; i < all.size(); ++i)
        if (all[i] != 0.0f) silentTail = false;
    CHECK(silentTail);

    // Dieselben Daten an ein MIDI-Geraet auf Kanal 3: Reihenfolge und Kanal stimmen.
    rig.engine->stop();
    rig.render();
    rig.render();
    controller.setTrackMidiOutput(2, 2);
    rig.engine->locate(0);
    rig.render();
    drainMidiOut(*rig.engine);
    rig.engine->play();
    for (int b = 0; b < 40; ++b) rig.render();
    auto sent = drainMidiOut(*rig.engine);
    CHECK(sent.size() == 4);
    if (sent.size() == 4) {
        CHECK(sent[0].status == 0x92 && sent[0].data1 == 69);
        CHECK(sent[1].status == 0x82 && sent[1].data1 == 69);
        CHECK(sent[2].status == 0x92 && sent[2].data1 == 72);
        CHECK(sent[3].status == 0x82 && sent[3].data1 == 72);  // am Clip-Ende beendet
    }

    // Stopp mitten in einer Note schickt "Note aus".
    rig.engine->stop();
    rig.render();
    rig.engine->locate(1000);
    rig.render();
    drainMidiOut(*rig.engine);
    rig.engine->play();
    rig.render();  // 1000..1255: Note 69 beginnt bei 1100
    rig.engine->stop();
    rig.render();
    sent = drainMidiOut(*rig.engine);
    CHECK(sent.size() == 2);
    if (sent.size() == 2) CHECK(sent[0].isNoteOn() && sent[1].isNoteOff() && sent[1].data1 == 69);

    // Verschieben und Loeschen.
    CHECK(controller.moveMidiClip(2, 0, 6, 0) == 0);
    CHECK(controller.project().tracks[2].midiClips.empty());
    CHECK(controller.project().tracks[6].midiClips.size() == 1);
    CHECK(controller.removeMidiClip(6, 0));
    CHECK(!controller.removeMidiClip(6, 0));
    std::string message;
    rig.render();
    controller.poll(message);  // gibt die alten Sequenzen frei (ASan prueft auf Lecks)
}

void testMidiRecording() {
    Rig rig;
    ProjectController controller(*rig.engine);
    controller.setSampleRate(kRate);
    std::string message;

    controller.armTrack(1);
    controller.setTrackInput(1, kTrackInputMidiAll);
    rig.engine->locate(10000);
    rig.render();

    // Spur mit MIDI-Eingang: Aufnahme braucht weder Audio-Eingang noch Ordner.
    CHECK(controller.startRecording(message));
    CHECK(controller.isRecording());
    rig.render();  // Aufnahme beginnt bei 10000
    controller.poll(message);
    std::int64_t start = 0, frames = 0;
    CHECK(controller.recordProgress(start, frames));
    CHECK(start == 10000);

    for (int b = 0; b < 4; ++b) rig.render();          // bis 11280
    rig.engine->postMidi(MidiMessage::noteOn(60, 90));
    rig.render();                                       // Note an bei etwa 11280
    for (int b = 0; b < 9; ++b) rig.render();          // bis 13840
    rig.engine->postMidi(MidiMessage::noteOff(60));
    rig.engine->postMidi(MidiMessage::noteOn(64, 70));  // bleibt bis zum Ende gedrueckt
    rig.render();
    for (int b = 0; b < 3; ++b) rig.render();
    CHECK(!controller.poll(message));

    rig.engine->stop();
    rig.render();
    CHECK(controller.poll(message));
    CHECK(!controller.isRecording());

    const auto& clips = controller.project().tracks[1].midiClips;
    CHECK(clips.size() == 1);
    CHECK(controller.project().tracks[1].clips.empty());  // keine Audiodatei
    if (clips.size() == 1) {
        const MidiClipModel& clip = clips[0];
        CHECK(clip.start == 10000);
        CHECK(clip.length == 19 * kBlock);
        CHECK(clip.notes.size() == 2);
        if (clip.notes.size() == 2) {
            // Die Positionen stimmen bis auf die Zeit zwischen Eingang und Block (hier fast null).
            CHECK(clip.notes[0].pitch == 60 && clip.notes[0].velocity == 90);
            CHECK(std::llabs(clip.notes[0].start - 5 * kBlock) <= 64);
            CHECK(std::llabs(clip.notes[0].length - 10 * kBlock) <= 64);
            CHECK(clip.notes[1].pitch == 64);
            CHECK(std::llabs(clip.notes[1].start - 15 * kBlock) <= 64);
            CHECK(clip.notes[1].start + clip.notes[1].length == clip.length);  // am Ende abgeschlossen
        }
    }

    // Aufnahme ohne Noten hinterlaesst keinen Clip.
    CHECK(controller.startRecording(message));
    for (int b = 0; b < 3; ++b) rig.render();
    rig.engine->stop();
    rig.render();
    CHECK(!controller.poll(message));
    CHECK(!controller.isRecording());
    CHECK(clips.size() == 1);
}

void testMetronome() {
    Rig rig;
    ProjectController controller(*rig.engine);
    controller.setSampleRate(kRate);
    controller.setBpm(120.0);  // ein Schlag = 24000 Frames
    CHECK(controller.framesPerBeat() == 24000.0);
    controller.setMetronome(true);

    rig.engine->play();
    std::vector<float> all;
    for (int b = 0; b < 200; ++b) {  // 51200 Frames: Schlaege bei 0, 24000 und 48000
        rig.render();
        all.insert(all.end(), rig.l.begin(), rig.l.end());
    }
    const auto loud = [&](std::size_t from, std::size_t to) {
        float peak = 0.0f;
        for (std::size_t i = from; i < to; ++i) peak = std::max(peak, std::fabs(all[i]));
        return peak;
    };
    CHECK(loud(0, 1000) > 0.1f);
    CHECK(loud(2000, 23999) == 0.0f);     // zwischen den Schlaegen still
    CHECK(all[23999] == 0.0f);
    CHECK(loud(24000, 25000) > 0.1f);     // der Klick beginnt genau auf dem Schlag
    CHECK(loud(26000, 47999) == 0.0f);
    CHECK(loud(48000, 49000) > 0.1f);

    // Abgeschaltet: still.
    controller.setMetronome(false);
    rig.engine->locate(0);
    rig.render();
    const auto [pl, pr] = rig.render();
    CHECK(pl == 0.0f && pr == 0.0f);

    // Noten eines Clips ersetzen.
    MidiClipModel clip;
    clip.start = 0;
    clip.length = 1000;
    CHECK(controller.addMidiClip(0, clip) == 0);
    CHECK(controller.updateMidiClip(0, 0, {{500, 100, 64, 90}, {0, 100, 60, 100}}, 2000));
    const auto& stored = controller.project().tracks[0].midiClips[0];
    CHECK(stored.length == 2000 && stored.notes.size() == 2);
    CHECK(stored.notes[0].pitch == 60 && stored.notes[1].pitch == 64);  // nach Zeit sortiert
    CHECK(!controller.updateMidiClip(0, 3, {}, 100));
}

// --- Plugins -----------------------------------------------------------------

// Instrument fuer Tests: gibt 0.5 aus, solange eine Note gehalten wird, und
// merkt sich, was es gesehen hat.
struct FakeInstrument final : AudioPlugin {
    int held = 0;
    int firstNoteOffset = -1;
    int calls = 0;
    PluginTransport lastTransport{};
    bool isInstrument() const noexcept override { return true; }
    void process(float* left, float* right, int numFrames, const PluginMidiEvent* events, int numEvents,
                 const PluginTransport& transport) noexcept override {
        ++calls;
        lastTransport = transport;
        int e = 0;
        for (int i = 0; i < numFrames; ++i) {
            while (e < numEvents && events[e].offset <= i) {
                if (events[e].message.isNoteOn()) {
                    if (firstNoteOffset < 0) firstNoteOffset = events[e].offset;
                    ++held;
                } else if (events[e].message.isNoteOff() && held > 0) {
                    --held;
                }
                ++e;
            }
            left[i] = right[i] = held > 0 ? 0.5f : 0.0f;
        }
    }
};

// Effekt fuer Tests: halbiert den Pegel.
struct FakeEffect final : AudioPlugin {
    bool isInstrument() const noexcept override { return false; }
    void process(float* left, float* right, int numFrames, const PluginMidiEvent*, int,
                 const PluginTransport&) noexcept override {
        for (int i = 0; i < numFrames; ++i) {
            left[i] *= 0.5f;
            right[i] *= 0.5f;
        }
    }
};

void testPlugins() {
    Rig rig;
    FakeInstrument instrument;
    FakeEffect effect;
    AudioPlugin* released = nullptr;

    // Instrument auf Spur 0: Es bekommt das Live-MIDI, der eingebaute Synthesizer schweigt.
    rig.engine->setArmedTrack(0);
    rig.engine->setTrackMidiInput(0, Engine::kInputMidiAll);
    rig.engine->setTrackPan(0, -1.0f);  // ganz links: Verstaerkung 1
    CHECK(rig.engine->setTrackPlugin(0, &instrument));
    rig.render();
    CHECK(instrument.calls > 0);  // laeuft auch bei stehendem Transport
    CHECK(!rig.engine->popReleasedPlugin(released));

    rig.engine->postMidi(MidiMessage::noteOn(60, 100));
    rig.render();
    auto [pl, pr] = rig.render();
    CHECK(std::fabs(pl - 0.5f) < 1e-6f && pr == 0.0f);  // genau der Pegel des Plugins, kein Synthesizer dazu
    rig.engine->postMidi(MidiMessage::noteOff(60));
    rig.render();
    std::tie(pl, pr) = rig.render();
    CHECK(pl == 0.0f);

    // MIDI-Clip: Die Note kommt samplegenau beim Plugin an, mit Transportdaten.
    ProjectController controller(*rig.engine);
    controller.setSampleRate(kRate);
    MidiClipModel clip;
    clip.start = 0;
    clip.length = 2000;
    clip.notes = {{300, 500, 64, 100}};
    controller.addMidiClip(0, clip);
    instrument.firstNoteOffset = -1;
    rig.engine->play();
    rig.render();  // 0..255
    rig.render();  // 256..511: Note bei 300 -> Offset 44
    CHECK(instrument.firstNoteOffset == 44);
    CHECK(instrument.lastTransport.playing);
    CHECK(instrument.lastTransport.position == 256);
    CHECK(instrument.lastTransport.sampleRate == kRate);
    for (int b = 0; b < 10; ++b) rig.render();
    CHECK(instrument.held == 0);  // "Note aus" kam an
    rig.engine->stop();
    rig.render();

    // Effekt auf Spur 1: halbiert den Testton der Spur.
    rig.engine->setTrackTestTone(1, 440.0f);
    rig.engine->setTrackPan(1, -1.0f);
    rig.engine->setTrackMute(0, true);
    rig.engine->locate(0);
    rig.engine->play();
    float dry = 0.0f, wet = 0.0f;
    for (int b = 0; b < 8; ++b) dry = std::max(dry, rig.render().first);
    CHECK(rig.engine->setTrackPlugin(1, &effect));
    rig.render();
    for (int b = 0; b < 8; ++b) wet = std::max(wet, rig.render().first);
    CHECK(std::fabs(dry - Engine::kTestToneLevel) < 0.01f);
    CHECK(std::fabs(wet - 0.5f * dry) < 0.01f);

    // Plugin entfernen und ersetzen: Die Engine meldet, was sie losgelassen hat.
    CHECK(rig.engine->setTrackPlugin(1, nullptr));
    CHECK(!rig.engine->popReleasedPlugin(released));  // erst nach dem naechsten Block
    rig.render();
    CHECK(rig.engine->popReleasedPlugin(released) && released == &effect);
    FakeInstrument second;
    CHECK(rig.engine->setTrackPlugin(0, &second));
    rig.render();
    CHECK(rig.engine->popReleasedPlugin(released) && released == &instrument);
    const int callsAfterRelease = instrument.calls;
    rig.render();
    CHECK(instrument.calls == callsAfterRelease);  // wird wirklich nicht mehr angefasst
    CHECK(!rig.engine->setTrackPlugin(99, &effect));

    rig.engine->setTrackPlugin(0, nullptr);
    rig.render();
    std::string message;
    controller.poll(message);
}

// Der ProjectController haelt Plugins am Leben, bis der Audio-Thread sie loslaesst.
struct Counted final : AudioPlugin {
    static inline int alive = 0;
    bool instrument;
    explicit Counted(bool isInstr) : instrument(isInstr) { ++alive; }
    ~Counted() override { --alive; }
    bool isInstrument() const noexcept override { return instrument; }
    void process(float*, float*, int, const PluginMidiEvent*, int, const PluginTransport&) noexcept override {}
};

void testControllerPlugins() {
    {
        Rig rig;
        ProjectController controller(*rig.engine);
        std::string message;

        controller.setTrackPlugin(2, std::make_shared<Counted>(true), L"Erstes");
        CHECK(controller.project().tracks[2].pluginName == L"Erstes");
        CHECK(controller.project().tracks[2].pluginIsInstrument);
        rig.render();

        // Austausch: Das alte Plugin lebt, bis die Engine es zurueckmeldet.
        controller.setTrackPlugin(2, std::make_shared<Counted>(false), L"Zweites");
        CHECK(!controller.project().tracks[2].pluginIsInstrument);
        CHECK(Counted::alive == 2);
        controller.poll(message);
        CHECK(Counted::alive == 2);  // der Audio-Thread koennte es noch benutzen
        rig.render();
        controller.poll(message);
        CHECK(Counted::alive == 1);

        // Entfernen.
        controller.setTrackPlugin(2, nullptr, L"egal");
        CHECK(controller.project().tracks[2].pluginName.empty());
        rig.render();
        controller.poll(message);
        CHECK(Counted::alive == 0);

        // Was beim Beenden noch geladen ist, gibt releasePlugins() frei.
        controller.setTrackPlugin(5, std::make_shared<Counted>(true), L"Rest");
        rig.render();
        CHECK(Counted::alive == 1);
        controller.releasePlugins();
        CHECK(Counted::alive == 0);
    }
    CHECK(Counted::alive == 0);
}

// --- Projektdatei und Export -------------------------------------------------

// Merkt sich, was gesichert und wiederhergestellt werden soll, statt echte Plugins zu laden.
struct FakePersistence final : PluginPersistence {
    struct Entry {
        std::filesystem::path file;
        std::vector<char> state;
    };
    std::array<Entry, kProjectTracks> slots;
    int restoreCalls = 0;
    bool failRestore = false;

    bool capture(int track, std::filesystem::path& file, std::vector<char>& state) override {
        const Entry& e = slots[static_cast<std::size_t>(track)];
        if (e.file.empty()) return false;
        file = e.file;
        state = e.state;
        return true;
    }
    bool restore(int track, const std::filesystem::path& file, const std::vector<char>& state,
                 std::string& error) override {
        if (!file.empty()) ++restoreCalls;
        if (failRestore && !file.empty()) { error = "Datei fehlt"; return false; }
        slots[static_cast<std::size_t>(track)] = {file, state};
        return true;
    }
};

void testProjectFile() {
    const auto folder = tempFile("minidaw_test_projekt");
    std::error_code ec;
    std::filesystem::remove_all(folder, ec);
    std::filesystem::create_directories(folder / "Audio");
    const auto projectPath = folder / "Mein Projekt.mdaw";

    // Zwei Audiodateien: eine im Projektordner, eine ausserhalb.
    makeSource();
    const auto outside = tempFile("minidaw_test_source.wav");
    const auto inside = folder / "Audio" / "drin.wav";
    std::filesystem::copy_file(outside, inside, std::filesystem::copy_options::overwrite_existing);

    std::string message;
    {
        Rig rig;
        ProjectController controller(*rig.engine);
        controller.setSampleRate(kRate);
        FakePersistence plugins;
        controller.setPluginPersistence(&plugins);
        CHECK(!controller.modified());

        CHECK(controller.addClip(0, outside, 1000, message));
        CHECK(controller.addClip(2, inside, 48000, message));
        MidiClipModel midi;
        midi.name = L"Bass \u00FC";
        midi.start = 96000;
        midi.length = 48000;
        midi.notes = {{0, 1200, 36, 110}, {24000, 600, 43, 64}};
        CHECK(controller.addMidiClip(5, midi) == 0);
        controller.setTrackGain(0, 0.5f);
        controller.setTrackPan(2, -0.25f);
        controller.setTrackMute(5, true);
        controller.setTrackSolo(2, true);
        controller.setTrackInput(5, 3);
        controller.setTrackMidiOutput(5, 9);
        controller.setTrackInput(6, kTrackInputMidiAll);
        controller.setMasterGain(0.8f);
        controller.setBpm(97.0);
        plugins.slots[5] = {"C:/Plugins/Piano.vst3", {1, 2, 3, 0, static_cast<char>(0xFF)}};
        CHECK(controller.modified());
        CHECK(controller.contentLength() == 96000 + 48000 || controller.contentLength() == 48000 + kSourceFrames);

        CHECK(controller.saveProject(projectPath, message));
        CHECK(!controller.modified());
        CHECK(controller.projectPath() == projectPath);
        controller.setTrackGain(1, 0.3f);
        CHECK(controller.modified());

        // Speichern in einen Ordner, den es nicht gibt, scheitert mit Meldung.
        CHECK(!controller.saveProject(folder / "gibtsnicht" / "x.mdaw", message));
        CHECK(!message.empty());
        while (!controller.peaksIdle()) std::this_thread::yield();
    }

    // Die Datei ist lesbarer Text; die Datei im Projektordner steht relativ darin.
    {
        std::ifstream in(projectPath, std::ios::binary);
        std::stringstream text;
        text << in.rdbuf();
        const std::string content = text.str();
        CHECK(content.rfind("MiniDAW-Projekt 1", 0) == 0);
        CHECK(content.find(" Audio/drin.wav\n") != std::string::npos);
        CHECK(content.find("tempo 97") != std::string::npos);
        CHECK(content.find("pluginzustand 01020300ff") != std::string::npos);
    }

    // Laden in ein frisches Programm: Alles ist wieder da.
    {
        Rig rig;
        ProjectController controller(*rig.engine);
        controller.setSampleRate(kRate);
        FakePersistence plugins;
        controller.setPluginPersistence(&plugins);
        controller.addClip(9, outside, 0, message);  // wird vom Laden ersetzt

        CHECK(controller.loadProject(projectPath, message));
        CHECK(message.empty());
        CHECK(!controller.modified());
        const Project& p = controller.project();
        CHECK(p.bpm == 97.0);
        CHECK(std::fabs(p.masterGain - 0.8f) < 1e-6f);
        CHECK(p.tracks[9].clips.empty());
        CHECK(p.tracks[0].clips.size() == 1 && p.tracks[0].clips[0].start == 1000 &&
              p.tracks[0].clips[0].length == kSourceFrames);
        CHECK(p.tracks[2].clips.size() == 1 && p.tracks[2].clips[0].start == 48000);
        CHECK(p.tracks[2].clips.size() == 1 && p.tracks[2].clips[0].file->path == inside);
        CHECK(std::fabs(p.tracks[0].gain - 0.5f) < 1e-6f);
        CHECK(std::fabs(p.tracks[2].pan + 0.25f) < 1e-6f);
        CHECK(p.tracks[5].mute && p.tracks[2].solo && !p.tracks[0].mute);
        CHECK(p.tracks[5].input == 3 && p.tracks[5].midiOutput == 9);
        CHECK(p.tracks[6].input == kTrackInputMidiAll);
        CHECK(p.tracks[5].midiClips.size() == 1);
        if (p.tracks[5].midiClips.size() == 1) {
            const MidiClipModel& m = p.tracks[5].midiClips[0];
            CHECK(m.name == L"Bass \u00FC" && m.start == 96000 && m.length == 48000);
            CHECK(m.notes.size() == 2 && m.notes[1].start == 24000 && m.notes[1].pitch == 43 && m.notes[1].velocity == 64);
        }
        CHECK(plugins.restoreCalls == 1);
        CHECK(plugins.slots[5].file == std::filesystem::path("C:/Plugins/Piano.vst3"));
        CHECK(plugins.slots[5].state.size() == 5 && plugins.slots[5].state[4] == static_cast<char>(0xFF));

        // Das geladene Projekt klingt auch: Der Clip auf Spur 3 (solo) wird abgespielt.
        rig.service();
        rig.render();
        rig.engine->locate(50000);
        rig.render();
        rig.service();
        rig.engine->play();
        rig.render();
        rig.service();
        CHECK(rig.render().first > 0.01f);

        // Kaputte Datei: Laden scheitert, das Projekt bleibt unveraendert.
        const auto broken = folder / "kaputt.mdaw";
        { std::ofstream out(broken); out << "irgendwas\n"; }
        CHECK(!controller.loadProject(broken, message));
        CHECK(!message.empty());
        CHECK(controller.project().tracks[0].clips.size() == 1);
        CHECK(controller.projectPath() == projectPath);

        // Neues Projekt: alles leer.
        controller.newProject();
        CHECK(controller.project().tracks[0].clips.empty() && controller.project().tracks[5].midiClips.empty());
        CHECK(controller.project().bpm == 120.0 && controller.projectPath().empty() && !controller.modified());
        CHECK(plugins.slots[5].file.empty());
        while (!controller.peaksIdle()) std::this_thread::yield();
    }

    // Fehlt eine Audiodatei oder ein Plugin, wird der Rest trotzdem geladen und der Mangel gemeldet.
    std::filesystem::remove(inside, ec);
    {
        Rig rig;
        ProjectController controller(*rig.engine);
        controller.setSampleRate(kRate);
        FakePersistence plugins;
        plugins.failRestore = true;
        controller.setPluginPersistence(&plugins);
        CHECK(controller.loadProject(projectPath, message));
        CHECK(message.find("1 Audiodatei") != std::string::npos);
        CHECK(message.find("1 Plugin") != std::string::npos);
        CHECK(controller.project().tracks[0].clips.size() == 1);
        CHECK(controller.project().tracks[2].clips.empty());
        while (!controller.peaksIdle()) std::this_thread::yield();
    }
    std::filesystem::remove_all(folder, ec);
}

void testOfflineRender() {
    auto source = makeSource();
    if (!source) return;
    const auto sourcePath = tempFile("minidaw_test_source.wav");
    const auto exportPath = tempFile("minidaw_test_export.wav");
    std::string message;

    Rig rig;
    ProjectController controller(*rig.engine);
    controller.setSampleRate(kRate);
    CHECK(controller.addClip(0, sourcePath, 0, message));  // beginnt genau am Anfang
    rig.engine->locate(12345);
    rig.render();

    constexpr std::int64_t kFrames = 20000;
    int progressCalls = 0;
    CHECK(renderOffline(*rig.engine, kFrames, kBlock, 48000, exportPath, WavFormat::Float32, message,
                        [&](double fraction) { ++progressCalls; return fraction >= 0.0 && fraction <= 1.0; }));
    CHECK(progressCalls > 0);
    CHECK(!rig.engine->isPlaying());
    CHECK(rig.engine->position() == 12345);  // Position wie vorher

    auto wav = WavFile::open(exportPath, message);
    CHECK(wav != nullptr);
    if (wav) {
        CHECK(wav->frames() == kFrames && wav->channels() == 2 && wav->sampleRate() == 48000);
        std::vector<float> data(static_cast<std::size_t>(2 * kFrames));
        wav->readStereo(0, static_cast<int>(kFrames), data.data());
        // Jedes Sample stimmt, auch das allererste: beim Export wird nichts eingeblendet.
        const float g = std::cos(0.7853981633974483f);
        bool ok = true;
        for (std::int64_t i = 0; i < kFrames; ++i) {
            if (std::fabs(data[static_cast<std::size_t>(2 * i)] - sourceL(i) * g) > 1e-6f ||
                std::fabs(data[static_cast<std::size_t>(2 * i + 1)] - sourceR(i) * g) > 1e-6f)
                ok = false;
        }
        CHECK(ok);
        CHECK(std::fabs(data[1]) > 0.1f);  // rechts beginnt die Quelle mit vollem Pegel
    }
    wav.reset();

    // 16 Bit geht auch, und Abbrechen entfernt die halbe Datei.
    CHECK(renderOffline(*rig.engine, kFrames, kBlock, 48000, exportPath, WavFormat::Pcm16, message));
    wav = WavFile::open(exportPath, message);
    CHECK(wav && wav->bitsPerSample() == 16 && wav->frames() == kFrames);
    wav.reset();
    CHECK(!renderOffline(*rig.engine, 200000, kBlock, 48000, exportPath, WavFormat::Pcm24, message,
                         [](double) { return false; }));
    CHECK(!std::filesystem::exists(exportPath));
    CHECK(!renderOffline(*rig.engine, 0, kBlock, 48000, exportPath, WavFormat::Pcm24, message));

    // Danach laeuft die normale Wiedergabe wieder mit Einblenden (kein Offline-Modus mehr).
    std::error_code ec;
    std::filesystem::remove(exportPath, ec);
    while (!controller.peaksIdle()) std::this_thread::yield();
}

// --- Latenzmessung -----------------------------------------------------------

// Simuliert ein Kabel vom Ausgang in den Eingang: Was die Engine ausgibt, kommt
// delay Frames spaeter am Eingang an, mit Faktor gain und etwas Rauschen.
// Mit delayAfter/afterPulse aendert sich die Verzoegerung mitten in der Messung.
struct Loopback {
    std::vector<float> history;  // alle bisherigen Ausgaben (links)
    int delay = 1234;
    float gain = -0.3f;          // auch umgedrehte Polaritaet muss erkannt werden
    float noise = 0.001f;
    bool rightOnly = false;      // Kabel steckt im rechten Eingang
    std::int64_t switchAt = -1;  // ab diesem Frame gilt delayAfter
    int delayAfter = 0;
    std::uint32_t seed = 1;

    float noiseSample() {
        seed = seed * 1664525u + 1013904223u;
        return noise * (static_cast<float>(seed >> 8) / 8388608.0f - 1.0f);
    }

    // Rechnet einen Block: Eingang aus der Vergangenheit, dann Ausgabe merken.
    void run(Rig& rig) {
        const auto start = static_cast<std::int64_t>(history.size());
        rig.inL.assign(kBlock, 0.0f);
        rig.inR.assign(kBlock, 0.0f);
        for (int i = 0; i < kBlock; ++i) {
            const std::int64_t f = start + i;
            const int d = (switchAt >= 0 && f >= switchAt) ? delayAfter : delay;
            const float s = (f - d >= 0 && f - d < start) ? gain * history[static_cast<std::size_t>(f - d)] : 0.0f;
            (rightOnly ? rig.inR : rig.inL)[static_cast<std::size_t>(i)] = s + noiseSample();
            (rightOnly ? rig.inL : rig.inR)[static_cast<std::size_t>(i)] = noiseSample();
        }
        rig.render();
        history.insert(history.end(), rig.l.begin(), rig.l.end());
    }
};

LatencyProbe::Result measure(Loopback loop, Rig& rig) {
    auto probe = std::make_unique<LatencyProbe>(kRate);
    CHECK(rig.engine->startLatencyProbe(probe.get()));
    for (int b = 0; b < 2000 && !probe->done(); ++b) loop.run(rig);
    CHECK(probe->done());
    return probe->analyze();
}

void testLatencyProbe() {
    {
        Rig rig;
        // Waehrend der Messung ist nur das Testsignal zu hoeren, nicht der Testton der Spur.
        rig.engine->setTrackTestTone(0, 440.0f);
        rig.engine->play();
        rig.render();
        auto probe = std::make_unique<LatencyProbe>(kRate);
        rig.engine->startLatencyProbe(probe.get());
        rig.render();
        CHECK(rig.l[0] == 0.0f && rig.r[100] == 0.0f);  // vor dem ersten Sweep: Stille
        while (!probe->done()) rig.render();
        rig.render();
        CHECK(std::fabs(rig.render().first) > 0.05f);  // danach wieder der Testton
    }

    // Verschiedene Verzoegerungen, auch kleiner als ein Block, umgedrehte Polaritaet, leise.
    for (const int delay : {300, 1234, 9000}) {
        Rig rig;
        Loopback loop;
        loop.delay = delay;
        const auto result = measure(loop, rig);
        CHECK(result.ok);
        CHECK(result.frames == delay);
        CHECK(result.validPulses == LatencyProbe::kPulses);
        if (!result.ok) std::printf("  Meldung: %s\n", result.message.c_str());
    }
    {
        Rig rig;
        Loopback loop;
        loop.delay = 2000;
        loop.gain = 0.02f;  // sehr leise, Rauschen bleibt
        loop.rightOnly = true;
        const auto result = measure(loop, rig);
        CHECK(result.ok && result.frames == 2000);
    }

    // Kein Kabel: verstaendliche Meldung statt einer Zahl.
    {
        Rig rig;
        Loopback loop;
        loop.gain = 0.0f;
        loop.noise = 0.0f;
        const auto result = measure(loop, rig);
        CHECK(!result.ok && !result.message.empty());
    }
    // Nur Rauschen: nichts sicher erkannt.
    {
        Rig rig;
        Loopback loop;
        loop.gain = 0.0f;
        loop.noise = 0.05f;
        const auto result = measure(loop, rig);
        CHECK(!result.ok && result.validPulses < 4);
    }
    // Verzoegerung springt mitten in der Messung: wird als unzuverlaessig gemeldet.
    {
        Rig rig;
        Loopback loop;
        loop.delay = 1000;
        loop.switchAt = static_cast<std::int64_t>(2.0 * kRate);
        loop.delayAfter = 1400;
        const auto result = measure(loop, rig);
        CHECK(!result.ok && result.spread >= 300);
    }

    // Leichte Streuung (wie bei zwei Geraeten oder durch die Luft): Ergebnis mit Hinweis.
    {
        Rig rig;
        Loopback loop;
        loop.delay = 3000;
        loop.switchAt = static_cast<std::int64_t>(2.0 * kRate);
        loop.delayAfter = 3044;  // knapp 1 ms Unterschied
        const auto result = measure(loop, rig);
        CHECK(result.ok && result.spread == 44 && !result.message.empty());
        CHECK(result.frames == 3000 || result.frames == 3044);
    }

    // Uebernahme in den Controller: Zusatz so, dass die angewandte Latenz der Messung entspricht.
    {
        Rig rig;
        ProjectController controller(*rig.engine);
        controller.setSampleRate(kRate);
        controller.setLatencyProvider([] { return std::int64_t{960}; });  // Schaetzung 20 ms
        controller.applyMeasuredLatency(1440.0);                           // gemessen 30 ms
        CHECK(controller.recordOffsetMs() == 10);
        CHECK(controller.appliedLatencyFrames() == 1440);
        controller.applyMeasuredLatency(480.0);
        CHECK(controller.recordOffsetMs() == -10);
    }
}

// --- Clips teilen, kuerzen, Blenden -------------------------------------------

void testClipEditing() {
    makeSource();
    const auto path = tempFile("minidaw_test_source.wav");
    std::string message;

    Rig rig;
    ProjectController controller(*rig.engine);
    controller.setSampleRate(kRate);
    CHECK(controller.addClip(0, path, 1000, message));
    const auto& clips = controller.project().tracks[0].clips;

    // Teilen: zwei Clips, die zusammen genau das Original ergeben.
    CHECK(!controller.splitClip(0, 0, 1000));  // am Rand: nichts zu teilen
    CHECK(!controller.splitClip(0, 0, 999999999));
    CHECK(controller.splitClip(0, 0, 5000));
    CHECK(clips.size() == 2);
    if (clips.size() == 2) {
        CHECK(clips[0].start == 1000 && clips[0].offset == 0 && clips[0].length == 4000);
        CHECK(clips[1].start == 5000 && clips[1].offset == 4000 && clips[1].length == kSourceFrames - 4000);
    }

    // Klang: Bis auf die kurzen Blenden um den Schnitt ist alles unveraendert,
    // und die Blenden laufen sauber auf null und wieder hoch.
    const Expected expected = [](std::int64_t p) -> std::int64_t {
        return (p >= 1000 && p < 1000 + kSourceFrames) ? p - 1000 : -1;
    };
    rig.service();
    rig.render();
    rig.engine->play();
    rig.render();
    std::vector<float> all;
    for (int b = 0; b < 30; ++b) {
        rig.service();
        rig.render();
        all.insert(all.end(), rig.l.begin(), rig.l.end());
    }
    const float g = std::cos(0.7853981633974483f);
    bool outsideExact = true, fadesBounded = true;
    for (std::size_t i = 0; i < all.size(); ++i) {
        const std::int64_t pos = static_cast<std::int64_t>(i) + kBlock;
        const std::int64_t src = expected(pos);
        const float want = src >= 0 ? sourceL(src) * g : 0.0f;
        const bool nearCut = std::llabs(pos - 5000) <= ProjectController::kCutFade;
        if (!nearCut && std::fabs(all[i] - want) > 1e-5f) outsideExact = false;
        if (nearCut && std::fabs(all[i]) > std::fabs(want) + 1e-5f) fadesBounded = false;
    }
    CHECK(outsideExact);
    CHECK(fadesBounded);
    CHECK(std::fabs(all[static_cast<std::size_t>(5000 - kBlock)]) < 1e-6f);  // genau am Schnitt: Stille

    // Kuerzen: Das Material bleibt an seinem Platz, nur der Ausschnitt aendert sich.
    CHECK(controller.trimClip(0, 1, 6000, 20000));
    CHECK(clips[1].start == 6000 && clips[1].offset == 5000 && clips[1].length == 14000);
    CHECK(controller.trimClip(0, 1, 0, 999999999));  // ueber die Datei hinaus: begrenzt
    CHECK(clips[1].start == 1000 && clips[1].offset == 0 && clips[1].length == kSourceFrames);
    CHECK(!controller.trimClip(0, 1, 7000, 7000));

    // MIDI teilen: Noten nach dem Schnitt wandern, eine ueberlappende endet am Schnitt.
    MidiClipModel midi;
    midi.start = 0;
    midi.length = 10000;
    midi.notes = {{0, 3000, 60, 100}, {2000, 5000, 62, 100}, {6000, 1000, 64, 100}};
    CHECK(controller.addMidiClip(1, midi) == 0);
    CHECK(controller.splitMidiClip(1, 0, 4000));
    const auto& mc = controller.project().tracks[1].midiClips;
    CHECK(mc.size() == 2);
    if (mc.size() == 2) {
        CHECK(mc[0].length == 4000 && mc[0].notes.size() == 2 && mc[0].notes[1].length == 2000);
        CHECK(mc[1].start == 4000 && mc[1].length == 6000 && mc[1].notes.size() == 1 && mc[1].notes[0].start == 2000);
    }
    CHECK(controller.resizeMidiClip(1, 1, 500));
    CHECK(mc[1].length == 500);
    while (!controller.peaksIdle()) std::this_thread::yield();
}

}  // namespace

int main() {
    testStoppedIsSilent();
    testPlayAdvancesPosition();
    testStopFadesAndHolds();
    testLocate();
    testPan();
    testMuteAndSolo();
    testNoClicks();
    testLargeBlock();
    testInvalidTrack();
    testQueueTwoThreads();
    testWavRoundTrip();
    testClipPlayback();
    testLocateWhilePlaying();
    testUnderrunRecovery();
    testPlaylistChangeWhilePlaying();
    testThreadedSeeking();
    testPeakData();
    testProjectController();
    testRecording();
    testInputMonitor();
    testControllerRecording();
    testSynth();
    testLiveMidi();
    testMidiChannels();
    testMidiClipPlayback();
    testMidiRecording();
    testMetronome();
    testPlugins();
    testControllerPlugins();
    testProjectFile();
    testOfflineRender();
    testLatencyProbe();
    testClipEditing();
    {
        std::error_code ec;
        std::filesystem::remove_all(tempFile("minidaw_test_aufnahmen"), ec);
    }
    {
        std::error_code ec;
        std::filesystem::remove(tempFile("minidaw_test_source.wav"), ec);
    }

    if (failures == 0) {
        std::printf("Alle Tests bestanden.\n");
        return 0;
    }
    std::printf("%d Pruefung(en) fehlgeschlagen.\n", failures);
    return 1;
}
