#pragma once

#include <memory>
#include <string>

class AudioCallback;

// Ausgabe über das Standard-Wiedergabegerät (WASAPI, Shared Mode, ereignisgesteuert).
// Wenn der Treiber es erlaubt, wird die kleinste Periode des Geräts benutzt.
//
// COM muss im aufrufenden Thread initialisiert sein (CoInitializeEx).
class WasapiDevice {
public:
    WasapiDevice();
    ~WasapiDevice();
    WasapiDevice(const WasapiDevice&) = delete;
    WasapiDevice& operator=(const WasapiDevice&) = delete;

    bool open(std::string& error);
    bool start(AudioCallback& callback, std::string& error);
    void stop();
    void close();

    double sampleRate() const;
    int numChannels() const;
    int bufferFrames() const;  // Größe des Gerätepuffers = größte Blocklänge
    int periodFrames() const;  // übliche Blocklänge pro Callback
    bool failed() const;       // true, wenn der Audio-Thread wegen eines Fehlers endete

private:
    struct Impl;
    std::unique_ptr<Impl> d;
};
