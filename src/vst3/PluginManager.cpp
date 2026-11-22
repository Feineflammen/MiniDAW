#include "vst3/PluginManager.h"

#include <algorithm>

namespace vst3 {

namespace {

// Die Meldungen des Hosts sind reines ASCII.
std::wstring widen(const std::string& text) { return std::wstring(text.begin(), text.end()); }

}  // namespace

// Platz 0 (Instrument) gibt es nur auf Spuren.
PluginManager::Slot* PluginManager::find(int strip, int slot) {
    if (strip < 0 || strip >= kProjectStrips || slot < 0 || slot >= kSlotsPerStrip) return nullptr;
    if (slot == 0 && strip >= kProjectTracks) return nullptr;
    return &slots_[static_cast<std::size_t>(strip)][static_cast<std::size_t>(slot)];
}

int PluginManager::setAudioFormat(double sampleRate, int maxFrames) {
    if (sampleRate == sampleRate_ && maxFrames <= maxFrames_) return 0;
    sampleRate_ = sampleRate;
    maxFrames_ = std::max(maxFrames, maxFrames_);
    int failed = 0;
    for (int strip = 0; strip < kProjectStrips; ++strip) {
        for (int s = 0; s < kSlotsPerStrip; ++s) {
            std::filesystem::path file;
            std::vector<char> state;
            if (!capture(strip, s, file, state)) continue;
            std::string error;
            if (!restore(strip, s, file, state, error)) {
                unload(strip, s);
                ++failed;
            }
        }
    }
    return failed;
}

void PluginManager::idle() {
    for (auto& strip : slots_)
        for (auto& slot : strip)
            if (slot.plugin) slot.plugin->idle();
}

void PluginManager::shutdown() {
    for (auto& strip : slots_)
        for (auto& slot : strip) slot.clear();
}

std::vector<std::wstring> PluginManager::pluginNames() {
    entries_ = scanPlugins();
    std::vector<std::wstring> names;
    names.reserve(entries_.size());
    for (const auto& entry : entries_) names.push_back(entry.name);
    return names;
}

bool PluginManager::load(int strip, int slot, int index, std::wstring& error) {
    if (!find(strip, slot) || index < 0 || index >= static_cast<int>(entries_.size())) {
        error = L"Ung\u00FCltige Auswahl";
        return false;
    }
    const PluginEntry& entry = entries_[static_cast<std::size_t>(index)];
    std::string reason;
    std::shared_ptr<Vst3Plugin> plugin = loadPlugin(entry.path, sampleRate_, maxFrames_, reason);
    if (!plugin) {
        error = entry.name + L": " + widen(reason);
        return false;
    }
    install(strip, slot, std::move(plugin), entry.path);
    return true;
}

// Setzt ein fertig geladenes Plugin auf den Platz.
void PluginManager::install(int strip, int slot, std::shared_ptr<Vst3Plugin> plugin,
                            const std::filesystem::path& path) {
    // Erst das Fenster des bisherigen Plugins schliessen, dann austauschen. Das
    // alte Plugin haelt der ProjectController, bis der Audio-Thread es loslaesst.
    Slot* target = find(strip, slot);
    if (!target) return;
    target->window.reset();
    target->masteringWindow.reset();
    const std::wstring name = plugin->name();
    controller_.setPlugin(strip, slot, plugin, name);
    target->mastering.reset();
    target->plugin = std::move(plugin);
    target->path = path;
}

void PluginManager::installMastering(int strip, int slot, std::shared_ptr<mastering::MasteringEffect> effect) {
    Slot* target = find(strip, slot);
    if (!target) return;
    target->window.reset();
    target->masteringWindow.reset();
    controller_.setPlugin(strip, slot, effect, kMasteringName);
    target->plugin.reset();
    target->mastering = std::move(effect);
    target->path = kMasteringPath;
}

bool PluginManager::loadBuiltin(int strip, int slot, int index, std::wstring& error) {
    if (index != 0 || !find(strip, slot) || slot == 0) {
        error = L"Der Mastering-Effekt geh\u00F6rt auf einen Effekt-Platz.";
        return false;
    }
    installMastering(strip, slot, std::make_shared<mastering::MasteringEffect>(sampleRate_));
    return true;
}

bool PluginManager::capture(int strip, int slot, std::filesystem::path& file, std::vector<char>& state) {
    Slot* target = find(strip, slot);
    if (!target) return false;
    if (target->mastering) {
        file = kMasteringPath;
        state = target->mastering->saveState();
        return true;
    }
    if (!target->plugin) return false;
    file = target->path;
    target->plugin->saveState(state);  // liefert das Plugin nichts, wird es ohne Einstellungen gespeichert
    return true;
}

bool PluginManager::restore(int strip, int slot, const std::filesystem::path& file, const std::vector<char>& state,
                            std::string& error) {
    if (file.empty()) {
        unload(strip, slot);
        return true;
    }
    if (file == std::filesystem::path(kMasteringPath)) {
        if (slot < 0) slot = 1;
        if (!find(strip, slot) || slot == 0) {
            error = "Ungueltiger Plugin-Platz";
            return false;
        }
        auto effect = std::make_shared<mastering::MasteringEffect>(sampleRate_);
        if (!state.empty()) effect->loadState(state);
        installMastering(strip, slot, std::move(effect));
        return true;
    }
    std::shared_ptr<Vst3Plugin> plugin = loadPlugin(file, sampleRate_, maxFrames_, error);
    if (!plugin) return false;
    // Alte Projektdateien kennen keine Plaetze: Instrumente auf 0, Effekte auf 1.
    if (slot < 0) slot = (plugin->isInstrument() && strip < kProjectTracks) ? 0 : 1;
    if (!find(strip, slot)) {
        error = "Ungueltiger Plugin-Platz";
        return false;
    }
    // Einstellungen setzen, bevor der Audio-Thread das Plugin bekommt.
    if (!state.empty()) plugin->loadState(state);
    install(strip, slot, std::move(plugin), file);
    return true;
}

void PluginManager::unload(int strip, int slot) {
    Slot* target = find(strip, slot);
    if (!target) return;
    target->window.reset();
    target->masteringWindow.reset();
    controller_.setPlugin(strip, slot, nullptr, L"");
    target->clear();
}

bool PluginManager::openEditor(int strip, int slot, std::wstring& error) {
    Slot* target = find(strip, slot);
    if (!target) return false;
    if (target->mastering) {
        if (!target->masteringWindow) {
            std::wstring title = L"Mastering \u2013 ";
            if (strip < kProjectTracks) title += L"Spur " + std::to_wstring(strip + 1);
            else if (strip < kMasterStripIndex) title += L"Bus " + std::to_wstring(strip - kFirstBusStrip + 1);
            else title += L"Master";
            target->masteringWindow = std::make_unique<ui::MasteringWindow>(*target->mastering, owner_, title);
            target->masteringWindow->onChange = [this] {
                if (onBuiltinChange) onBuiltinChange();
            };
        }
        std::string reason;
        if (!target->masteringWindow->open(reason)) {
            error = widen(reason);
            return false;
        }
        return true;
    }
    if (!target->plugin) {
        error = L"Auf diesem Platz ist kein Plugin geladen.";
        return false;
    }
    if (!target->window) target->window = std::make_unique<PluginWindow>(*target->plugin, owner_);
    std::string reason;
    if (!target->window->open(reason)) {
        error = widen(reason);
        return false;
    }
    return true;
}

}  // namespace vst3
