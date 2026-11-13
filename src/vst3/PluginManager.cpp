#include "vst3/PluginManager.h"

#include <algorithm>

namespace vst3 {

namespace {

// Die Meldungen des Hosts sind reines ASCII.
std::wstring widen(const std::string& text) { return std::wstring(text.begin(), text.end()); }

bool validTrack(int track) { return track >= 0 && track < kProjectTracks; }

}  // namespace

int PluginManager::setAudioFormat(double sampleRate, int maxFrames) {
    if (sampleRate == sampleRate_ && maxFrames <= maxFrames_) return 0;
    sampleRate_ = sampleRate;
    maxFrames_ = std::max(maxFrames, maxFrames_);
    int failed = 0;
    for (int t = 0; t < kProjectTracks; ++t) {
        std::filesystem::path file;
        std::vector<char> state;
        if (!capture(t, file, state)) continue;
        std::string error;
        if (!restore(t, file, state, error)) {
            unload(t);
            ++failed;
        }
    }
    return failed;
}

void PluginManager::idle() {
    for (auto& slot : slots_)
        if (slot.plugin) slot.plugin->idle();
}

void PluginManager::shutdown() {
    for (auto& slot : slots_) {
        slot.window.reset();
        slot.plugin.reset();
    }
}

std::vector<std::wstring> PluginManager::pluginNames() {
    entries_ = scanPlugins();
    std::vector<std::wstring> names;
    names.reserve(entries_.size());
    for (const auto& entry : entries_) names.push_back(entry.name);
    return names;
}

bool PluginManager::load(int track, int index, std::wstring& error) {
    if (!validTrack(track) || index < 0 || index >= static_cast<int>(entries_.size())) {
        error = L"Ungültige Auswahl";
        return false;
    }

    std::string reason;
    std::shared_ptr<Vst3Plugin> plugin = loadPlugin(entries_[static_cast<std::size_t>(index)].path,
                                                    sampleRate_, maxFrames_, reason);
    if (!plugin) {
        error = entries_[static_cast<std::size_t>(index)].name + L": " + widen(reason);
        return false;
    }

    install(track, std::move(plugin), entries_[static_cast<std::size_t>(index)].path);
    return true;
}

// Setzt ein fertig geladenes Plugin auf die Spur.
void PluginManager::install(int track, std::shared_ptr<Vst3Plugin> plugin, const std::filesystem::path& path) {
    // Erst das Fenster des bisherigen Plugins schliessen, dann austauschen. Das
    // alte Plugin haelt der ProjectController, bis der Audio-Thread es loslaesst.
    Slot& slot = slots_[static_cast<std::size_t>(track)];
    slot.window.reset();
    const std::wstring name = plugin->name();
    controller_.setTrackPlugin(track, plugin, name);
    slot.plugin = std::move(plugin);
    slot.path = path;
}

bool PluginManager::capture(int track, std::filesystem::path& file, std::vector<char>& state) {
    if (!validTrack(track)) return false;
    Slot& slot = slots_[static_cast<std::size_t>(track)];
    if (!slot.plugin) return false;
    file = slot.path;
    slot.plugin->saveState(state);  // liefert das Plugin nichts, wird es ohne Einstellungen gespeichert
    return true;
}

bool PluginManager::restore(int track, const std::filesystem::path& file, const std::vector<char>& state,
                            std::string& error) {
    if (!validTrack(track)) return false;
    if (file.empty()) {
        unload(track);
        return true;
    }
    std::shared_ptr<Vst3Plugin> plugin = loadPlugin(file, sampleRate_, maxFrames_, error);
    if (!plugin) return false;
    // Einstellungen setzen, bevor der Audio-Thread das Plugin bekommt.
    if (!state.empty()) plugin->loadState(state);
    install(track, std::move(plugin), file);
    return true;
}

void PluginManager::unload(int track) {
    if (!validTrack(track)) return;
    Slot& slot = slots_[static_cast<std::size_t>(track)];
    slot.window.reset();
    controller_.setTrackPlugin(track, nullptr, L"");
    slot.plugin.reset();
    slot.path.clear();
}

bool PluginManager::openEditor(int track, std::wstring& error) {
    if (!validTrack(track)) return false;
    Slot& slot = slots_[static_cast<std::size_t>(track)];
    if (!slot.plugin) {
        error = L"Auf dieser Spur ist kein Plugin geladen.";
        return false;
    }
    if (!slot.window) slot.window = std::make_unique<PluginWindow>(*slot.plugin, owner_);
    std::string reason;
    if (!slot.window->open(reason)) {
        error = widen(reason);
        return false;
    }
    return true;
}

}  // namespace vst3
