#pragma once

#include <memory>
#include <string>
#include <vector>

class AudioCallback;

// Ein Audiogeraet, wie Windows es kennt: eindeutige Kennung und Anzeigename.
struct AudioEndpoint {
    std::wstring id;
    std::wstring name;
};

// Wiedergabe- und Aufnahmegeraet ueber WASAPI (Shared Mode, ereignisgesteuert).
// Ohne Angabe werden die Standardgeraete von Windows benutzt.
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

    // Alle angeschlossenen Geraete (capture = Aufnahme, sonst Wiedergabe).
    // COM muss im aufrufenden Thread initialisiert sein.
    static std::vector<AudioEndpoint> endpoints(bool capture);

    // Oeffnet die Ausgabe (Pflicht) und den Eingang (optional). Leere Kennung =
    // Standardgeraet von Windows. Mit noInput wird kein Eingang geoeffnet.
    // Laeuft das Geraet, wird es vorher angehalten und geschlossen.
    bool open(std::string& error) { return open(L"", L"", false, error); }
    bool open(const std::wstring& outputId, const std::wstring& inputId, bool noInput, std::string& error);

    // Kennung und Name der tatsaechlich geoeffneten Geraete (leer = keins).
    const std::wstring& outputId() const;
    const std::wstring& outputName() const;
    const std::wstring& inputId() const;
    const std::wstring& inputName() const;
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
