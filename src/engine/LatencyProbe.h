#pragma once

#include <atomic>
#include <cstdint>
#include <string>
#include <vector>

// Misst die tatsaechliche Latenz ueber eine Kabelverbindung vom Ausgang
// zurueck in einen Eingang ("Loopback").
//
// Die Sonde gibt mehrmals einen kurzen Sweep aus und zeichnet gleichzeitig
// den Eingang auf. Ausgabe und Aufnahme laufen ueber denselben Zaehler, so wie
// bei einer echten Aufnahme Wiedergabe und Eingang. Der Abstand, um den der
// Sweep spaeter im Eingang auftaucht, ist genau der Betrag, um den eine
// Aufnahme verschoben werden muss.
//
// Ablauf: Der UI-Thread legt die Sonde an und uebergibt sie der Engine. Der
// Audio-Thread ruft process(), bis done() gilt. Danach wertet der UI-Thread
// mit analyze() aus. Alle Puffer werden im Konstruktor angelegt.
class LatencyProbe {
public:
    static constexpr int kPulses = 6;

    struct Result {
        bool ok = false;
        double frames = 0.0;    // gemessene Latenz in Frames
        int validPulses = 0;    // so viele Sweeps wurden sicher erkannt
        int spread = 0;         // groesste Abweichung der Einzelmessungen in Frames
        float inputPeak = 0.0f; // hoechster Pegel am Eingang (1.0 = Vollaussteuerung)
        std::string message;    // Grund bei Fehlschlag oder Hinweis bei Erfolg (UTF-8)
    };

    explicit LatencyProbe(double sampleRate);

    double sampleRate() const noexcept { return sampleRate_; }
    std::int64_t totalFrames() const noexcept { return total_; }
    // Groesste messbare Latenz in Frames.
    int maxLatencyFrames() const noexcept { return maxLag_; }

    // --- Audio-Thread ---
    // Ueberschreibt die Ausgaenge mit dem Testsignal und zeichnet den Eingang auf.
    // inL/inR duerfen nullptr sein (kein Eingang), outR ebenfalls (Mono-Ausgang).
    void process(const float* inL, const float* inR, float* outL, float* outR, int numFrames) noexcept;
    bool done() const noexcept { return done_.load(std::memory_order_acquire); }

    // --- UI-Thread, nach done() ---
    Result analyze() const;

private:
    double sampleRate_;
    std::vector<float> sweep_;
    std::vector<float> captureL_, captureR_;
    std::int64_t lead_ = 0;     // Stille vor dem ersten Sweep
    std::int64_t spacing_ = 0;  // Abstand der Sweeps
    std::int64_t total_ = 0;
    int maxLag_ = 0;
    std::int64_t frame_ = 0;    // nur Audio-Thread
    std::atomic<bool> done_{false};
};
