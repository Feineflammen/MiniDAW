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

bool Engine::setTrackPlugin(int track, AudioPlugin* plugin) noexcept {
    if (!validTrack(track)) return false;
    Command c{CommandType::TrackPlugin, track};
    c.plugin = plugin;
    return send(c);
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

    for (auto& t : tracks_) {
        t.phase = 0.0;
        t.phaseInc = kTwoPi * static_cast<double>(t.toneHz) / sampleRate_;
        t.held = {};
        t.seqDirty = true;
    }
    for (auto& synth : synths_) synth.prepare(sampleRate);
    liveMidiCount_ = 0;
    updateTargets();
    for (auto& t : tracks_) { t.curL = t.tgtL; t.curR = t.tgtR; }
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
            case CommandType::Metronome:
                metronome_ = c.value != 0.0f;
                break;
            case CommandType::TrackPlugin:
                if (t) {
                    if (t->plugin) releasedPlugins_.push(t->plugin);  // der Besitzer darf es jetzt zerstoeren
                    t->plugin = c.plugin;
                    // Was bisher klang, verstummt; die Sequenz setzt neu ein.
                    synths_[static_cast<std::size_t>(c.track)].allNotesOff();
                    t->held = {};
                    t->seqDirty = true;
                } else if (c.plugin) {
                    releasedPlugins_.push(c.plugin);
                }
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

    // Noten aus den MIDI-Clips.
    if (rolling_ && wantPlay_ && t.sequence) {
        const auto& events = t.sequence->events;
        if (t.seqDirty || t.seqNextPos != position_) {
            // Sprung oder neue Daten: klingende Noten beenden und neu einsetzen.
            if (anyHeld) releaseHeld();
            const auto it = std::lower_bound(
                events.begin(), events.end(), position_,
                [](const MidiSequence::Event& e, std::int64_t pos) { return e.frame < pos; });
            t.seqIndex = static_cast<std::size_t>(it - events.begin());
            t.seqDirty = false;
        }
        const std::int64_t end = position_ + numFrames;
        while (t.seqIndex < events.size() && events[t.seqIndex].frame < end) {
            const MidiSequence::Event& e = events[t.seqIndex++];
            if (!add(static_cast<int>(e.frame - position_), e.message)) continue;
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
    bool anySynth = false;
    for (const auto& synth : synths_) anySynth = anySynth || synth.active();
    for (const auto& track : tracks_) anySynth = anySynth || track.plugin != nullptr;
    const bool liveMidi = liveMidiCount_ > 0 && armedTrack_ >= 0;

    if (!rolling_) {
        // Transport steht: Stroeme auf die aktuelle Position vorbereiten,
        // damit beim Start sofort Daten da sind.
        for (auto& stream : streams_)
            if (stream.active()) stream.prime(position_);
        if (!monitoring && !anySynth && !liveMidi) {
            liveMidiCount_ = 0;
            return;
        }
    }

    // Aufnahme: den Eingang dieses Blocks an den Disk-Thread weiterreichen.
    if (rolling_ && recordSink_ && haveInput) recordSink_->push(position_, inL_, inR_, numFrames);

    const float invFrames = 1.0f / static_cast<float>(numFrames);
    const float transportTarget = (rolling_ && wantPlay_) ? 1.0f : 0.0f;
    const float transportStep = (transportTarget - transportCur_) * invFrames;
    const bool transportFading = transportCur_ != 1.0f || transportTarget != 1.0f;
    const double microsPerFrame = 1'000'000.0 / sampleRate_;
    const int outputLatency = outputLatency_.load(std::memory_order_relaxed);
    float* tl = trackL_.data();
    float* tr = trackR_.data();

    for (std::size_t ti = 0; ti < tracks_.size(); ++ti) {
        Track& t = tracks_[ti];
        TrackStream& stream = streams_[ti];

        // MIDI dieser Spur: an den eingebauten Synthesizer oder an das MIDI-Geraet.
        const int midiCount = gatherMidi(ti, numFrames, blockMicros);
        const bool midiToSynth = t.midiOutput == kOutputSynth;
        if (!midiToSynth) {
            for (int i = 0; i < midiCount; ++i) {
                const BlockEvent& e = blockEvents_[static_cast<std::size_t>(i)];
                // Senden, wenn das zugehoerige Audio aus dem Lautsprecher kommt.
                const auto due = blockMicros +
                                 static_cast<std::int64_t>((e.offset + outputLatency) * microsPerFrame);
                midiOut_.push({onChannel(e.message, t.midiOutput), due});
            }
        }

        const bool hasClips = rolling_ && stream.active();
        const bool hasTone = rolling_ && t.toneHz > 0.0f;
        const bool hasInput = monitoring && static_cast<int>(ti) == armedTrack_;
        AudioPlugin* const instrument = (t.plugin && t.plugin->isInstrument()) ? t.plugin : nullptr;
        AudioPlugin* const effect = (t.plugin && !t.plugin->isInstrument()) ? t.plugin : nullptr;
        const bool midiToBuiltIn = midiToSynth && instrument == nullptr;
        const bool hasSynth = synths_[ti].active() || (midiToBuiltIn && midiCount > 0);
        if (!hasClips && !hasTone && !hasInput && !hasSynth && t.plugin == nullptr) {
            t.curL = t.tgtL;
            t.curR = t.tgtR;
            continue;
        }

        // Quelle der Spur nach tl/tr: Audio-Clips, dazu gegebenenfalls der Testton.
        if (hasClips) {
            if (stream.read(position_, tl, tr, numFrames) < numFrames)
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

        // Mithoeren: Der Eingang laeuft durch Fader und Panorama der Spur.
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

        // Plugin der Spur: Ein Instrument bekommt das MIDI und kommt zum Klang
        // der Spur dazu, ein Effekt bearbeitet alles, was bis hier entstanden ist.
        if (t.plugin != nullptr) {
            const PluginTransport transport{rolling_ && wantPlay_, position_, tempo_, sampleRate_};
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
                effect->process(tl, tr, numFrames, nullptr, 0, transport);
            }
        }

        // Fader und Panorama, ueber den Block ueberblendet.
        const float stepL = (t.tgtL - t.curL) * invFrames;
        const float stepR = (t.tgtR - t.curR) * invFrames;
        float gl = t.curL, gr = t.curR;
        float peak = 0.0f;
        for (int i = 0; i < numFrames; ++i) {
            gl += stepL;
            gr += stepR;
            const float l = tl[i] * gl;
            const float r = tr[i] * gr;
            L[i] += l;
            R[i] += r;
            peak = std::max(peak, std::max(std::fabs(l), std::fabs(r)));
        }
        t.curL = t.tgtL;
        t.curR = t.tgtR;
        raisePeak(trackPeak_[ti], peak);
    }
    liveMidiCount_ = 0;

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

    positionOut_.store(position_, std::memory_order_relaxed);
    playingOut_.store(wantPlay_, std::memory_order_relaxed);
    recordingOut_.store(recordSink_ != nullptr || midiRecording_, std::memory_order_relaxed);
}
