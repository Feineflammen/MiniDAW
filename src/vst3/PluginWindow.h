#pragma once

#include "vst3/Vst3Host.h"

#include <windows.h>

#include <string>

namespace vst3 {

// Eigenes Fenster fuer die Bedienoberflaeche eines Plugins.
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

    // --- IPlugFrame: Das Plugin moechte eine andere Fenstergroesse ---
    Steinberg::tresult PLUGIN_API queryInterface(const Steinberg::TUID interfaceId, void** obj) override;
    Steinberg::uint32 PLUGIN_API addRef() override { return 1; }
    Steinberg::uint32 PLUGIN_API release() override { return 1; }
    Steinberg::tresult PLUGIN_API resizeView(Steinberg::IPlugView* view, Steinberg::ViewRect* newSize) override;

private:
    static LRESULT CALLBACK windowProc(HWND hwnd, UINT message, WPARAM wParam, LPARAM lParam);
    void setClientSize(int width, int height);

    Vst3Plugin& plugin_;
    HWND owner_;
    HWND hwnd_ = nullptr;
    Steinberg::IPlugView* view_ = nullptr;
    bool resizing_ = false;  // verhindert Rueckkopplung zwischen Fenster und Plugin
};

}  // namespace vst3
