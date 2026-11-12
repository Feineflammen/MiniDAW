#include "vst3/PluginManager.h"

namespace vst3 {

namespace {

// Die Meldungen des Hosts sind reines ASCII.
std::wstring widen(const std::string& text) { return std::wstring(text.begin(), text.end()); }

bool validTrack(int track) { return track >= 0 && track < kProjectTracks; }

}  // namespace

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

    // Erst das Fenster des bisherigen Plugins schliessen, dann austauschen. Das
    // alte Plugin haelt der ProjectController, bis der Audio-Thread es loslaesst.
    Slot& slot = slots_[static_cast<std::size_t>(track)];
    slot.window.reset();
    const std::wstring name = plugin->name();
    controller_.setTrackPlugin(track, plugin, name);
    slot.plugin = std::move(plugin);
    return true;
}

void PluginManager::unload(int track) {
    if (!validTrack(track)) return;
    Slot& slot = slots_[static_cast<std::size_t>(track)];
    slot.window.reset();
    controller_.setTrackPlugin(track, nullptr, L"");
    slot.plugin.reset();
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
