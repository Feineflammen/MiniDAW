// Offline-Tests der Engine. Laufen ohne Audiogeraet, also auch auf GitHub.

#include "engine/Engine.h"
#include "util/SpscQueue.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
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

    explicit Rig(int maxFrames = kBlock) { engine->prepare(kRate, 2, maxFrames); }

    // Rendert einen Block und liefert {Spitze links, Spitze rechts}.
    std::pair<float, float> render(int frames = kBlock) {
        l.assign(static_cast<std::size_t>(frames), 0.0f);
        r.assign(static_cast<std::size_t>(frames), 0.0f);
        float* out[2] = {l.data(), r.data()};
        engine->process(out, 2, frames);

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

    if (failures == 0) {
        std::printf("Alle Tests bestanden.\n");
        return 0;
    }
    std::printf("%d Pruefung(en) fehlgeschlagen.\n", failures);
    return 1;
}
