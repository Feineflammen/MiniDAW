#include "engine/Engine.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstring>
#include <utility>

namespace {

constexpr double kTwoPi = 6.283185307179586;
constexpr float kQuarterPi = 0.7853981633974483f;

bool validTrack(int track) noexcept { return track >= 0 && track < Engine::kNumTracks; }

void raisePeak(std::atomic<float>& peak, float value) noexcept {
    if (value > peak.load(std::memory_order_relaxed))
        peak.store(value, std::memory_order_relaxed);
}

// Zeit einer gleichmaessig laufenden Uhr in Mikrosekunden. Wartet nie.
std::int64_t nowMicros() noexcept {
    return std::chrono::duration_cast<std::chrono::microseconds>(
               std::chrono::steady_clock::now().time_since_epoch())
        .count();
}

MidiMessage onChannel(MidiMessage m, int channel) noexcept {
    m.status = static_cast<std::uint8_t>((m.status & 0xF0) | (channel & 0x0F));
    return m;
}

}  // namespace

Engine::~Engine() {
    // Alles freigeben, was noch an MIDI-Sequenzen unterwegs ist.
    for (auto& t : tracks_) delete t.sequence;
    Command c;
    while (commands_.pop(c)) delete c.sequence;
    collectGarbage();
}

// --- UI-Thread ---------------------------------------------------------------

bool Engine::play() noexcept { return send({CommandType::Play}); }
bool Engine::stop() noexcept { return send({CommandType::Stop}); }

bool Engine::locate(std::int64_t samplePosition) noexcept {
    Command c{CommandType::Locate};
    c.position = std::max<std::int64_t>(0, samplePosition);
    return send(c);
}

bool Engine::setTrackGain(int track, float gain) noexcept {
    if (!validTrack(track)) return false;
    return send({CommandType::TrackGain, track, std::max(0.0f, gain)});
}

bool Engine::setTrackPan(int track, float pan) noexcept {
    if (!validTrack(track)) return false;
    return send({CommandType::TrackPan, track, std::clamp(pan, -1.0f, 1.0f)});
}

bool Engine::setTrackMute(int track, bool mute) noexcept {
    if (!validTrack(track)) return false;
    return send({CommandType::TrackMute, track, mute ? 1.0f : 0.0f});
}

bool Engine::setTrackSolo(int track, bool solo) noexcept {
    if (!validTrack(track)) return false;
    return send({CommandType::TrackSolo, track, solo ? 1.0f : 0.0f});
}

bool Engine::setMasterGain(float gain) noexcept {
    return send({CommandType::MasterGain, 0, std::max(0.0f, gain)});
}

bool Engine::setTempo(double bpm) noexcept {
    return send({CommandType::Tempo, 0, static_cast<float>(std::clamp(bpm, 20.0, 400.0))});
}

bool Engine::startLatencyProbe(LatencyProbe* probe) noexcept {
    if (!probe) return false;
    Command c{CommandType::Probe};
    c.probe = probe;
    return send(c);
}

bool Engine::setOffline(bool on) noexcept { return send({CommandType::Offline, 0, on ? 1.0f : 0.0f}); }

bool Engine::setMetronome(bool on) noexcept { return send({CommandType::Metronome, 0, on ? 1.0f : 0.0f}); }

bool Engine::setTrackTestTone(int track, float hz) noexcept {
    if (!validTrack(track)) return false;
    return send({CommandType::TrackTestTone, track, std::max(0.0f, hz)});
}

bool Engine::setTrackPlaylist(int track, Playlist playlist) {
    if (!validTrack(track)) return false;
    streams_[static_cast<std::size_t>(track)].setPlaylist(std::move(playlist));
    return true;
}

bool Engine::setTrackMidiInput(int track, int input) noexcept {
    if (!validTrack(track) || input < kInputAudio || input > 15) return false;
    Command c{CommandType::TrackMidiInput, track};
    c.position = input;
    return send(c);
}

bool Engine::setTrackMidiOutput(int track, int output) noexcept {
    if (!validTrack(track) || output < kOutputSynth || output > 15) return false;
    Command c{CommandType::TrackMidiOutput, track};
    c.position = output;
    return send(c);
}

bool Engine::setTrackMidiSequence(int track, std::unique_ptr<MidiSequence> sequence) {
    if (!validTrack(track)) return false;
    Command c{CommandType::TrackMidiSequence, track};
    c.sequence = sequence.release();
    if (send(c)) return true;
    delete c.sequence;
    return false;
}

void Engine::collectGarbage() {
    const MidiSequence* old = nullptr;
    while (garbage_.pop(old)) delete old;
}

bool Engine::setPlugin(int strip, int slot, AudioPlugin* plugin) noexcept {
    if (strip < 0 || strip >= kNumStrips || slot < 0 || slot >= kNumSlots) return false;
    if (slot == kInstrumentSlot && strip >= kNumTracks) return false;  // Instrumente nur auf Spuren
    Command c{CommandType::TrackPlugin, strip};
    c.position = slot;
    c.plugin = plugin;
    return send(c);
}

bool Engine::setTrackPlugin(int track, AudioPlugin* plugin) noexcept {
    if (!validTrack(track)) return false;
    const int slot = (plugin && !plugin->isInstrument()) ? 1 : kInstrumentSlot;
    return setPlugin(track, 1 - slot, nullptr) && setPlugin(track, slot, plugin);
}

bool Engine::setTrackSend(int track, int bus, float level) noexcept {
    if (!validTrack(track) || bus < 0 || bus >= kNumBuses) return false;
    Command c{CommandType::TrackSend, track, std::max(0.0f, level)};
    c.position = bus;
    return send(c);
}

bool Engine::setTrackOutput(int track, int bus) noexcept {
    if (!validTrack(track) || bus < -1 || bus >= kNumBuses) return false;
    Command c{CommandType::TrackOutput, track};
    c.position = bus;
    return send(c);
}

bool Engine::setBusGain(int bus, float gain) noexcept {
    if (bus < 0 || bus >= kNumBuses) return false;
    return send({CommandType::BusGain, bus, std::max(0.0f, gain)});
}

bool Engine::setBusPan(int bus, float pan) noexcept {
    if (bus < 0 || bus >= kNumBuses) return false;
    return send({CommandType::BusPan, bus, std::clamp(pan, -1.0f, 1.0f)});
}

bool Engine::setBusMute(int bus, bool mute) noexcept {
    if (bus < 0 || bus >= kNumBuses) return false;
    return send({CommandType::BusMute, bus, mute ? 1.0f : 0.0f});
}

bool Engine::postMidi(MidiMessage message, bool fromKeyboard) {
    std::lock_guard<std::mutex> lock(midiPostMutex_);
    lastMidi_.store(static_cast<std::uint32_t>(message.status) |
                        (static_cast<std::uint32_t>(message.data1) << 8) |
                        (static_cast<std::uint32_t>(message.data2) << 16),
                    std::memory_order_relaxed);
    midiCount_.fetch_add(1, std::memory_order_relaxed);
    return midiIn_.push({message, fromKeyboard, nowMicros()});
}

bool Engine::setArmedTrack(int track) noexcept {
    if (track != -1 && !validTrack(track)) return false;
    return send({CommandType::ArmTrack, track});
}

bool Engine::setInputMonitor(bool on) noexcept {
    return send({CommandType::InputMonitor, 0, on ? 1.0f : 0.0f});
}

bool Engine::record(std::shared_ptr<RecordSink> sink) {
    if (!sink) return false;
    Command c{CommandType::Record};
    c.sink = sink.get();
    {
        std::lock_guard<std::mutex> lock(sinkMutex_);
        diskSink_ = std::move(sink);
    }
    if (send(c)) return true;
    releaseRecordSink();
    return false;
}

void Engine::releaseRecordSink() {
    std::lock_guard<std::mutex> lock(sinkMutex_);
    diskSink_.reset();
}

bool Engine::recordMidi() noexcept { return send({CommandType::RecordMidi}); }

float Engine::takeTrackPeak(int track) noexcept {
    if (!validTrack(track)) return 0.0f;
    return trackPeak_[static_cast<std::size_t>(track)].exchange(0.0f, std::memory_order_relaxed);
}

float Engine::takeMasterPeak() noexcept {
    return masterPeak_.exchange(0.0f, std::memory_order_relaxed);
}

float Engine::takeBusPeak(int bus) noexcept {
    if (bus < 0 || bus >= kNumBuses) return 0.0f;
    return busPeak_[static_cast<std::size_t>(bus)].exchange(0.0f, std::memory_order_relaxed);
}

float Engine::takeInputPeak() noexcept {
    return inputPeak_.exchange(0.0f, std::memory_order_relaxed);
}

// --- Disk-Thread -------------------------------------------------------------

bool Engine::serviceDisk() {
    bool worked = false;
    for (auto& stream : streams_) worked = stream.service() || worked;

    std::shared_ptr<RecordSink> sink;
    {
        std::lock_guard<std::mutex> lock(sinkMutex_);
        sink = diskSink_;
    }
    if (sink) worked = sink->service() || worked;
    return worked;
}

// --- Audio-Thread ------------------------------------------------------------

void Engine::prepare(double sampleRate, int /*numInputs*/, int /*numOutputs*/, int maxFrames) {
    sampleRate_ = sampleRate;
    maxFrames_ = std::max(1, maxFrames);
    const auto size = static_cast<std::size_t>(maxFrames_);
    masterL_.assign(size, 0.0f);
    masterR_.assign(size, 0.0f);
    trackL_.assign(size, 0.0f);
    trackR_.assign(size, 0.0f);
    pluginL_.assign(size, 0.0f);
    pluginR_.assign(size, 0.0f);
    for (auto& bus : buses_) {
        bus.left.assign(size, 0.0f);
        bus.right.assign(size, 0.0f);
        bus.delay.prepare();
    }
    for (auto& t : tracks_) t.delay.prepare();
    masterDirectDelay_.prepare();
    tailFrames_ = 0;

    for (auto& t : tracks_) {
        t.phase = 0.0;
        t.phaseInc = kTwoPi * static_cast<double>(t.toneHz) / sampleRate_;
        t.held = {};
        t.seqDirty = true;
    }
    for (auto& synth : synths_) synth.prepare(sampleRate);
    liveMidiCount_ = 0;
    updateTargets();
    recomputeLatency();
    for (auto& t : tracks_) {
        t.curL = t.tgtL;
        t.curR = t.tgtR;
        t.sendCur = t.send;
    }
    for (auto& bus : buses_) { bus.curL = bus.tgtL; bus.curR = bus.tgtR; }
    masterCur_ = masterGain_;
    transportCur_ = wantPlay_ ? 1.0f : 0.0f;
}

// Laesst alles verstummen, was die Spur gerade per MIDI spielt.
void Engine::silenceTrack(std::size_t ti, std::int64_t dueMicros) noexcept {
    Track& t = tracks_[ti];
    if (t.midiOutput == kOutputSynth) {
        synths_[ti].allNotesOff();
    } else {
        for (int note = 0; note < 128; ++note)
            if (t.held[static_cast<std::size_t>(note >> 6)] & (std::uint64_t{1} << (note & 63)))
                midiOut_.push({onChannel(MidiMessage::noteOff(note), t.midiOutput), dueMicros});
        midiOut_.push({MidiMessage::allNotesOff(t.midiOutput), dueMicros});
    }
    t.held = {};
}

void Engine::drainCommands() noexcept {
    const std::int64_t now = nowMicros();
    bool mixChanged = false;
    Command c;
    while (commands_.pop(c)) {
        Track* t = validTrack(c.track) ? &tracks_[static_cast<std::size_t>(c.track)] : nullptr;
        switch (c.type) {
            case CommandType::Play:
                wantPlay_ = true;
                rolling_ = true;
                break;
            case CommandType::Stop:
                wantPlay_ = false;  // rolling_ endet nach dem Ausblenden
                if (recordSink_) {
                    recordSink_->finish();
                    recordSink_ = nullptr;
                }
                if (midiRecording_) {
                    midiRecordOut_.push({MidiRecordEvent::Kind::End, {}, position_});
                    midiRecording_ = false;
                }
                break;
            case CommandType::Locate:
                position_ = c.position;
                break;
            case CommandType::TrackGain:
                if (t) { t->gain = c.value; mixChanged = true; }
                break;
            case CommandType::TrackPan:
                if (t) { t->pan = c.value; mixChanged = true; }
                break;
            case CommandType::TrackMute:
                if (t) { t->mute = c.value != 0.0f; mixChanged = true; }
                break;
            case CommandType::TrackSolo:
                if (t) { t->solo = c.value != 0.0f; mixChanged = true; }
                break;
            case CommandType::TrackTestTone:
                if (t) {
                    t->toneHz = c.value;
                    t->phase = 0.0;
                    t->phaseInc = kTwoPi * static_cast<double>(c.value) / sampleRate_;
                }
                break;
            case CommandType::MasterGain:
                masterGain_ = c.value;
                break;
            case CommandType::ArmTrack:
                // Gehaltene Noten der bisherigen Spur verstummen lassen.
                if (validTrack(armedTrack_) && armedTrack_ != c.track)
                    silenceTrack(static_cast<std::size_t>(armedTrack_), now);
                armedTrack_ = c.track;
                break;
            case CommandType::InputMonitor:
                monitor_ = c.value != 0.0f;
                break;
            case CommandType::Record:
                if (recordSink_) recordSink_->finish();
                recordSink_ = c.sink;
                wantPlay_ = true;
                rolling_ = true;
                break;
            case CommandType::RecordMidi:
                if (!midiRecording_) {
                    midiRecording_ = true;
                    midiRecordOut_.push({MidiRecordEvent::Kind::Begin, {}, position_});
                }
                wantPlay_ = true;
                rolling_ = true;
                break;
            case CommandType::TrackMidiInput:
                if (t) t->midiInput = static_cast<int>(c.position);
                break;
            case CommandType::TrackMidiOutput:
                if (t && t->midiOutput != static_cast<int>(c.position)) {
                    silenceTrack(static_cast<std::size_t>(c.track), now);  // am bisherigen Ziel
                    t->midiOutput = static_cast<int>(c.position);
                    t->seqDirty = true;
                }
                break;
            case CommandType::Tempo:
                tempo_ = static_cast<double>(c.value);
                break;
            case CommandType::Probe:
                probe_ = c.probe;
                break;
            case CommandType::Offline:
                offline_ = c.value != 0.0f;
                break;
            case CommandType::Metronome:
                metronome_ = c.value != 0.0f;
                break;
            case CommandType::TrackPlugin:
                if (AudioPlugin** slot = slotPointer(c.track, static_cast<int>(c.position))) {
                    if (*slot) releasedPlugins_.push(*slot);  // der Besitzer darf es jetzt zerstoeren
                    *slot = c.plugin;
                    if (t) {
                        // Was bisher klang, verstummt; die Sequenz setzt neu ein.
                        synths_[static_cast<std::size_t>(c.track)].allNotesOff();
                        t->held = {};
                        t->seqDirty = true;
                    }
                    recomputeLatency();
                } else if (c.plugin) {
                    releasedPlugins_.push(c.plugin);
                }
                break;
            case CommandType::TrackSend:
                if (t) t->send[static_cast<std::size_t>(c.position)] = c.value;
                break;
            case CommandType::TrackOutput:
                if (t) t->output = static_cast<int>(c.position);
                break;
            case CommandType::BusGain:
                buses_[static_cast<std::size_t>(c.track)].gain = c.value;
                mixChanged = true;
                break;
            case CommandType::BusPan:
                buses_[static_cast<std::size_t>(c.track)].pan = c.value;
                mixChanged = true;
                break;
            case CommandType::BusMute:
                buses_[static_cast<std::size_t>(c.track)].mute = c.value != 0.0f;
                mixChanged = true;
                break;
            case CommandType::TrackMidiSequence:
                if (t) {
                    if (t->sequence) garbage_.push(t->sequence);  // gibt der UI-Thread frei
                    t->sequence = c.sequence;
                    t->seqDirty = true;
                } else if (c.sequence) {
                    garbage_.push(c.sequence);
                }
                break;
        }
    }
    if (mixChanged) updateTargets();

    // Live-MIDI einsammeln; verteilt wird es im naechsten Block.
    LiveMidi m;
    while (liveMidiCount_ < static_cast<int>(liveMidi_.size()) && midiIn_.pop(m))
        liveMidi_[static_cast<std::size_t>(liveMidiCount_++)] = m;
}

void Engine::updateTargets() noexcept {
    bool anySolo = false;
    for (const auto& t : tracks_) anySolo = anySolo || t.solo;

    for (auto& t : tracks_) {
        // Mute gewinnt gegen Solo. Ist irgendwo Solo aktiv, sind nur Solo-Spuren hoerbar.
        const bool audible = !t.mute && (!anySolo || t.solo);
        if (!audible) { t.tgtL = 0.0f; t.tgtR = 0.0f; continue; }
        // Constant-Power-Panning: in der Mitte -3 dB je Seite.
        const float angle = (t.pan + 1.0f) * kQuarterPi;
        t.tgtL = t.gain * std::cos(angle);
        t.tgtR = t.gain * std::sin(angle);
        if (t.pan <= -1.0f) t.tgtR = 0.0f;
        if (t.pan >= 1.0f) t.tgtL = 0.0f;
    }

    // Busse: kein Solo (sie bleiben hoerbar, wenn eine Spur solo ist).
    for (auto& bus : buses_) {
        if (bus.mute) { bus.tgtL = 0.0f; bus.tgtR = 0.0f; continue; }
        const float angle = (bus.pan + 1.0f) * kQuarterPi;
        bus.tgtL = bus.gain * std::cos(angle);
        bus.tgtR = bus.gain * std::sin(angle);
        if (bus.pan <= -1.0f) bus.tgtR = 0.0f;
        if (bus.pan >= 1.0f) bus.tgtL = 0.0f;
    }
}

AudioPlugin** Engine::slotPointer(int strip, int slot) noexcept {
    if (slot < 0 || slot >= kNumSlots) return nullptr;
    const auto s = static_cast<std::size_t>(slot);
    if (strip >= 0 && strip < kNumTracks) return &tracks_[static_cast<std::size_t>(strip)].slots[s];
    if (slot == kInstrumentSlot) return nullptr;
    if (strip >= kFirstBus && strip < kMasterStrip) return &buses_[static_cast<std::size_t>(strip - kFirstBus)].slots[s];
    if (strip == kMasterStrip) return &masterSlots_[s];
    return nullptr;
}

// Latenzausgleich: Jede Spur wird auf die Spur mit der groessten Plugin-Latenz
// verzoegert, jeder Bus auf den langsamsten Bus, die direkt zum Master gehenden
// Spuren um die Bus-Latenz. Damit alles trotzdem zur Zeitachse passt, liest die
// Engine Clips und Noten um die Gesamtlatenz voraus.
void Engine::recomputeLatency() noexcept {
    const auto sum = [](const std::array<AudioPlugin*, kNumSlots>& slots, bool& any) {
        int total = 0;
        for (AudioPlugin* p : slots) {
            if (!p) continue;
            any = true;
            total += std::max(0, p->latencyFrames());
        }
        return std::min(total, DelayLine::kCapacity - 1);
    };
    bool any = false;
    std::array<int, kNumTracks> trackLatency{};
    std::array<int, kNumBuses> busLatency{};
    int maxTrack = 0, maxBus = 0;
    for (std::size_t i = 0; i < tracks_.size(); ++i) {
        trackLatency[i] = sum(tracks_[i].slots, any);
        maxTrack = std::max(maxTrack, trackLatency[i]);
    }
    for (std::size_t i = 0; i < buses_.size(); ++i) {
        busLatency[i] = sum(buses_[i].slots, any);
        maxBus = std::max(maxBus, busLatency[i]);
    }
    const int masterLatency = sum(masterSlots_, any);
    for (std::size_t i = 0; i < tracks_.size(); ++i) tracks_[i].delay.setDelay(maxTrack - trackLatency[i]);
    for (std::size_t i = 0; i < buses_.size(); ++i) buses_[i].delay.setDelay(maxBus - busLatency[i]);
    masterDirectDelay_.setDelay(maxBus);
    anyPlugin_ = any;
    latency_ = maxTrack + maxBus + masterLatency;
    latencyOut_.store(latency_, std::memory_order_relaxed);
    for (auto& t : tracks_) t.seqDirty = true;  // Noten setzen an der neuen Leseposition ein
}

// Rechnet die Effekt-Plaetze 1..4 nacheinander. Ein Instrument auf einem
// Effekt-Platz bekommt das MIDI der Spur.
void Engine::runInserts(std::array<AudioPlugin*, kNumSlots>& slots, float* left, float* right, int numFrames,
                        const PluginMidiEvent* events, int numEvents, const PluginTransport& transport) noexcept {
    for (int s = 1; s < kNumSlots; ++s) {
        AudioPlugin* p = slots[static_cast<std::size_t>(s)];
        if (!p) continue;
        const bool instrument = p->isInstrument();
        p->process(left, right, numFrames, instrument ? events : nullptr, instrument ? numEvents : 0, transport);
    }
}

// Sammelt die MIDI-Ereignisse einer Spur fuer diesen Block nach blockEvents_,
// nach Zeit geordnet: live gespielte Noten (am Blockanfang) und die Noten aus
// den MIDI-Clips (samplegenau). Liefert die Anzahl.
int Engine::gatherMidi(std::size_t ti, int numFrames, std::int64_t blockMicros) noexcept {
    Track& t = tracks_[ti];
    int count = 0;
    const auto add = [&](int offset, MidiMessage m) {
        if (count >= static_cast<int>(blockEvents_.size())) return false;
        blockEvents_[static_cast<std::size_t>(count++)] = {offset, m};
        return true;
    };
    const auto releaseHeld = [&] {
        for (int note = 0; note < 128; ++note)
            if (t.held[static_cast<std::size_t>(note >> 6)] & (std::uint64_t{1} << (note & 63)))
                add(0, MidiMessage::noteOff(note));
        t.held = {};
    };
    const bool anyHeld = (t.held[0] | t.held[1]) != 0;

    // Live-MIDI: nur fuer die scharfgeschaltete Spur und nur, wenn der Kanal passt.
    if (static_cast<int>(ti) == armedTrack_ && t.midiInput != kInputAudio) {
        const double framesPerMicro = sampleRate_ / 1'000'000.0;
        for (int i = 0; i < liveMidiCount_; ++i) {
            const LiveMidi& live = liveMidi_[static_cast<std::size_t>(i)];
            const int channel = live.message.status & 0x0F;
            if (!live.keyboard && t.midiInput != kInputMidiAll && channel != t.midiInput) continue;
            add(0, live.message);

            if (midiRecording_) {
                // Zeitachsen-Position der Note: Sie kam etwas vor diesem Block an,
                // und der Spieler hoert alles um die Ausgangslatenz verzoegert.
                const std::int64_t age = std::clamp<std::int64_t>(blockMicros - live.micros, 0, 200'000);
                const auto ageFrames = static_cast<std::int64_t>(static_cast<double>(age) * framesPerMicro);
                const std::int64_t pos = position_ - ageFrames - outputLatency_.load(std::memory_order_relaxed);
                midiRecordOut_.push({MidiRecordEvent::Kind::Message, live.message, pos});
            }
        }
    }

    lastLiveCount_ = count;

    // Noten aus den MIDI-Clips, um die Plugin-Latenz vorausgelesen.
    const std::int64_t sourcePos = position_ + latency_;
    if (rolling_ && wantPlay_ && t.sequence) {
        const auto& events = t.sequence->events;
        if (t.seqDirty || t.seqNextPos != sourcePos) {
            // Sprung oder neue Daten: klingende Noten beenden und neu einsetzen.
            if (anyHeld) releaseHeld();
            const auto it = std::lower_bound(
                events.begin(), events.end(), sourcePos,
                [](const MidiSequence::Event& e, std::int64_t pos) { return e.frame < pos; });
            t.seqIndex = static_cast<std::size_t>(it - events.begin());
            t.seqDirty = false;
        }
        const std::int64_t end = sourcePos + numFrames;
        while (t.seqIndex < events.size() && events[t.seqIndex].frame < end) {
            const MidiSequence::Event& e = events[t.seqIndex++];
            if (!add(static_cast<int>(e.frame - sourcePos), e.message)) continue;
            const auto word = static_cast<std::size_t>(e.message.data1 >> 6);
            const std::uint64_t bit = std::uint64_t{1} << (e.message.data1 & 63);
            if (e.message.isNoteOn()) t.held[word] |= bit;
            else if (e.message.isNoteOff()) t.held[word] &= ~bit;
        }
        t.seqNextPos = end;
    } else if (anyHeld) {
        releaseHeld();  // Stopp oder keine Daten mehr
    }
    return count;
}

void Engine::renderBlock(int numFrames, std::int64_t blockMicros) noexcept {
    float* L = masterL_.data();
    float* R = masterR_.data();
    const std::size_t bytes = static_cast<std::size_t>(numFrames) * sizeof(float);
    std::memset(L, 0, bytes);
    std::memset(R, 0, bytes);

    const bool haveInput = inL_ != nullptr;
    const bool monitoring = monitor_ && haveInput && armedTrack_ >= 0;

    // Eingangspegel fuer die Anzeige der scharfgeschalteten Spur.
    if (haveInput && armedTrack_ >= 0) {
        float peak = 0.0f;
        for (int i = 0; i < numFrames; ++i)
            peak = std::max(peak, std::max(std::fabs(inL_[i]), std::fabs(inR_[i])));
        raisePeak(inputPeak_, peak);
    }

    // Synthesizer mit klingenden Stimmen und Plugins laufen auch bei stehendem Transport weiter.
    bool anySynth = anyPlugin_;
    for (const auto& synth : synths_) anySynth = anySynth || synth.active();
    const bool liveMidi = liveMidiCount_ > 0 && armedTrack_ >= 0;

    // Clips und Noten werden um die Plugin-Latenz vorausgelesen.
    const std::int64_t sourcePos = position_ + latency_;

    if (!rolling_) {
        // Transport steht: Stroeme auf die aktuelle Position vorbereiten,
        // damit beim Start sofort Daten da sind.
        for (auto& stream : streams_)
            if (stream.active()) stream.prime(sourcePos);
        if (!monitoring && !anySynth && !liveMidi && tailFrames_ <= 0) {
            liveMidiCount_ = 0;
            return;
        }
    }

    // Aufnahme: den Eingang dieses Blocks an den Disk-Thread weiterreichen.
    if (rolling_ && recordSink_ && haveInput) recordSink_->push(position_, inL_, inR_, numFrames);

    for (auto& bus : buses_) {
        std::memset(bus.left.data(), 0, bytes);
        std::memset(bus.right.data(), 0, bytes);
    }

    const float invFrames = 1.0f / static_cast<float>(numFrames);
    const float transportTarget = (rolling_ && wantPlay_) ? 1.0f : 0.0f;
    if (offline_) transportCur_ = transportTarget;  // beim Export nichts ein- oder ausblenden
    const float transportStep = (transportTarget - transportCur_) * invFrames;
    const bool transportFading = transportCur_ != 1.0f || transportTarget != 1.0f;
    const double microsPerFrame = 1'000'000.0 / sampleRate_;
    const int outputLatency = outputLatency_.load(std::memory_order_relaxed);
    const PluginTransport transport{rolling_ && wantPlay_, sourcePos, tempo_, sampleRate_};
    float* tl = trackL_.data();
    float* tr = trackR_.data();
    bool anyActive = false;

    for (std::size_t ti = 0; ti < tracks_.size(); ++ti) {
        Track& t = tracks_[ti];
        TrackStream& stream = streams_[ti];

        // MIDI dieser Spur: an Instrument, eingebauten Synthesizer oder MIDI-Geraet.
        const int midiCount = gatherMidi(ti, numFrames, blockMicros);
        const bool midiToSynth = t.midiOutput == kOutputSynth;
        if (!midiToSynth) {
            for (int i = 0; i < midiCount; ++i) {
                const BlockEvent& e = blockEvents_[static_cast<std::size_t>(i)];
                // Senden, wenn das zugehoerige Audio aus dem Lautsprecher kommt. Noten aus
                // Clips sind um die Plugin-Latenz vorausgelesen und warten entsprechend laenger.
                const int ahead = i >= lastLiveCount_ ? latency_ : 0;
                const auto due = blockMicros +
                                 static_cast<std::int64_t>((e.offset + outputLatency + ahead) * microsPerFrame);
                midiOut_.push({onChannel(e.message, t.midiOutput), due});
            }
        }

        AudioPlugin* const first = t.slots[kInstrumentSlot];
        AudioPlugin* const instrument = (first && first->isInstrument()) ? first : nullptr;
        bool hasPlugins = false;
        for (AudioPlugin* p : t.slots) hasPlugins = hasPlugins || p != nullptr;

        const bool hasClips = rolling_ && stream.active();
        const bool hasTone = rolling_ && t.toneHz > 0.0f;
        const bool hasInput = monitoring && static_cast<int>(ti) == armedTrack_;
        const bool midiToBuiltIn = midiToSynth && instrument == nullptr;
        const bool hasSynth = synths_[ti].active() || (midiToBuiltIn && midiCount > 0);
        const bool active = hasClips || hasTone || hasInput || hasSynth || hasPlugins;
        anyActive = anyActive || active;
        if (!active && tailFrames_ <= 0) {
            t.curL = t.tgtL;
            t.curR = t.tgtR;
            t.sendCur = t.send;
            continue;
        }

        // Quelle der Spur nach tl/tr: Audio-Clips, dazu gegebenenfalls der Testton.
        if (hasClips) {
            if (stream.read(sourcePos, tl, tr, numFrames) < numFrames)
                underruns_.fetch_add(1, std::memory_order_relaxed);
        } else {
            std::memset(tl, 0, bytes);
            std::memset(tr, 0, bytes);
        }
        if (hasTone) {
            for (int i = 0; i < numFrames; ++i) {
                const float s = kTestToneLevel * static_cast<float>(std::sin(t.phase));
                t.phase += t.phaseInc;
                if (t.phase >= kTwoPi) t.phase -= kTwoPi;
                tl[i] += s;
                tr[i] += s;
            }
        }

        // Start und Stopp blenden das abgespielte Material ueber einen Block.
        if ((hasClips || hasTone) && transportFading) {
            float g = transportCur_;
            for (int i = 0; i < numFrames; ++i) {
                g += transportStep;
                tl[i] *= g;
                tr[i] *= g;
            }
        }

        // Mithoeren: Der Eingang laeuft durch Effekte, Fader und Panorama der Spur.
        if (hasInput) {
            for (int i = 0; i < numFrames; ++i) {
                tl[i] += inL_[i];
                tr[i] += inR_[i];
            }
        }

        // Synthesizer der Spur. Zwischen den Ereignissen wird stueckweise
        // gerechnet, damit jede Note auf das Sample genau einsetzt.
        if (hasSynth) {
            Synth& synth = synths_[ti];
            int done = 0;
            if (midiToBuiltIn) {
                for (int i = 0; i < midiCount; ++i) {
                    const BlockEvent& e = blockEvents_[static_cast<std::size_t>(i)];
                    const int offset = std::clamp(e.offset, done, numFrames);
                    if (offset > done) synth.render(tl + done, tr + done, offset - done);
                    done = offset;
                    synth.handle(e.message);
                }
            }
            if (numFrames > done) synth.render(tl + done, tr + done, numFrames - done);
        }

        // Platz 0: Ein Instrument bekommt das MIDI und kommt zum Klang der Spur
        // dazu; ein Effekt auf diesem Platz bearbeitet alles bis hier.
        if (first != nullptr) {
            if (instrument != nullptr) {
                std::memset(pluginL_.data(), 0, bytes);
                std::memset(pluginR_.data(), 0, bytes);
                instrument->process(pluginL_.data(), pluginR_.data(), numFrames, blockEvents_.data(),
                                    midiToSynth ? midiCount : 0, transport);
                for (int i = 0; i < numFrames; ++i) {
                    tl[i] += pluginL_[static_cast<std::size_t>(i)];
                    tr[i] += pluginR_[static_cast<std::size_t>(i)];
                }
            } else {
                first->process(tl, tr, numFrames, nullptr, 0, transport);
            }
        }

        // Effekte 1..4, dann Latenzausgleich.
        runInserts(t.slots, tl, tr, numFrames, blockEvents_.data(), midiToSynth ? midiCount : 0, transport);
        t.delay.process(tl, tr, numFrames);

        // Fader und Panorama, ueber den Block ueberblendet; dann Ausgang und Sends.
        float* outL = t.output < 0 ? L : buses_[static_cast<std::size_t>(t.output)].left.data();
        float* outR = t.output < 0 ? R : buses_[static_cast<std::size_t>(t.output)].right.data();
        const float stepL = (t.tgtL - t.curL) * invFrames;
        const float stepR = (t.tgtR - t.curR) * invFrames;
        float gl = t.curL, gr = t.curR;
        float peak = 0.0f;
        for (int i = 0; i < numFrames; ++i) {
            gl += stepL;
            gr += stepR;
            const float l = tl[i] * gl;
            const float r = tr[i] * gr;
            tl[i] = l;
            tr[i] = r;
            outL[i] += l;
            outR[i] += r;
            peak = std::max(peak, std::max(std::fabs(l), std::fabs(r)));
        }
        t.curL = t.tgtL;
        t.curR = t.tgtR;
        raisePeak(trackPeak_[ti], peak);

        for (std::size_t b = 0; b < buses_.size(); ++b) {
            const float target = t.send[b];
            float level = t.sendCur[b];
            if (target == 0.0f && level == 0.0f) continue;
            const float step = (target - level) * invFrames;
            float* bl = buses_[b].left.data();
            float* br = buses_[b].right.data();
            for (int i = 0; i < numFrames; ++i) {
                level += step;
                bl[i] += tl[i] * level;
                br[i] += tr[i] * level;
            }
            t.sendCur[b] = target;
        }
    }
    liveMidiCount_ = 0;

    // Verzoegerungen laufen nach, bis alles, was in ihnen steckt, heraus ist.
    if (anyActive) tailFrames_ = static_cast<std::int64_t>(latency_) + numFrames;
    else tailFrames_ = std::max<std::int64_t>(0, tailFrames_ - numFrames);

    // Direkt zum Master gehende Spuren warten auf die Busse.
    masterDirectDelay_.process(L, R, numFrames);

    // Busse: Effekte, Latenzausgleich, Fader und Panorama, dann zum Master.
    for (std::size_t b = 0; b < buses_.size(); ++b) {
        Bus& bus = buses_[b];
        float* bl = bus.left.data();
        float* br = bus.right.data();
        runInserts(bus.slots, bl, br, numFrames, nullptr, 0, transport);
        bus.delay.process(bl, br, numFrames);
        const float stepL = (bus.tgtL - bus.curL) * invFrames;
        const float stepR = (bus.tgtR - bus.curR) * invFrames;
        float gl = bus.curL, gr = bus.curR;
        float peak = 0.0f;
        for (int i = 0; i < numFrames; ++i) {
            gl += stepL;
            gr += stepR;
            const float l = bl[i] * gl;
            const float r = br[i] * gr;
            L[i] += l;
            R[i] += r;
            peak = std::max(peak, std::max(std::fabs(l), std::fabs(r)));
        }
        bus.curL = bus.tgtL;
        bus.curR = bus.tgtR;
        raisePeak(busPeak_[b], peak);
    }

    // Effekte auf dem Master.
    runInserts(masterSlots_, L, R, numFrames, nullptr, 0, transport);

    // Metronom: kurzer Klick auf jedem Schlag, hoeher auf der Eins.
    if (metronome_ && rolling_ && wantPlay_) {
        const double framesPerBeat = sampleRate_ * 60.0 / tempo_;
        auto beat = static_cast<std::int64_t>(std::ceil(static_cast<double>(position_) / framesPerBeat));
        auto beatFrame = static_cast<std::int64_t>(std::ceil(static_cast<double>(beat) * framesPerBeat));
        for (int i = 0; i < numFrames; ++i) {
            if (position_ + i == beatFrame) {
                clickLength_ = std::max(1, static_cast<int>(0.03 * sampleRate_));
                clickRemaining_ = clickLength_;
                clickPhase_ = 0.0;
                clickPhaseInc_ = kTwoPi * (beat % 4 == 0 ? 1600.0 : 1000.0) / sampleRate_;
                ++beat;
                beatFrame = static_cast<std::int64_t>(std::ceil(static_cast<double>(beat) * framesPerBeat));
            }
            if (clickRemaining_ > 0) {
                const float env = static_cast<float>(clickRemaining_) / static_cast<float>(clickLength_);
                const float s = 0.35f * env * env * static_cast<float>(std::sin(clickPhase_));
                clickPhase_ += clickPhaseInc_;
                --clickRemaining_;
                L[i] += s;
                R[i] += s;
            }
        }
    } else {
        clickRemaining_ = 0;
    }

    // Master-Fader.
    const float step = (masterGain_ - masterCur_) * invFrames;
    float g = masterCur_;
    float peak = 0.0f;
    for (int i = 0; i < numFrames; ++i) {
        g += step;
        L[i] *= g;
        R[i] *= g;
        peak = std::max(peak, std::max(std::fabs(L[i]), std::fabs(R[i])));
    }
    masterCur_ = masterGain_;
    raisePeak(masterPeak_, peak);

    if (rolling_) {
        transportCur_ = transportTarget;
        position_ += numFrames;
        if (!wantPlay_) rolling_ = false;  // ausgeblendet, Transport steht
    }
}

void Engine::process(const float* const* in, int numInputs, float* const* out, int numOutputs,
                     int numFrames) noexcept {
    const std::int64_t blockMicros = nowMicros();
    drainCommands();

    const double microsPerFrame = 1'000'000.0 / sampleRate_;
    int done = 0;
    while (done < numFrames) {
        const int n = std::min(numFrames - done, maxFrames_);
        if (in != nullptr && numInputs > 0) {
            inL_ = in[0] + done;
            inR_ = (numInputs > 1 ? in[1] : in[0]) + done;
        } else {
            inL_ = inR_ = nullptr;
        }
        renderBlock(n, blockMicros + static_cast<std::int64_t>(done * microsPerFrame));

        const std::size_t bytes = static_cast<std::size_t>(n) * sizeof(float);
        if (numOutputs >= 2) {
            std::memcpy(out[0] + done, masterL_.data(), bytes);
            std::memcpy(out[1] + done, masterR_.data(), bytes);
            for (int c = 2; c < numOutputs; ++c) std::memset(out[c] + done, 0, bytes);
        } else if (numOutputs == 1) {
            for (int i = 0; i < n; ++i)
                out[0][done + i] = 0.5f * (masterL_[static_cast<std::size_t>(i)] +
                                           masterR_[static_cast<std::size_t>(i)]);
        }
        done += n;
    }
    inL_ = inR_ = nullptr;

    // Latenzmessung: Das Testsignal ersetzt alles, was die Engine sonst ausgibt.
    if (probe_ && numOutputs > 0) {
        const float* probeL = (in != nullptr && numInputs > 0) ? in[0] : nullptr;
        const float* probeR = (in != nullptr && numInputs > 1) ? in[1] : probeL;
        probe_->process(probeL, probeR, out[0], numOutputs > 1 ? out[1] : nullptr, numFrames);
        if (probe_->done()) probe_ = nullptr;
    }

    positionOut_.store(position_, std::memory_order_relaxed);
    playingOut_.store(wantPlay_, std::memory_order_relaxed);
    recordingOut_.store(recordSink_ != nullptr || midiRecording_, std::memory_order_relaxed);
}
