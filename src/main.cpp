// Schritt 1: Sinus-Test. Gibt 440 Hz auf dem Standard-Wiedergabegeraet aus.

#include "audio/AudioCallback.h"
#include "audio/WasapiDevice.h"

#include <objbase.h>

#include <cmath>
#include <iostream>
#include <string>

namespace {

class SineTest final : public AudioCallback {
public:
    void prepare(double sampleRate, int, int) override {
        phase_ = 0.0;
        phaseInc_ = kTwoPi * 440.0 / sampleRate;
    }

    void process(float* const* out, int numChannels, int numFrames) noexcept override {
        for (int i = 0; i < numFrames; ++i) {
            const float s = 0.2f * static_cast<float>(std::sin(phase_));
            phase_ += phaseInc_;
            if (phase_ >= kTwoPi) phase_ -= kTwoPi;
            for (int c = 0; c < numChannels; ++c) out[c][i] = s;
        }
    }

private:
    static constexpr double kTwoPi = 6.283185307179586;
    double phase_ = 0.0;
    double phaseInc_ = 0.0;
};

}  // namespace

int main() {
    if (FAILED(CoInitializeEx(nullptr, COINIT_MULTITHREADED))) {
        std::cerr << "COM nicht initialisierbar\n";
        return 1;
    }

    int result = 0;
    {
        WasapiDevice device;
        SineTest sine;
        std::string error;

        if (!device.open(error) || !device.start(sine, error)) {
            std::cerr << "Fehler: " << error << "\n";
            result = 1;
        } else {
            const double periodMs = 1000.0 * device.periodFrames() / device.sampleRate();
            std::cout << "Samplerate:  " << device.sampleRate() << " Hz\n"
                      << "Kanaele:     " << device.numChannels() << "\n"
                      << "Periode:     " << device.periodFrames() << " Frames (" << periodMs << " ms)\n"
                      << "Geraetepuffer: " << device.bufferFrames() << " Frames\n"
                      << "440 Hz laeuft. Enter beendet.\n";
            std::string line;
            std::getline(std::cin, line);
            device.stop();
            if (device.failed()) {
                std::cerr << "Der Audio-Thread wurde durch einen Geraetefehler beendet.\n";
                result = 1;
            }
        }
    }

    CoUninitialize();
    return result;
}
