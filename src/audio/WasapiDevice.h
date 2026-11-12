#pragma once

#include <memory>
#include <string>

class AudioCallback;

// Standard-Wiedergabegeraet und, falls vorhanden, Standard-Aufnahmegeraet
// ueber WASAPI (Shared Mode, ereignisgesteuert).
//
// Die Ausgabe gibt den Takt vor. Der Eingang laeuft in einem eigenen Thread
// und wird ueber einen Ringpuffer in den Callback gereicht. Windows rechnet
// den Eingang dabei auf die Samplerate der Ausgabe um.
//
// COM muss im aufrufenden Thread initialisiert sein (CoInitializeEx).
class WasapiDevice {
public:
    WasapiDevice();
    ~WasapiDevice();
    WasapiDevice(const WasapiDevice&) = delete;
    WasapiDevice& operator=(const WasapiDevice&) = delete;

    // Oeffnet die Ausgabe (Pflicht) und den Eingang (optional).
    bool open(std::string& error);
    bool start(AudioCallback& callback, std::string& error);
    void stop();
    void close();

    double sampleRate() const;
    int numChannels() const;   // Ausgangskanaele
    int bufferFrames() const;  // Groesse des Geraetepuffers = groesste Blocklaenge
    int periodFrames() const;  // uebliche Blocklaenge pro Callback
    bool failed() const;       // true, wenn der Audio-Thread wegen eines Fehlers endete

    int inputChannels() const;              // 0 = kein Eingang
    const std::string& inputError() const;  // Grund, falls der Eingang fehlt

    // Geschaetzte Latenzen in Frames. Die Eingangslatenz enthaelt den Ringpuffer
    // zwischen Aufnahme- und Wiedergabe-Thread und pendelt sich nach dem Start ein.
    int outputLatencyFrames() const;
    int inputLatencyFrames() const;

    // Anzahl der Stoerungen im Eingang (Ringpuffer leer oder uebergelaufen).
    unsigned inputGlitches() const;

private:
    struct Impl;
    std::unique_ptr<Impl> d;
};
