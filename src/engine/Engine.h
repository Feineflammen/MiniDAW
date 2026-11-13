#pragma once

#include "audio/AudioCallback.h"
#include "disk/RecordSink.h"
#include "disk/TrackStream.h"
#include "engine/AudioPlugin.h"
#include "engine/Automation.h"
#include "engine/DelayLine.h"
#include "engine/LatencyProbe.h"
#include "engine/Synth.h"
#include "midi/MidiMessage.h"
#include "util/SpscQueue.h"

#include <array>
#include <atomic>
#include <cstdint>
#include <memory>
#include <mutex>
#include <vector>

// Fertig aufbereitete MIDI-Daten einer Spur: alle Ereignisse ihrer MIDI-Clips,
// nach Zeit sortiert. Wird vom UI-Thread gebaut und danach nicht mehr geaendert.
struct MidiSequence {
    struct Event {
        std::int64_t frame = 0;  // Position auf der Zeitachse
        MidiMessage message;
    };
    std::vector<Event> events;
};

// Eine MIDI-Nachricht fuer ein externes Geraet, mit dem Zeitpunkt, zu dem sie
// gesendet werden soll (Mikrosekunden der std::chrono::steady_clock).
struct MidiOutEvent {
    MidiMessage message;
    std::int64_t dueMicros = 0;
};

// Was der Audio-Thread waehrend einer MIDI-Aufnahme an die UI meldet.
struct MidiRecordEvent {
    enum class Kind : std::uint8_t { Message, Begin, End };
    Kind kind = Kind::Message;
    MidiMessage message;
    std::int64_t position = 0;  // Zeitachse, bereits um die Latenz korrigiert
    int track = -1;             // Spur der Nachricht; bei Begin und End -1
};

// Die Engine rechnet das Audio. Sie kennt kein Audiogeraet: Sie bekommt nur
// "rechne N Samples" und laesst sich deshalb auch offline betreiben.
//
// Die Seiten:
//  - UI-Thread: die Methoden im ersten Block. Sie schicken Kommandos ueber eine
//    lock-freie Queue und lesen Zustand ueber Atomics. Genau EIN Thread darf sie rufen.
//  - Disk-Thread: serviceDisk() liest die Audiodateien der Clips vor und
//    schreibt laufende Aufnahmen.
//  - MIDI-Ausgabe-Thread: popMidiOut().
//  - Audio-Thread: prepare() und process().
class Engine final : public AudioCallback {
public:
    static constexpr int kNumTracks = 16;
    static constexpr int kNumBuses = 4;
    static constexpr int kNumInserts = 4;
    // Kanalzuege: Spuren 0..15, Busse 16..19, Master 20.
    static constexpr int kFirstBus = kNumTracks;
    static constexpr int kMasterStrip = kNumTracks + kNumBuses;
    static constexpr int kNumStrips = kMasterStrip + 1;
    // Plugin-Plaetze je Kanalzug: 0 = Instrument (nur Spuren), 1..4 = Effekte.
    static constexpr int kInstrumentSlot = 0;
    static constexpr int kNumSlots = 1 + kNumInserts;
    static constexpr float kTestToneLevel = 0.15f;
    // Hoechstzahl der Eingangskanaele, die die Engine auswerten kann.
    static constexpr int kMaxInputs = 64;

    // Werte fuer den MIDI-Eingang einer Spur; 0..15 waehlt einen einzelnen Kanal.
    static constexpr int kInputAudio = -2;    // Spur nimmt Audio auf und ignoriert MIDI
    static constexpr int kInputMidiAll = -1;  // MIDI von allen Kanaelen
    // Wert fuer den MIDI-Ausgang einer Spur; 0..15 sendet auf diesem Kanal an das MIDI-Geraet.
    static constexpr int kOutputSynth = -1;

    Engine();
    ~Engine() override;

    // --- UI-Thread -------------------------------------------------------
    // Alle Setter liefern false, wenn der Index ungueltig oder die Queue voll ist.
    bool play() noexcept;
    bool stop() noexcept;  // beendet auch eine laufende Aufnahme
    bool locate(std::int64_t samplePosition) noexcept;

    bool setTrackGain(int track, float gain) noexcept;  // linear, 1.0 = 0 dB
    bool setTrackPan(int track, float pan) noexcept;    // -1 (links) .. +1 (rechts)
    bool setTrackMute(int track, bool mute) noexcept;
    bool setTrackSolo(int track, bool solo) noexcept;
    bool setMasterGain(float gain) noexcept;

    // Sends (nach Fader und Panorama) einer Spur an die Busse, linear, 0 = aus.
    bool setTrackSend(int track, int bus, float level) noexcept;
    // Ausgang einer Spur: -1 = Master, 0..3 = Bus.
    bool setTrackOutput(int track, int bus) noexcept;
    bool setBusGain(int bus, float gain) noexcept;
    bool setBusPan(int bus, float pan) noexcept;
    bool setBusMute(int bus, bool mute) noexcept;

    // Gesamtlatenz der Plugins in Frames. Um so viel liest die Engine die
    // Zeitachse voraus, damit das Gehoerte trotzdem zur Position passt.
    int pluginLatency() const noexcept { return latencyOut_.load(std::memory_order_relaxed); }

    // Tempo in Schlaegen pro Minute (nur fuer das Metronom) und Metronom an/aus.
    // Das Metronom klickt waehrend der Wiedergabe auf jedem Schlag, betont auf der Eins (4/4).
    bool setTempo(double bpm) noexcept;
    bool setMetronome(bool on) noexcept;

    // Latenzmessung: Solange die Sonde laeuft, gibt die Engine nur ihr
    // Testsignal aus. Der Aufrufer haelt probe am Leben, bis probe->done() gilt.
    bool startLatencyProbe(LatencyProbe* probe) noexcept;

    // Offline-Rechnen (Export): Start und Stopp werden nicht ein- und
    // ausgeblendet, damit der Anfang der Datei unveraendert bleibt.
    bool setOffline(bool on) noexcept;

    // Testton als Quelle einer Spur (fuer Tests). 0 Hz schaltet ihn aus.
    bool setTrackTestTone(int track, float hz) noexcept;

    // Ersetzt die Audio-Clips einer Spur. Wirkt, sobald der Disk-Thread sie uebernimmt.
    bool setTrackPlaylist(int track, Playlist playlist);

    // MIDI-Eingang (kInputAudio, kInputMidiAll oder Kanal 0..15) und
    // MIDI-Ausgang (kOutputSynth oder Kanal 0..15) einer Spur.
    bool setTrackMidiInput(int track, int input) noexcept;
    bool setTrackMidiOutput(int track, int output) noexcept;

    // Ersetzt die MIDI-Daten einer Spur (nullptr = keine). Die Engine uebernimmt
    // den Besitz; die alte Fassung kommt ueber collectGarbage() zurueck.
    bool setTrackMidiSequence(int track, std::unique_ptr<MidiSequence> sequence);
    // Gibt Objekte frei, die der Audio-Thread nicht mehr braucht. Regelmaessig rufen.
    void collectGarbage();

    // Automation einer Spur: kAutomationVolume (Fader-Stellung 0..1) oder
    // kAutomationPan (-1..1). Mit einer Kurve gilt sie statt des festen Werts,
    // nullptr schaltet sie ab. Die alte Kurve kommt ueber collectGarbage() zurueck.
    static constexpr int kAutomationVolume = 0;
    static constexpr int kAutomationPan = 1;
    static constexpr int kAutomationParams = 2;
    bool setTrackAutomation(int track, int param, std::unique_ptr<AutomationCurve> curve);
    // Aktueller Wert der Automation (fuer die Anzeige); NaN, wenn keine aktiv ist.
    float automationValue(int track, int param) const noexcept;

    // Setzt ein Plugin auf einen Platz eines Kanalzugs (nullptr = leer).
    // Platz 0 einer Spur ist das Instrument: Es bekommt das MIDI der Spur
    // anstelle des eingebauten Synthesizers. Die Plaetze 1..4 sind Effekte und
    // werden in dieser Reihenfolge vor dem Fader gerechnet. Das bisherige Plugin
    // erscheint in popReleasedPlugin(), sobald der Audio-Thread es losgelassen hat.
    bool setPlugin(int strip, int slot, AudioPlugin* plugin) noexcept;
    // Kurzform fuer genau ein Plugin je Spur: ein Instrument auf Platz 0,
    // einen Effekt auf Platz 1; der jeweils andere Platz wird geleert.
    bool setTrackPlugin(int track, AudioPlugin* plugin) noexcept;
    bool popReleasedPlugin(AudioPlugin*& plugin) noexcept { return releasedPlugins_.pop(plugin); }

    // Live-MIDI (Keyboard, Rechnertastatur) fuer die scharfgeschaltete Spur.
    // Darf als einzige Methode aus mehreren Threads gerufen werden, zum
    // Beispiel aus dem Rueckruf des MIDI-Treibers; nie aus dem Audio-Thread.
    // fromKeyboard: Nachricht der Rechnertastatur, sie umgeht den Kanalfilter.
    bool postMidi(MidiMessage message, bool fromKeyboard = false);
    // Meldet eine fehlerhafte Nachricht vom MIDI-Treiber (nur fuer die Anzeige).
    void reportMidiError() noexcept { midiErrors_.fetch_add(1, std::memory_order_relaxed); }

    // Fuer die MIDI-Anzeige: Anzahl aller bisher eingegangenen Nachrichten,
    // die zuletzt eingegangene und die Anzahl der Fehler.
    std::uint32_t midiMessageCount() const noexcept { return midiCount_.load(std::memory_order_relaxed); }
    std::uint32_t midiErrorCount() const noexcept { return midiErrors_.load(std::memory_order_relaxed); }
    MidiMessage lastMidiMessage() const noexcept {
        const std::uint32_t packed = lastMidi_.load(std::memory_order_relaxed);
        return {static_cast<std::uint8_t>(packed & 0xFF), static_cast<std::uint8_t>((packed >> 8) & 0xFF),
                static_cast<std::uint8_t>((packed >> 16) & 0xFF)};
    }

    // Scharfgeschaltete Spuren bekommen den Eingang (Pegelanzeige, Mithoeren,
    // Live-MIDI, Aufnahme). Es koennen beliebig viele Spuren scharf sein.
    bool setTrackArmed(int track, bool armed) noexcept;
    // Schaltet genau diese Spur scharf und alle anderen ab; -1 = keine.
    bool setArmedTrack(int track) noexcept;
    // Audio-Eingang einer Spur: erster Kanal des Geraets (0-basiert), mono
    // oder als Stereopaar mit dem folgenden Kanal.
    bool setTrackAudioInput(int track, int channel, bool stereo) noexcept;
    // Eingang ueber die scharfgeschalteten Spuren mithoeren.
    bool setInputMonitor(bool on) noexcept;

    // Startet die Wiedergabe und nimmt auf, bis stop() kommt: je Eintrag den
    // Audio-Eingang einer Spur in ihre Datei, mit midi auch das Live-MIDI der
    // scharfgeschalteten MIDI-Spuren (abzuholen mit popMidiRecord()).
    // Der Aufrufer haelt die Dateien am Leben, bis done() gilt, und ruft dann
    // releaseRecordSinks().
    struct TrackSink {
        int track = 0;
        std::shared_ptr<RecordSink> sink;
    };
    bool record(const std::vector<TrackSink>& sinks, bool midi);
    void releaseRecordSinks();
    // Nur MIDI aufnehmen.
    bool recordMidi() { return record({}, true); }
    bool popMidiRecord(MidiRecordEvent& event) noexcept { return midiRecordOut_.pop(event); }

    // Ausgangslatenz des Audiogeraets in Frames. MIDI an externe Geraete wird
    // um diese Zeit verzoegert gesendet, damit es zum hoerbaren Audio passt;
    // aufgenommenes MIDI wird um sie nach vorn geschoben.
    void setOutputLatencyFrames(int frames) noexcept { outputLatency_.store(frames, std::memory_order_relaxed); }

    // Anzahl der Bloecke, in denen einer Spur Daten von der Platte fehlten.
    std::uint32_t underruns() const noexcept { return underruns_.load(std::memory_order_relaxed); }

    std::int64_t position() const noexcept { return positionOut_.load(std::memory_order_relaxed); }
    bool isPlaying() const noexcept { return playingOut_.load(std::memory_order_relaxed); }
    bool isRecording() const noexcept { return recordingOut_.load(std::memory_order_relaxed); }

    // Liefert den hoechsten Pegel seit dem letzten Aufruf und setzt ihn zurueck.
    float takeTrackPeak(int track) noexcept;
    float takeMasterPeak() noexcept;
    float takeBusPeak(int bus) noexcept;
    float takeInputPeak(int track) noexcept;  // Eingang einer scharfgeschalteten Spur

    // --- Disk-Thread -----------------------------------------------------
    // Liest Audiodaten fuer alle Spuren vor und schreibt Aufnahmedaten.
    // true, wenn etwas zu tun war. Wird von DiskThread in einer Schleife
    // gerufen, in Tests direkt.
    bool serviceDisk();

    // --- MIDI-Ausgabe-Thread ---------------------------------------------
    bool popMidiOut(MidiOutEvent& event) noexcept { return midiOut_.pop(event); }

    // --- Audio-Thread ----------------------------------------------------
    void prepare(double sampleRate, int numInputs, int numOutputs, int maxFrames) override;
    void process(const float* const* in, int numInputs, float* const* out, int numOutputs,
                 int numFrames) noexcept override;

private:
    enum class CommandType : std::uint8_t {
        Play, Stop, Locate,
        TrackGain, TrackPan, TrackMute, TrackSolo, TrackTestTone,
        MasterGain,
        ArmTrack, TrackAudioInput, TrackRecordSink, InputMonitor, Record,
        TrackMidiInput, TrackMidiOutput, TrackMidiSequence, TrackAutomation,
        Tempo, Metronome, TrackPlugin, Offline, Probe,
        TrackSend, TrackOutput, BusGain, BusPan, BusMute,
    };

    struct Command {
        CommandType type{};
        std::int32_t track = 0;
        float value = 0.0f;
        std::int64_t position = 0;
        RecordSink* sink = nullptr;
        const MidiSequence* sequence = nullptr;
        const AutomationCurve* curve = nullptr;
        AudioPlugin* plugin = nullptr;
        LatencyProbe* probe = nullptr;
    };

    struct Track {
        // Mixerwerte
        float gain = 1.0f;
        float pan = 0.0f;
        bool mute = false;
        bool solo = false;
        // Aktuelle und angestrebte Verstaerkung je Seite (wird pro Block ueberblendet)
        float curL = 0.0f, curR = 0.0f;
        float tgtL = 0.0f, tgtR = 0.0f;
        // Testton
        float toneHz = 0.0f;
        double phase = 0.0;
        double phaseInc = 0.0;
        // MIDI
        int midiInput = kInputAudio;
        int midiOutput = kOutputSynth;
        const MidiSequence* sequence = nullptr;
        std::size_t seqIndex = 0;        // naechstes Ereignis der Sequenz
        std::int64_t seqNextPos = -1;    // Position, an der die Wiedergabe fortsetzt
        bool seqDirty = false;           // Sequenz wurde ersetzt
        std::array<std::uint64_t, 2> held{};  // klingende Noten aus der Sequenz
        // Mixer
        std::array<AudioPlugin*, kNumSlots> slots{};
        std::array<float, kNumBuses> send{};     // angestrebter Send-Pegel
        std::array<float, kNumBuses> sendCur{};  // aktueller (ueberblendet)
        int output = -1;                         // -1 = Master, sonst Bus
        DelayLine delay;                         // Latenzausgleich
        // Automation (nullptr = aus) und der daraus gerade gueltige Wert
        std::array<const AutomationCurve*, kAutomationParams> automation{};
        float autoGain = 1.0f;
        float autoPan = 0.0f;
        // Eingang und Aufnahme
        bool armed = false;
        int inChannel = 0;
        bool inStereo = true;
        RecordSink* sink = nullptr;
    };

    struct Bus {
        float gain = 1.0f;
        float pan = 0.0f;
        bool mute = false;
        float curL = 0.0f, curR = 0.0f;
        float tgtL = 0.0f, tgtR = 0.0f;
        std::array<AudioPlugin*, kNumSlots> slots{};  // Platz 0 bleibt leer
        DelayLine delay;
        std::vector<float> left, right;  // Eingang des Blocks
    };

    struct LiveMidi {
        MidiMessage message;
        bool keyboard = false;
        std::int64_t micros = 0;  // Zeitpunkt des Eingangs
    };

    using BlockEvent = PluginMidiEvent;

    bool send(const Command& c) noexcept { return commands_.push(c); }
    void drainCommands() noexcept;
    void updateTargets() noexcept;
    void applyAutomation() noexcept;
    void renderBlock(int numFrames, std::int64_t blockMicros) noexcept;
    int gatherMidi(std::size_t trackIndex, int numFrames, std::int64_t blockMicros) noexcept;
    void silenceTrack(std::size_t trackIndex, std::int64_t dueMicros) noexcept;
    // Eingangskanaele einer Spur in diesem Block; false, wenn sie keinen Audio-Eingang hat.
    bool trackInput(const Track& t, const float*& left, const float*& right) const noexcept;
    AudioPlugin** slotPointer(int strip, int slot) noexcept;
    void recomputeLatency() noexcept;
    void runInserts(std::array<AudioPlugin*, kNumSlots>& slots, float* left, float* right, int numFrames,
                    const PluginMidiEvent* events, int numEvents, const PluginTransport& transport) noexcept;

    SpscQueue<Command, 1024> commands_;
    SpscQueue<const MidiSequence*, 256> garbage_;  // Audio -> UI: nicht mehr benoetigt
    SpscQueue<const AutomationCurve*, 256> curveGarbage_;
    bool anyAutomation_ = false;
    int busLatency_ = 0;     // groesste Latenz der Busse
    int masterLatency_ = 0;  // Latenz der Master-Plaetze
    // Werte der Automation fuer die Anzeige, je Spur und Parameter (NaN = keine).
    std::array<std::atomic<float>, kNumTracks * kAutomationParams> automationOut_{};
    SpscQueue<AudioPlugin*, 256> releasedPlugins_;  // Audio -> UI: wird nicht mehr benutzt

    // Live-MIDI: mehrere Absender teilen sich die Schreibseite ueber einen Mutex,
    // der Audio-Thread liest ohne Sperre.
    SpscQueue<LiveMidi, 1024> midiIn_;
    std::mutex midiPostMutex_;
    std::atomic<std::uint32_t> midiCount_{0};
    std::atomic<std::uint32_t> midiErrors_{0};
    std::atomic<std::uint32_t> lastMidi_{0};

    SpscQueue<MidiOutEvent, 4096> midiOut_;              // Audio -> MIDI-Ausgabe-Thread
    SpscQueue<MidiRecordEvent, 4096> midiRecordOut_;     // Audio -> UI
    std::atomic<int> outputLatency_{0};

    // Zustand des Audio-Threads
    double sampleRate_ = 48000.0;
    int maxFrames_ = 0;
    std::array<Track, kNumTracks> tracks_{};
    std::array<Bus, kNumBuses> buses_{};
    std::array<AudioPlugin*, kNumSlots> masterSlots_{};
    DelayLine masterDirectDelay_;   // gleicht die Spuren, die direkt zum Master gehen, an die Busse an
    int latency_ = 0;               // Gesamtlatenz der Plugins
    bool anyPlugin_ = false;
    std::int64_t tailFrames_ = 0;   // so lange laufen Verzoegerungen noch nach
    int lastLiveCount_ = 0;         // so viele Ereignisse am Anfang von blockEvents_ sind live
    float masterGain_ = 1.0f;
    float masterCur_ = 1.0f;
    float transportCur_ = 0.0f;  // blendet die Wiedergabe bei Start und Stopp
    bool wantPlay_ = false;      // Wunsch des Benutzers
    bool rolling_ = false;       // Transport laeuft (auch noch waehrend des Ausblendens)
    std::int64_t position_ = 0;
    bool anyArmed_ = false;
    bool monitor_ = false;
    bool recording_ = false;  // Audio-Aufnahme laeuft
    bool midiRecording_ = false;
    double tempo_ = 120.0;
    bool metronome_ = false;
    bool offline_ = false;
    LatencyProbe* probe_ = nullptr;
    int clickRemaining_ = 0;   // noch zu spielende Frames des aktuellen Klicks
    int clickLength_ = 1;
    double clickPhase_ = 0.0;
    double clickPhaseInc_ = 0.0;
    std::array<const float*, kMaxInputs> in_{};  // Eingang des aktuellen Blocks
    int numIn_ = 0;
    std::vector<float> silence_;  // Ersatz fuer fehlende Eingangskanaele
    std::vector<float> masterL_, masterR_;
    std::vector<float> trackL_, trackR_;
    std::vector<float> pluginL_, pluginR_;
    std::array<TrackStream, kNumTracks> streams_;
    std::array<Synth, kNumTracks> synths_;
    std::array<LiveMidi, 128> liveMidi_{};  // Nachrichten fuer den naechsten Block
    int liveMidiCount_ = 0;
    std::array<BlockEvent, 256> blockEvents_{};

    // Rueckkanal zur UI
    std::atomic<std::int64_t> positionOut_{0};
    std::atomic<bool> playingOut_{false};
    std::atomic<bool> recordingOut_{false};
    std::array<std::atomic<float>, kNumTracks> trackPeak_{};
    std::atomic<float> masterPeak_{0.0f};
    std::array<std::atomic<float>, kNumBuses> busPeak_{};
    std::atomic<int> latencyOut_{0};
    std::array<std::atomic<float>, kNumTracks> inputPeak_{};
    std::atomic<std::uint32_t> underruns_{0};

    // Aufnahmeziele fuer den Disk-Thread, je Spur (UI und Disk duerfen warten)
    std::mutex sinkMutex_;
    std::array<std::shared_ptr<RecordSink>, kNumTracks> diskSinks_;
};
