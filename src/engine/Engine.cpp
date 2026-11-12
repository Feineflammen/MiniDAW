#include "engine/Engine.h"

#include <algorithm>
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

}  // namespace

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

bool Engine::setTrackTestTone(int track, float hz) noexcept {
    if (!validTrack(track)) return false;
    return send({CommandType::TrackTestTone, track, std::max(0.0f, hz)});
}

bool Engine::setTrackPlaylist(int track, Playlist playlist) {
    if (!validTrack(track)) return false;
    streams_[static_cast<std::size_t>(track)].setPlaylist(std::move(playlist));
    return true;
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

    for (auto& t : tracks_) {
        t.phase = 0.0;
        t.phaseInc = kTwoPi * static_cast<double>(t.toneHz) / sampleRate_;
    }
    updateTargets();
    for (auto& t : tracks_) { t.curL = t.tgtL; t.curR = t.tgtR; }
    masterCur_ = masterGain_;
    transportCur_ = wantPlay_ ? 1.0f : 0.0f;
}

void Engine::drainCommands() noexcept {
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
        }
    }
    if (mixChanged) updateTargets();
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

void Engine::renderBlock(int numFrames) noexcept {
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

    if (!rolling_) {
        // Transport steht: Stroeme auf die aktuelle Position vorbereiten,
        // damit beim Start sofort Daten da sind.
        for (auto& stream : streams_)
            if (stream.active()) stream.prime(position_);
        if (!monitoring) return;
    }

    // Aufnahme: den Eingang dieses Blocks an den Disk-Thread weiterreichen.
    if (rolling_ && recordSink_ && haveInput) recordSink_->push(position_, inL_, inR_, numFrames);

    const float invFrames = 1.0f / static_cast<float>(numFrames);
    const float transportTarget = (rolling_ && wantPlay_) ? 1.0f : 0.0f;
    const float transportStep = (transportTarget - transportCur_) * invFrames;
    const bool transportFading = transportCur_ != 1.0f || transportTarget != 1.0f;
    float* tl = trackL_.data();
    float* tr = trackR_.data();

    for (std::size_t ti = 0; ti < tracks_.size(); ++ti) {
        Track& t = tracks_[ti];
        TrackStream& stream = streams_[ti];
        const bool hasClips = rolling_ && stream.active();
        const bool hasTone = rolling_ && t.toneHz > 0.0f;
        const bool hasInput = monitoring && static_cast<int>(ti) == armedTrack_;
        if (!hasClips && !hasTone && !hasInput) { t.curL = t.tgtL; t.curR = t.tgtR; continue; }

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
    drainCommands();

    int done = 0;
    while (done < numFrames) {
        const int n = std::min(numFrames - done, maxFrames_);
        if (in != nullptr && numInputs > 0) {
            inL_ = in[0] + done;
            inR_ = (numInputs > 1 ? in[1] : in[0]) + done;
        } else {
            inL_ = inR_ = nullptr;
        }
        renderBlock(n);

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
    recordingOut_.store(recordSink_ != nullptr, std::memory_order_relaxed);
}
