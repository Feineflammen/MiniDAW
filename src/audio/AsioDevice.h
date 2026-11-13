#pragma once

#include "audio/AudioDevice.h"

#include <memory>
#include <string>
#include <vector>

// Audiogeraet ueber einen ASIO-Treiber: Ein- und Ausgang laufen im selben
// Takt, ohne Windows-Mischer dazwischen. Die Latenz ist dadurch klein und
// fest, und alle Eingaenge der Soundkarte sind auf einmal da.
//
// Die Treiberschnittstelle ist hier selbst beschrieben (kein ASIO-SDK).
// Treiber stehen in der Registry unter HKLM\SOFTWARE\ASIO.
//
// Oeffnen, Starten und Schliessen nur aus dem UI-Thread (dort ist COM
// initialisiert). Es kann immer nur ein ASIO-Treiber offen sein.
class AsioDevice final : public AudioDevice {
public:
    AsioDevice();
    ~AsioDevice() override;
    AsioDevice(const AsioDevice&) = delete;
    AsioDevice& operator=(const AsioDevice&) = delete;

    // Namen aller installierten ASIO-Treiber.
    static std::vector<std::wstring> drivers();

    // Oeffnet den Treiber mit diesem Namen. window ist das Hauptfenster (HWND),
    // manche Treiber brauchen es. Die Samplerate bleibt, wie sie im Treiber
    // eingestellt ist; die Puffergroesse ist die vom Treiber bevorzugte.
    bool open(const std::wstring& driverName, void* window, std::string& error);

    bool start(AudioCallback& callback, std::string& error) override;
    void stop() override;
    void close() override;

    double sampleRate() const override;
    int numChannels() const override;
    int bufferFrames() const override;
    int periodFrames() const override { return bufferFrames(); }
    bool failed() const override { return false; }

    int inputChannels() const override;
    const std::string& inputError() const override;
    int outputLatencyFrames() const override;
    int inputLatencyFrames() const override;

    const std::wstring& outputName() const override;  // Name des Treibers
    const std::wstring& inputName() const override;   // ebenfalls der Treiber
    std::wstring settingsKey() const override;

    bool resetRequested() const override;
    bool hasControlPanel() const override { return true; }
    void showControlPanel() override;

    // Ueberlastungen, die der Treiber gemeldet hat.
    unsigned overloads() const;

private:
    struct Impl;
    std::unique_ptr<Impl> d;
};
