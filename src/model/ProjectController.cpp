#include "model/ProjectController.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <ctime>
#include <thread>
#include <utility>

ProjectController::ProjectController(Engine& engine) : engine_(engine) {
    for (int t = 0; t < kProjectTracks; ++t)
        project_.tracks[static_cast<std::size_t>(t)].name = L"Spur " + std::to_wstring(t + 1);
    for (int b = 0; b < kProjectBuses; ++b)
        project_.buses[static_cast<std::size_t>(b)].name = L"Bus " + std::to_wstring(b + 1);
}

std::shared_ptr<AudioFile> ProjectController::openFile(const std::filesystem::path& path,
                                                       std::string& error) {
    // Schon geladene Dateien wiederverwenden.
    for (auto it = files_.begin(); it != files_.end();) {
        auto file = it->lock();
        if (!file) { it = files_.erase(it); continue; }
        if (file->path == path) return file;
        ++it;
    }

    auto wav = WavFile::open(path, error);
    if (!wav) return nullptr;

    auto file = std::make_shared<AudioFile>();
    file->path = path;
    file->name = path.stem().wstring();
    file->wav = wav;
    file->peaks = std::make_shared<PeakData>(wav->frames());
    peakBuilder_.add(wav, file->peaks);
    files_.push_back(file);
    return file;
}

bool ProjectController::addClip(int track, const std::filesystem::path& path, std::int64_t start,
                                std::string& message) {
    message.clear();
    if (!validTrack(track)) { message = "Ungueltige Spur"; return false; }

    auto file = openFile(path, message);
    if (!file) return false;

    if (std::fabs(file->wav->sampleRate() - sampleRate_) > 0.5) {
        message = "Datei hat " + std::to_string(file->wav->sampleRate()) + " Hz, das Geraet " +
                  std::to_string(static_cast<int>(sampleRate_)) +
                  " Hz. Sie klingt deshalb zu " +
                  (file->wav->sampleRate() < sampleRate_ ? "hoch und schnell." : "tief und langsam.");
    }

    ClipModel clip;
    clip.start = std::max<std::int64_t>(0, start);
    clip.offset = 0;
    clip.length = file->wav->frames();
    clip.file = std::move(file);
    project_.tracks[static_cast<std::size_t>(track)].clips.push_back(std::move(clip));
    syncTrack(track);
    return true;
}

int ProjectController::moveClip(int track, int index, int newTrack, std::int64_t newStart) {
    if (!validTrack(track) || !validTrack(newTrack)) return -1;
    auto& from = project_.tracks[static_cast<std::size_t>(track)].clips;
    if (index < 0 || index >= static_cast<int>(from.size())) return -1;

    newStart = std::max<std::int64_t>(0, newStart);
    if (newTrack == track) {
        from[static_cast<std::size_t>(index)].start = newStart;
        syncTrack(track);
        return index;
    }

    ClipModel clip = std::move(from[static_cast<std::size_t>(index)]);
    from.erase(from.begin() + index);
    clip.start = newStart;
    auto& to = project_.tracks[static_cast<std::size_t>(newTrack)].clips;
    to.push_back(std::move(clip));
    syncTrack(track);
    syncTrack(newTrack);
    return static_cast<int>(to.size()) - 1;
}

bool ProjectController::removeClip(int track, int index) {
    if (!validTrack(track)) return false;
    auto& clips = project_.tracks[static_cast<std::size_t>(track)].clips;
    if (index < 0 || index >= static_cast<int>(clips.size())) return false;
    clips.erase(clips.begin() + index);
    syncTrack(track);
    return true;
}

bool ProjectController::splitClip(int track, int index, std::int64_t at) {
    if (!validTrack(track)) return false;
    auto& clips = project_.tracks[static_cast<std::size_t>(track)].clips;
    if (index < 0 || index >= static_cast<int>(clips.size())) return false;
    ClipModel& first = clips[static_cast<std::size_t>(index)];
    if (at <= first.start || at >= first.start + first.length) return false;

    ClipModel second = first;
    const std::int64_t cut = at - first.start;
    first.length = cut;
    second.start = at;
    second.offset += cut;
    second.length -= cut;
    clips.push_back(std::move(second));
    syncTrack(track);
    return true;
}

bool ProjectController::splitMidiClip(int track, int index, std::int64_t at) {
    if (!validTrack(track)) return false;
    auto& clips = project_.tracks[static_cast<std::size_t>(track)].midiClips;
    if (index < 0 || index >= static_cast<int>(clips.size())) return false;
    MidiClipModel& first = clips[static_cast<std::size_t>(index)];
    if (at <= first.start || at >= first.start + first.length) return false;

    const std::int64_t cut = at - first.start;
    MidiClipModel second;
    second.name = first.name;
    second.start = at;
    second.length = first.length - cut;
    std::vector<MidiNote> keep;
    for (MidiNote note : first.notes) {
        if (note.start >= cut) {
            note.start -= cut;  // gehoert zum hinteren Teil
            second.notes.push_back(note);
        } else {
            note.length = std::min(note.length, cut - note.start);  // endet am Schnitt
            keep.push_back(note);
        }
    }
    first.notes = std::move(keep);
    first.length = cut;
    clips.push_back(std::move(second));
    syncMidi(track);
    return true;
}

bool ProjectController::trimClip(int track, int index, std::int64_t newStart, std::int64_t newEnd) {
    if (!validTrack(track)) return false;
    auto& clips = project_.tracks[static_cast<std::size_t>(track)].clips;
    if (index < 0 || index >= static_cast<int>(clips.size())) return false;
    ClipModel& clip = clips[static_cast<std::size_t>(index)];

    // Wo laege der Dateianfang auf der Zeitachse? Das Material bewegt sich nicht.
    const std::int64_t fileStart = clip.start - clip.offset;
    const std::int64_t fileEnd = fileStart + clip.file->wav->frames();
    newStart = std::clamp(newStart, std::max<std::int64_t>(0, fileStart), fileEnd);
    newEnd = std::clamp(newEnd, newStart, fileEnd);
    if (newEnd - newStart < 1) return false;
    clip.start = newStart;
    clip.offset = newStart - fileStart;
    clip.length = newEnd - newStart;
    syncTrack(track);
    return true;
}

bool ProjectController::resizeMidiClip(int track, int index, std::int64_t newLength) {
    if (!validTrack(track) || newLength < 1) return false;
    auto& clips = project_.tracks[static_cast<std::size_t>(track)].midiClips;
    if (index < 0 || index >= static_cast<int>(clips.size())) return false;
    clips[static_cast<std::size_t>(index)].length = newLength;
    syncMidi(track);
    return true;
}

void ProjectController::setTrackGain(int track, float gain) {
    if (!validTrack(track)) return;
    modified_ = true;
    project_.tracks[static_cast<std::size_t>(track)].gain = std::max(0.0f, gain);
    engine_.setTrackGain(track, gain);
}

void ProjectController::setTrackPan(int track, float pan) {
    if (!validTrack(track)) return;
    pan = std::clamp(pan, -1.0f, 1.0f);
    modified_ = true;
    project_.tracks[static_cast<std::size_t>(track)].pan = pan;
    engine_.setTrackPan(track, pan);
}

void ProjectController::setTrackMute(int track, bool mute) {
    if (!validTrack(track)) return;
    modified_ = true;
    project_.tracks[static_cast<std::size_t>(track)].mute = mute;
    engine_.setTrackMute(track, mute);
}

void ProjectController::setTrackSolo(int track, bool solo) {
    if (!validTrack(track)) return;
    modified_ = true;
    project_.tracks[static_cast<std::size_t>(track)].solo = solo;
    engine_.setTrackSolo(track, solo);
}

void ProjectController::setMasterGain(float gain) {
    modified_ = true;
    project_.masterGain = std::max(0.0f, gain);
    engine_.setMasterGain(gain);
}

// --- Projektdatei ------------------------------------------------------------

std::int64_t ProjectController::contentLength() const noexcept {
    std::int64_t end = 0;
    for (const auto& track : project_.tracks) {
        for (const auto& clip : track.clips) end = std::max(end, clip.start + clip.length);
        for (const auto& clip : track.midiClips) end = std::max(end, clip.start + clip.length);
    }
    return end;
}

// Setzt alles auf den Ausgangszustand zurueck: keine Clips, keine Plugins, Regler neutral.
void ProjectController::clearProject() {
    engine_.stop();
    engine_.locate(0);
    // Alle Plugins entfernen.
    for (int strip = 0; strip < kProjectStrips; ++strip) {
        for (int s = 0; s < kSlotsPerStrip; ++s) {
            const SlotModel* info = slot(strip, s);
            if (!info || !info->used()) continue;
            std::string ignored;
            if (persistence_) persistence_->restore(strip, s, {}, {}, ignored);
            else setPlugin(strip, s, nullptr, L"");
        }
    }
    for (int b = 0; b < kProjectBuses; ++b) {
        project_.buses[static_cast<std::size_t>(b)].name = L"Bus " + std::to_wstring(b + 1);
        setBusGain(b, 1.0f);
        setBusPan(b, 0.0f);
        setBusMute(b, false);
    }
    for (int t = 0; t < kProjectTracks; ++t) {
        TrackModel& track = project_.tracks[static_cast<std::size_t>(t)];
        for (int b = 0; b < kProjectBuses; ++b) setTrackSend(t, b, 0.0f);
        setTrackOutput(t, -1);
        track.clips.clear();
        track.midiClips.clear();
        syncTrack(t);
        syncMidi(t);
        track.name = L"Spur " + std::to_wstring(t + 1);
        setTrackGain(t, 1.0f);
        setTrackPan(t, 0.0f);
        setTrackMute(t, false);
        setTrackSolo(t, false);
        setTrackInput(t, kTrackInputAudio);
        setTrackMidiOutput(t, kTrackOutputSynth);
    }
    setMasterGain(1.0f);
    setBpm(120.0);
}

void ProjectController::newProject() {
    if (isRecording()) return;
    clearProject();
    projectPath_.clear();
    modified_ = false;
}

bool ProjectController::saveProject(const std::filesystem::path& path, std::string& message) {
    message.clear();
    SavedProject saved;
    saved.sampleRate = sampleRate_;
    saved.bpm = project_.bpm;
    saved.masterGain = project_.masterGain;
    for (int t = 0; t < kProjectTracks; ++t) {
        const TrackModel& track = project_.tracks[static_cast<std::size_t>(t)];
        SavedTrack& out = saved.tracks[static_cast<std::size_t>(t)];
        out.name = track.name;
        out.gain = track.gain;
        out.pan = track.pan;
        out.mute = track.mute;
        out.solo = track.solo;
        out.input = track.input;
        out.midiOutput = track.midiOutput;
        out.sends = track.sends;
        out.output = track.output;
        for (const ClipModel& clip : track.clips)
            out.clips.push_back({clip.file->path, clip.start, clip.offset, clip.length});
        out.midiClips = track.midiClips;
    }
    // Plugins aller Kanalzuege mit ihren Einstellungen.
    for (int strip = 0; strip < kProjectStrips && persistence_; ++strip) {
        std::vector<SavedSlot>* list = strip < kProjectTracks ? &saved.tracks[static_cast<std::size_t>(strip)].slots
                                       : strip < kMasterStripIndex
                                           ? &saved.buses[static_cast<std::size_t>(strip - kFirstBusStrip)].slots
                                           : &saved.masterSlots;
        for (int s = 0; s < kSlotsPerStrip; ++s) {
            const SlotModel* info = slot(strip, s);
            if (!info || !info->used()) continue;
            SavedSlot entry;
            entry.slot = s;
            entry.name = info->name;
            if (persistence_->capture(strip, s, entry.file, entry.state)) list->push_back(std::move(entry));
        }
    }
    for (int b = 0; b < kProjectBuses; ++b) {
        const BusModel& bus = project_.buses[static_cast<std::size_t>(b)];
        SavedBus& out = saved.buses[static_cast<std::size_t>(b)];
        out.name = bus.name;
        out.gain = bus.gain;
        out.pan = bus.pan;
        out.mute = bus.mute;
    }
    if (!writeProjectFile(path, saved, message)) return false;
    projectPath_ = path;
    modified_ = false;
    return true;
}

bool ProjectController::loadProject(const std::filesystem::path& path, std::string& message) {
    message.clear();
    if (isRecording()) { message = "W\u00E4hrend einer Aufnahme l\u00E4sst sich kein Projekt laden."; return false; }

    // Erst vollstaendig lesen; schlaegt das fehl, bleibt das bisherige Projekt stehen.
    SavedProject saved;
    if (!readProjectFile(path, saved, message)) return false;

    clearProject();
    int missingFiles = 0, failedPlugins = 0;
    std::string firstProblem;

    setBpm(saved.bpm);
    setMasterGain(saved.masterGain);
    for (int t = 0; t < kProjectTracks; ++t) {
        const SavedTrack& in = saved.tracks[static_cast<std::size_t>(t)];
        TrackModel& track = project_.tracks[static_cast<std::size_t>(t)];
        track.name = in.name;
        setTrackGain(t, in.gain);
        setTrackPan(t, in.pan);
        setTrackMute(t, in.mute);
        setTrackSolo(t, in.solo);
        setTrackInput(t, in.input);
        setTrackMidiOutput(t, in.midiOutput);
        setTrackOutput(t, in.output);
        for (int b = 0; b < kProjectBuses; ++b) setTrackSend(t, b, in.sends[static_cast<std::size_t>(b)]);

        for (const SavedClip& savedClip : in.clips) {
            std::string error;
            auto file = openFile(savedClip.file, error);
            if (!file) {
                ++missingFiles;
                if (firstProblem.empty()) firstProblem = toUtf8(savedClip.file.filename().wstring()) + ": " + error;
                continue;
            }
            ClipModel clip;
            clip.start = std::max<std::int64_t>(0, savedClip.start);
            clip.offset = std::clamp<std::int64_t>(savedClip.offset, 0, file->wav->frames());
            clip.length = std::min(savedClip.length, file->wav->frames() - clip.offset);
            if (clip.length <= 0) continue;
            clip.file = std::move(file);
            track.clips.push_back(std::move(clip));
        }
        syncTrack(t);

        track.midiClips = in.midiClips;
        syncMidi(t);

    }

    // Plugins aller Kanalzuege.
    const auto restoreSlots = [&](int strip, const std::vector<SavedSlot>& slots) {
        for (const SavedSlot& entry : slots) {
            if (entry.file.empty()) continue;
            std::string error;
            if (!persistence_ || !persistence_->restore(strip, entry.slot, entry.file, entry.state, error)) {
                ++failedPlugins;
                if (firstProblem.empty())
                    firstProblem = toUtf8(entry.name) + ": " + (error.empty() ? "Plugins sind hier nicht verf\u00FCgbar" : error);
            }
        }
    };
    for (int t = 0; t < kProjectTracks; ++t) restoreSlots(t, saved.tracks[static_cast<std::size_t>(t)].slots);
    for (int b = 0; b < kProjectBuses; ++b) {
        const SavedBus& in = saved.buses[static_cast<std::size_t>(b)];
        if (!in.name.empty()) project_.buses[static_cast<std::size_t>(b)].name = in.name;
        setBusGain(b, in.gain);
        setBusPan(b, in.pan);
        setBusMute(b, in.mute);
        restoreSlots(kFirstBusStrip + b, in.slots);
    }
    restoreSlots(kMasterStripIndex, saved.masterSlots);

    projectPath_ = path;
    modified_ = false;

    if (missingFiles > 0 || failedPlugins > 0) {
        message = "Geladen mit L\u00FCcken: ";
        if (missingFiles > 0) message += std::to_string(missingFiles) + " Audiodatei(en) fehlen";
        if (missingFiles > 0 && failedPlugins > 0) message += ", ";
        if (failedPlugins > 0) message += std::to_string(failedPlugins) + " Plugin(s) nicht geladen";
        message += " (" + firstProblem + ")";
    } else if (saved.sampleRate > 0.0 && std::fabs(saved.sampleRate - sampleRate_) > 0.5) {
        message = "Das Projekt wurde mit " + std::to_string(static_cast<int>(saved.sampleRate)) +
                  " Hz gespeichert, das Ger\u00E4t l\u00E4uft mit " + std::to_string(static_cast<int>(sampleRate_)) +
                  " Hz. Positionen und Tonh\u00F6hen stimmen deshalb nicht.";
    }
    return true;
}

// --- MIDI --------------------------------------------------------------------

void ProjectController::setTrackInput(int track, int input) {
    if (!validTrack(track) || input < kTrackInputAudio || input > 15) return;
    if (isRecording()) return;
    modified_ = true;
    project_.tracks[static_cast<std::size_t>(track)].input = input;
    engine_.setTrackMidiInput(track, input);
}

void ProjectController::setTrackMidiOutput(int track, int output) {
    if (!validTrack(track) || output < kTrackOutputSynth || output > 15) return;
    modified_ = true;
    project_.tracks[static_cast<std::size_t>(track)].midiOutput = output;
    engine_.setTrackMidiOutput(track, output);
}

SlotArray* ProjectController::slotsOf(int strip) noexcept {
    if (strip >= 0 && strip < kProjectTracks) return &project_.tracks[static_cast<std::size_t>(strip)].slots;
    if (strip >= kFirstBusStrip && strip < kMasterStripIndex)
        return &project_.buses[static_cast<std::size_t>(strip - kFirstBusStrip)].slots;
    if (strip == kMasterStripIndex) return &project_.masterSlots;
    return nullptr;
}

const SlotModel* ProjectController::slot(int strip, int slot) const noexcept {
    if (slot < 0 || slot >= kSlotsPerStrip) return nullptr;
    const SlotArray* slots = const_cast<ProjectController*>(this)->slotsOf(strip);
    return slots ? &(*slots)[static_cast<std::size_t>(slot)] : nullptr;
}

void ProjectController::setPlugin(int strip, int slot, std::shared_ptr<AudioPlugin> plugin, std::wstring name) {
    SlotArray* slots = slotsOf(strip);
    if (!slots || slot < 0 || slot >= kSlotsPerStrip) return;
    if (!engine_.setPlugin(strip, slot, plugin.get())) return;
    modified_ = true;
    auto& owner = plugins_[static_cast<std::size_t>(strip)][static_cast<std::size_t>(slot)];
    if (owner) retiringPlugins_.push_back(std::move(owner));
    owner = std::move(plugin);
    SlotModel& model = (*slots)[static_cast<std::size_t>(slot)];
    model.name = owner ? std::move(name) : std::wstring();
    if (owner && model.name.empty()) model.name = L"Plugin";
    model.instrument = owner && owner->isInstrument();
}

void ProjectController::setTrackSend(int track, int bus, float level) {
    if (!validTrack(track) || bus < 0 || bus >= kProjectBuses) return;
    level = std::clamp(level, 0.0f, 4.0f);
    modified_ = true;
    project_.tracks[static_cast<std::size_t>(track)].sends[static_cast<std::size_t>(bus)] = level;
    engine_.setTrackSend(track, bus, level);
}

void ProjectController::setTrackOutput(int track, int bus) {
    if (!validTrack(track) || bus < -1 || bus >= kProjectBuses) return;
    modified_ = true;
    project_.tracks[static_cast<std::size_t>(track)].output = bus;
    engine_.setTrackOutput(track, bus);
}

void ProjectController::setBusGain(int bus, float gain) {
    if (bus < 0 || bus >= kProjectBuses) return;
    modified_ = true;
    project_.buses[static_cast<std::size_t>(bus)].gain = std::max(0.0f, gain);
    engine_.setBusGain(bus, gain);
}

void ProjectController::setBusPan(int bus, float pan) {
    if (bus < 0 || bus >= kProjectBuses) return;
    pan = std::clamp(pan, -1.0f, 1.0f);
    modified_ = true;
    project_.buses[static_cast<std::size_t>(bus)].pan = pan;
    engine_.setBusPan(bus, pan);
}

void ProjectController::setBusMute(int bus, bool mute) {
    if (bus < 0 || bus >= kProjectBuses) return;
    modified_ = true;
    project_.buses[static_cast<std::size_t>(bus)].mute = mute;
    engine_.setBusMute(bus, mute);
}

void ProjectController::releasePlugins() {
    for (int strip = 0; strip < kProjectStrips; ++strip)
        if (SlotArray* slots = slotsOf(strip))
            for (auto& slot : *slots) slot = {};
    for (auto& strip : plugins_)
        for (auto& owner : strip) owner.reset();
    retiringPlugins_.clear();
}

void ProjectController::setBpm(double bpm) {
    modified_ = true;
    project_.bpm = std::clamp(bpm, 30.0, 300.0);
    engine_.setTempo(project_.bpm);
}

void ProjectController::setMetronome(bool on) {
    metronome_ = on;
    engine_.setMetronome(on);
}

bool ProjectController::updateMidiClip(int track, int index, std::vector<MidiNote> notes, std::int64_t length) {
    if (!validTrack(track) || length <= 0) return false;
    auto& clips = project_.tracks[static_cast<std::size_t>(track)].midiClips;
    if (index < 0 || index >= static_cast<int>(clips.size())) return false;
    std::sort(notes.begin(), notes.end(), [](const MidiNote& a, const MidiNote& b) { return a.start < b.start; });
    clips[static_cast<std::size_t>(index)].notes = std::move(notes);
    clips[static_cast<std::size_t>(index)].length = length;
    syncMidi(track);
    return true;
}

int ProjectController::addMidiClip(int track, MidiClipModel clip) {
    if (!validTrack(track) || clip.length <= 0) return -1;
    clip.start = std::max<std::int64_t>(0, clip.start);
    auto& clips = project_.tracks[static_cast<std::size_t>(track)].midiClips;
    clips.push_back(std::move(clip));
    syncMidi(track);
    return static_cast<int>(clips.size()) - 1;
}

int ProjectController::moveMidiClip(int track, int index, int newTrack, std::int64_t newStart) {
    if (!validTrack(track) || !validTrack(newTrack)) return -1;
    auto& from = project_.tracks[static_cast<std::size_t>(track)].midiClips;
    if (index < 0 || index >= static_cast<int>(from.size())) return -1;

    newStart = std::max<std::int64_t>(0, newStart);
    if (newTrack == track) {
        from[static_cast<std::size_t>(index)].start = newStart;
        syncMidi(track);
        return index;
    }
    MidiClipModel clip = std::move(from[static_cast<std::size_t>(index)]);
    from.erase(from.begin() + index);
    clip.start = newStart;
    auto& to = project_.tracks[static_cast<std::size_t>(newTrack)].midiClips;
    to.push_back(std::move(clip));
    syncMidi(track);
    syncMidi(newTrack);
    return static_cast<int>(to.size()) - 1;
}

bool ProjectController::removeMidiClip(int track, int index) {
    if (!validTrack(track)) return false;
    auto& clips = project_.tracks[static_cast<std::size_t>(track)].midiClips;
    if (index < 0 || index >= static_cast<int>(clips.size())) return false;
    clips.erase(clips.begin() + index);
    syncMidi(track);
    return true;
}

// Baut aus allen MIDI-Clips der Spur die sortierte Ereignisliste fuer die Engine.
void ProjectController::syncMidi(int track) {
    modified_ = true;
    const auto& clips = project_.tracks[static_cast<std::size_t>(track)].midiClips;
    if (clips.empty()) {
        engine_.setTrackMidiSequence(track, nullptr);
        return;
    }
    auto sequence = std::make_unique<MidiSequence>();
    for (const auto& clip : clips) {
        for (const auto& note : clip.notes) {
            if (note.start < 0 || note.start >= clip.length || note.length <= 0) continue;
            // Noten enden spaetestens am Clip-Ende.
            const std::int64_t on = clip.start + note.start;
            const std::int64_t off = clip.start + std::min(note.start + note.length, clip.length);
            sequence->events.push_back({on, MidiMessage::noteOn(note.pitch, std::max<int>(1, note.velocity))});
            sequence->events.push_back({off, MidiMessage::noteOff(note.pitch)});
        }
    }
    // Nach Zeit; bei gleicher Zeit erst "Note aus", damit Tonwiederholungen neu anschlagen.
    std::stable_sort(sequence->events.begin(), sequence->events.end(),
                     [](const MidiSequence::Event& a, const MidiSequence::Event& b) {
                         if (a.frame != b.frame) return a.frame < b.frame;
                         return a.message.isNoteOff() && !b.message.isNoteOff();
                     });
    engine_.setTrackMidiSequence(track, std::move(sequence));
}

// Schliesst die MIDI-Aufnahme ab: offene Noten beenden, Clip anlegen.
void ProjectController::finishMidiRecording(std::int64_t end) {
    const int track = recordTrack_;
    midiRecording_ = false;
    recordTrack_ = -1;
    if (!midiRecordBegun_) return;
    midiRecordBegun_ = false;

    const std::int64_t length = std::max<std::int64_t>(1, end - midiRecordStart_);
    for (int pitch = 0; pitch < 128; ++pitch) {
        OpenNote& open = midiRecordOpen_[static_cast<std::size_t>(pitch)];
        if (!open.open) continue;
        midiRecordNotes_.push_back({open.start, std::max<std::int64_t>(1, length - open.start),
                                    static_cast<std::uint8_t>(pitch), open.velocity});
        open.open = false;
    }
    if (midiRecordNotes_.empty() || !validTrack(track)) return;

    MidiClipModel clip;
    clip.name = L"MIDI";
    clip.start = midiRecordStart_;
    clip.length = length;
    clip.notes = std::move(midiRecordNotes_);
    midiRecordNotes_.clear();
    std::sort(clip.notes.begin(), clip.notes.end(),
              [](const MidiNote& a, const MidiNote& b) { return a.start < b.start; });
    addMidiClip(track, std::move(clip));
}

// --- Aufnahme ----------------------------------------------------------------

std::int64_t ProjectController::estimatedLatencyFrames() const {
    return latencyProvider_ ? std::max<std::int64_t>(0, latencyProvider_()) : 0;
}

void ProjectController::applyMeasuredLatency(double frames) {
    const double ms = (frames - static_cast<double>(estimatedLatencyFrames())) * 1000.0 / sampleRate_;
    setRecordOffsetMs(static_cast<int>(std::lround(ms)));
}

std::int64_t ProjectController::appliedLatencyFrames() const {
    const auto offset = static_cast<std::int64_t>(recordOffsetMs_ * sampleRate_ / 1000.0);
    return std::max<std::int64_t>(0, estimatedLatencyFrames() + offset);
}

void ProjectController::armTrack(int track) {
    if (track != -1 && !validTrack(track)) return;
    if (recordSink_) return;  // waehrend der Aufnahme nicht umschalten
    armedTrack_ = track;
    engine_.setArmedTrack(track);
}

void ProjectController::setMonitor(bool on) {
    monitor_ = on;
    engine_.setInputMonitor(on);
}

bool ProjectController::startRecording(std::string& message) {
    message.clear();
    if (isRecording()) { message = "Es l\u00E4uft bereits eine Aufnahme."; return false; }
    if (!validTrack(armedTrack_)) {
        message = "Keine Spur scharfgeschaltet. Erst R an einer Spur anklicken.";
        return false;
    }

    // Spur mit MIDI-Eingang: Es werden Noten aufgenommen, keine Audiodatei.
    if (project_.tracks[static_cast<std::size_t>(armedTrack_)].input != kTrackInputAudio) {
        MidiRecordEvent stale;
        while (engine_.popMidiRecord(stale)) {}
        midiRecordNotes_.clear();
        midiRecordOpen_ = {};
        midiRecordBegun_ = false;
        if (!engine_.recordMidi()) { message = "Aufnahme lie\u00DF sich nicht starten."; return false; }
        midiRecording_ = true;
        recordTrack_ = armedTrack_;
        return true;
    }

    if (inputChannels_ < 1) { message = "Kein Audio-Eingang verf\u00FCgbar."; return false; }
    if (recordingFolder_.empty()) { message = "Kein Aufnahmeordner festgelegt."; return false; }

    std::error_code ec;
    std::filesystem::create_directories(recordingFolder_, ec);
    if (ec) { message = "Aufnahmeordner l\u00E4sst sich nicht anlegen."; return false; }

    // Dateiname aus Datum und Uhrzeit; bei Namensgleichheit hochzaehlen.
    const std::time_t now = std::time(nullptr);
    std::tm local{};
#ifdef _WIN32
    localtime_s(&local, &now);
#else
    localtime_r(&now, &local);
#endif
    char stamp[64];
    std::strftime(stamp, sizeof stamp, "%Y-%m-%d_%H-%M-%S", &local);
    std::filesystem::path path;
    for (int n = 0;; ++n) {
        std::string name = std::string("Aufnahme_") + stamp;
        if (n > 0) name += "_" + std::to_string(n);
        path = recordingFolder_ / (name + ".wav");
        if (!std::filesystem::exists(path, ec)) break;
    }

    std::string error;
    auto sink = RecordSink::create(path, inputChannels_, static_cast<int>(sampleRate_ + 0.5), error);
    if (!sink) { message = "Aufnahmedatei: " + error; return false; }

    recordSink_ = sink;
    recordTrack_ = armedTrack_;
    if (!engine_.record(std::move(sink))) {
        recordSink_.reset();
        recordTrack_ = -1;
        std::filesystem::remove(path, ec);
        message = "Aufnahme lie\u00DF sich nicht starten.";
        return false;
    }
    return true;
}

bool ProjectController::recordProgress(std::int64_t& start, std::int64_t& frames) const noexcept {
    if (midiRecording_) {
        if (!midiRecordBegun_) return false;
        start = midiRecordStart_;
        frames = std::max<std::int64_t>(0, engine_.position() - midiRecordStart_);
        return true;
    }
    if (!recordSink_ || !recordSink_->started()) return false;
    start = recordSink_->startPosition();
    frames = recordSink_->framesCaptured();
    return true;
}

bool ProjectController::poll(std::string& message) {
    message.clear();
    engine_.collectGarbage();

    // Abgeloeste Plugins freigeben, sobald der Audio-Thread sie zurueckmeldet.
    AudioPlugin* released = nullptr;
    while (engine_.popReleasedPlugin(released)) {
        retiringPlugins_.erase(std::remove_if(retiringPlugins_.begin(), retiringPlugins_.end(),
                                              [released](const auto& p) { return p.get() == released; }),
                               retiringPlugins_.end());
    }

    // MIDI-Aufnahme: Ereignisse abholen, Noten zusammensetzen, am Ende den Clip anlegen.
    if (midiRecording_) {
        MidiRecordEvent e;
        while (midiRecording_ && engine_.popMidiRecord(e)) {
            if (e.kind == MidiRecordEvent::Kind::Begin) {
                midiRecordBegun_ = true;
                midiRecordStart_ = e.position;
            } else if (e.kind == MidiRecordEvent::Kind::End) {
                const std::size_t before = validTrack(recordTrack_)
                    ? project_.tracks[static_cast<std::size_t>(recordTrack_)].midiClips.size() : 0;
                const int track = recordTrack_;
                finishMidiRecording(e.position);
                return validTrack(track) &&
                       project_.tracks[static_cast<std::size_t>(track)].midiClips.size() != before;
            } else if (midiRecordBegun_) {
                const std::int64_t at = std::max<std::int64_t>(0, e.position - midiRecordStart_);
                OpenNote& open = midiRecordOpen_[e.message.data1];
                if (e.message.isNoteOn() || e.message.isNoteOff()) {
                    if (open.open) {  // vorherige Note auf dieser Taste abschliessen
                        midiRecordNotes_.push_back({open.start, std::max<std::int64_t>(1, at - open.start),
                                                    e.message.data1, open.velocity});
                        open.open = false;
                    }
                    if (e.message.isNoteOn()) open = {true, at, e.message.data2};
                }
            }
        }
        return false;
    }

    if (!recordSink_ || !recordSink_->done()) return false;

    // Die Datei ist fertig geschrieben: Aufnahme abschliessen und Clip anlegen.
    const std::shared_ptr<RecordSink> sink = std::move(recordSink_);
    const int track = recordTrack_;
    recordSink_.reset();
    recordTrack_ = -1;
    engine_.releaseRecordSink();

    std::error_code ec;
    if (!sink->started() || sink->framesCaptured() <= 0) {
        std::filesystem::remove(sink->path(), ec);
        return false;
    }

    std::string error;
    auto file = openFile(sink->path(), error);
    if (!file) { message = "Aufnahme nicht lesbar: " + error; return false; }

    // Um die Latenz nach vorn schieben: Was aufgenommen wurde, gehoert zu dem,
    // was der Spieler in dem Moment gehoert hat.
    const std::int64_t latency = appliedLatencyFrames();
    ClipModel clip;
    clip.start = sink->startPosition() - latency;
    clip.offset = 0;
    clip.length = file->wav->frames();
    if (clip.start < 0) {
        // Der Anfang laege vor dem Nullpunkt: vorn entsprechend abschneiden.
        clip.offset = -clip.start;
        clip.length -= clip.offset;
        clip.start = 0;
    }
    if (clip.length <= 0) return false;
    clip.file = std::move(file);
    project_.tracks[static_cast<std::size_t>(track)].clips.push_back(std::move(clip));
    syncTrack(track);

    if (sink->overruns() > 0)
        message = "Aufnahme mit L\u00FCcken: Die Platte kam nicht nach.";
    return true;
}

void ProjectController::finishRecordingOnShutdown() {
    midiRecording_ = false;  // eine laufende MIDI-Aufnahme wird verworfen
    if (!recordSink_) return;
    recordSink_->finish();  // der Audio-Thread laeuft nicht mehr, also darf das hier passieren
    std::string message;
    for (int i = 0; i < 600 && recordSink_; ++i) {
        poll(message);
        if (recordSink_) std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
}

std::int64_t ProjectController::projectLength() const noexcept {
    std::int64_t end = 0;
    for (const auto& track : project_.tracks)
        for (const auto& clip : track.clips) end = std::max(end, clip.start + clip.length);
    return end;
}

void ProjectController::syncTrack(int track) {
    modified_ = true;
    Playlist playlist;
    for (const auto& clip : project_.tracks[static_cast<std::size_t>(track)].clips) {
        // Blenden nur dort, wo mitten im Material geschnitten ist, nicht an den Enden der Datei.
        const std::int64_t fade = std::min(kCutFade, clip.length / 2);
        const std::int64_t fadeIn = clip.offset > 0 ? fade : 0;
        const std::int64_t fadeOut = clip.offset + clip.length < clip.file->wav->frames() ? fade : 0;
        playlist.push_back(Clip{clip.file->wav, clip.start, clip.offset, clip.length, 1.0f, fadeIn, fadeOut});
    }
    engine_.setTrackPlaylist(track, std::move(playlist));
}
