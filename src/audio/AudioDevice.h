#pragma once

#include <string>

class AudioCallback;

// Gemeinsame Sicht auf ein geoeffnetes Audiogeraet, egal ob WASAPI oder ASIO.
// Oeffnen ist je nach Art verschieden und steht in den Klassen selbst.
// Alle Methoden werden aus dem UI-Thread gerufen.
class AudioDevice {
public:
    virtual ~AudioDevice() = default;

    virtual bool start(AudioCallback& callback, std::string& error) = 0;
    virtual void stop() = 0;
    virtual void close() = 0;

    virtual double sampleRate() const = 0;  // 0 = nicht geoeffnet
    virtual int numChannels() const = 0;    // Ausgangskanaele
    virtual int bufferFrames() const = 0;   // groesste Blocklaenge
    virtual int periodFrames() const = 0;   // uebliche Blocklaenge pro Callback
    virtual bool failed() const = 0;        // Audio-Thread wegen eines Fehlers beendet

    virtual int inputChannels() const = 0;              // 0 = kein Eingang
    virtual const std::string& inputError() const = 0;  // Grund, falls der Eingang fehlt

    // Geschaetzte Latenzen in Frames (vom Treiber gemeldet).
    virtual int outputLatencyFrames() const = 0;
    virtual int inputLatencyFrames() const = 0;

    // Anzeigenamen fuer Statusleiste und Menue.
    virtual const std::wstring& outputName() const = 0;
    virtual const std::wstring& inputName() const = 0;

    // Schluessel, unter dem Einstellungen zu diesem Geraet gespeichert werden
    // (zum Beispiel die gemessene Latenz).
    virtual std::wstring settingsKey() const = 0;

    // Der Treiber will neu geoeffnet werden (Puffergroesse oder Samplerate
    // im Treiberfenster geaendert). Die UI fragt das regelmaessig ab.
    virtual bool resetRequested() const { return false; }
    // Eigenes Einstellungsfenster des Treibers (nur ASIO).
    virtual bool hasControlPanel() const { return false; }
    virtual void showControlPanel() {}
};
