// Offline-Tests der Engine. Laufen ohne Audiogeraet, also auch auf GitHub.

#include "disk/DiskThread.h"
#include "disk/WavFile.h"
#include "engine/Engine.h"
#include "model/ProjectController.h"
#include "util/SpscQueue.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <filesystem>
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
    rig.engine->setArmedTrack(3);
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
