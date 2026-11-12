#include "model/ProjectController.h"

#include <algorithm>
#include <cmath>
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
