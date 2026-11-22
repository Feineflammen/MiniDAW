#pragma once

#include "engine/BuiltinEffect.h"
#include "model/ProjectController.h"
#include "ui/EffectWindow.h"
#include "ui/PluginHost.h"
#include "vst3/PluginWindow.h"
#include "vst3/Vst3Host.h"
#include "vst3/Vst3Module.h"

#include <windows.h>

#include <array>
#include <functional>
#include <memory>
#include <string>
#include <vector>

namespace vst3 {

// Verbindet Oberflaeche, Projekt und VST3-Host: sucht Plugins, laedt sie auf
// Spuren und verwaltet ihre Fenster. Alles hier laeuft im UI-Thread.
class PluginManager final : public ui::PluginHost, public PluginPersistence {
public:
    PluginManager(ProjectController& controller, double sampleRate, int maxFrames)
        : controller_(controller), sampleRate_(sampleRate), maxFrames_(maxFrames) {}
    ~PluginManager() override { shutdown(); }

    // Neue Samplerate oder Blocklaenge (anderes Audiogeraet): Alle geladenen
    // Plugins werden mit ihren Einstellungen neu geladen. Nur rufen, solange
    // der Audio-Thread steht. Liefert die Zahl der Plugins, die das nicht ueberstanden.
    int setAudioFormat(double sampleRate, int maxFrames);

    // Hauptfenster, dem die Plugin-Fenster zugeordnet werden.
    void setOwner(HWND owner) { owner_ = owner; }

    // Etwa 30-mal pro Sekunde rufen: haelt die Plugin-Oberflaechen aktuell.
    void idle();

    // Schliesst alle Fenster und gibt die eigenen Verweise auf die Plugins frei.
    // Zerstoert werden sie erst, wenn auch der ProjectController sie loslaesst.
    void shutdown();

    // --- ui::PluginHost ---
    std::vector<std::wstring> pluginNames() override;
    std::wstring pluginFolder() override { return vst3::pluginFolder().wstring(); }
    bool load(int strip, int slot, int index, std::wstring& error) override;
    void unload(int strip, int slot) override;
    bool openEditor(int strip, int slot, std::wstring& error) override;
    std::vector<std::wstring> builtinNames() override;
    bool loadBuiltin(int strip, int slot, int index, std::wstring& error) override;

    // Wird gerufen, wenn der Benutzer im Fenster eines eingebauten Effekts etwas aendert.
    std::function<void()> onBuiltinChange;

    // --- PluginPersistence (Projekt speichern und laden) ---
    bool capture(int strip, int slot, std::filesystem::path& file, std::vector<char>& state) override;
    bool restore(int strip, int slot, const std::filesystem::path& file, const std::vector<char>& state,
                 std::string& error) override;

private:
    // Eingebaute Effekte stehen in der Projektdatei als "<MiniDAW>/id". Das
    // Zeichen < kommt in Windows-Dateinamen nicht vor, eine Verwechslung ist ausgeschlossen.
    static constexpr const wchar_t* kBuiltinPrefix = L"<MiniDAW>/";

    struct Slot {
        std::unique_ptr<PluginWindow> window;  // vor dem Plugin zerstoeren
        std::shared_ptr<Vst3Plugin> plugin;
        std::shared_ptr<fx::BuiltinEffect> builtin;          // statt plugin, wenn eingebaut
        std::unique_ptr<ui::EffectWindow> builtinWindow;     // nach dem Effekt: wird vor ihm zerstoert
        std::filesystem::path path;            // Datei, aus der das Plugin stammt
        void clear() {
            window.reset();
            builtinWindow.reset();
            plugin.reset();
            builtin.reset();
            path.clear();
        }
    };
    Slot* find(int strip, int slot);
    void install(int strip, int slot, std::shared_ptr<Vst3Plugin> plugin, const std::filesystem::path& path);
    void installBuiltin(int strip, int slot, std::shared_ptr<fx::BuiltinEffect> effect);

    ProjectController& controller_;
    double sampleRate_;
    int maxFrames_;
    HWND owner_ = nullptr;
    std::vector<PluginEntry> entries_;
    std::array<std::array<Slot, kSlotsPerStrip>, kProjectStrips> slots_;
};

}  // namespace vst3
