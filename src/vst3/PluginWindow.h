#pragma once

#include "vst3/Vst3Host.h"

#include <windows.h>

#include <functional>
#include <string>

namespace vst3 {

// Eigenes Fenster fuer die Bedienoberflaeche eines Plugins. Oben eine schmale
// Leiste fuer die MIDI-Zuweisung, darunter die Oberflaeche des Plugins in einem
// eigenen Unterfenster.
class PluginWindow final : public Steinberg::IPlugFrame {
public:
    PluginWindow(Vst3Plugin& plugin, HWND owner) : plugin_(plugin), owner_(owner) {}
    ~PluginWindow() { close(); }
    PluginWindow(const PluginWindow&) = delete;
    PluginWindow& operator=(const PluginWindow&) = delete;

    // Oeffnet das Fenster oder holt es nach vorn. false, wenn das Plugin keine Oberflaeche hat.
    bool open(std::string& error);
    void close();
    bool isOpen() const { return hwnd_ != nullptr; }

    // Wird gerufen, wenn eine MIDI-Zuweisung entsteht oder geloescht wird.
    std::function<void()> onChange;

    // --- IPlugFrame: Das Plugin moechte eine andere Fenstergroesse ---
    Steinberg::tresult PLUGIN_API queryInterface(const Steinberg::TUID interfaceId, void** obj) override;
    Steinberg::uint32 PLUGIN_API addRef() override { return 1; }
    Steinberg::uint32 PLUGIN_API release() override { return 1; }
    Steinberg::tresult PLUGIN_API resizeView(Steinberg::IPlugView* view, Steinberg::ViewRect* newSize) override;

private:
    static LRESULT CALLBACK windowProc(HWND hwnd, UINT message, WPARAM wParam, LPARAM lParam);
    void setClientSize(int width, int height);  // Groesse der Plugin-Oberflaeche
    int barHeight() const;
    void paintBar(HDC dc);
    void click(int x, int y);
    void layoutHost();
    std::wstring statusText() const;

    Vst3Plugin& plugin_;
    HWND owner_;
    HWND hwnd_ = nullptr;
    HWND host_ = nullptr;  // Unterfenster fuer die Plugin-Oberflaeche
    HFONT font_ = nullptr;
    RECT learnButton_{}, clearButton_{};
    std::wstring lastResult_;  // Ergebnis der letzten Zuweisung fuer die Anzeige
    Steinberg::IPlugView* view_ = nullptr;
    bool resizing_ = false;  // verhindert Rueckkopplung zwischen Fenster und Plugin
};

}  // namespace vst3
