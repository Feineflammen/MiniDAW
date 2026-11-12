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

void ProjectController::setTrackGain(int track, float gain) {
    if (!validTrack(track)) return;
    project_.tracks[static_cast<std::size_t>(track)].gain = std::max(0.0f, gain);
    engine_.setTrackGain(track, gain);
}

void ProjectController::setTrackPan(int track, float pan) {
    if (!validTrack(track)) return;
    pan = std::clamp(pan, -1.0f, 1.0f);
    project_.tracks[static_cast<std::size_t>(track)].pan = pan;
    engine_.setTrackPan(track, pan);
}

void ProjectController::setTrackMute(int track, bool mute) {
    if (!validTrack(track)) return;
    project_.tracks[static_cast<std::size_t>(track)].mute = mute;
    engine_.setTrackMute(track, mute);
}

void ProjectController::setTrackSolo(int track, bool solo) {
    if (!validTrack(track)) return;
    project_.tracks[static_cast<std::size_t>(track)].solo = solo;
    engine_.setTrackSolo(track, solo);
}

void ProjectController::setMasterGain(float gain) {
    project_.masterGain = std::max(0.0f, gain);
    engine_.setMasterGain(gain);
}

// --- Aufnahme ----------------------------------------------------------------

std::int64_t ProjectController::estimatedLatencyFrames() const {
    return latencyProvider_ ? std::max<std::int64_t>(0, latencyProvider_()) : 0;
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
    if (recordSink_) { message = "Es l\u00E4uft bereits eine Aufnahme."; return false; }
    if (inputChannels_ < 1) { message = "Kein Audio-Eingang verf\u00FCgbar."; return false; }
    if (!validTrack(armedTrack_)) {
        message = "Keine Spur scharfgeschaltet. Erst R an einer Spur anklicken.";
        return false;
    }
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
    if (!recordSink_ || !recordSink_->started()) return false;
    start = recordSink_->startPosition();
    frames = recordSink_->framesCaptured();
    return true;
}

bool ProjectController::poll(std::string& message) {
    message.clear();
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
    Playlist playlist;
    for (const auto& clip : project_.tracks[static_cast<std::size_t>(track)].clips)
        playlist.push_back(Clip{clip.file->wav, clip.start, clip.offset, clip.length, 1.0f});
    engine_.setTrackPlaylist(track, std::move(playlist));
}
