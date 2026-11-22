#pragma once

#include "ui/Canvas.h"
#include "ui/MasteringView.h"

#include <windows.h>

#include <functional>
#include <memory>
#include <string>

namespace ui {

// Eigenes Fenster fuer den eingebauten Mastering-Effekt (Direct2D wie das Hauptfenster).
class MasteringWindow {
public:
    MasteringWindow(mastering::MasteringEffect& effect, HWND owner, std::wstring title);
    ~MasteringWindow() { close(); }
    MasteringWindow(const MasteringWindow&) = delete;
    MasteringWindow& operator=(const MasteringWindow&) = delete;

    // Oeffnet das Fenster oder holt es nach vorn.
    bool open(std::string& error);
    void close();

    // Nach jeder Aenderung durch den Benutzer (Projekt gilt dann als geaendert).
    std::function<void()> onChange;

private:
    static LRESULT CALLBACK windowProc(HWND hwnd, UINT message, WPARAM wParam, LPARAM lParam);
    LRESULT handle(UINT message, WPARAM wParam, LPARAM lParam);
    void paint();
    float toDip(int pixels) const { return static_cast<float>(pixels) * 96.0f / dpi_; }

    MasteringView view_;
    HWND owner_;
    std::wstring title_;
    HWND hwnd_ = nullptr;
    float dpi_ = 96.0f;
    Microsoft::WRL::ComPtr<ID2D1Factory> d2d_;
    Microsoft::WRL::ComPtr<IDWriteFactory> dwrite_;
    Microsoft::WRL::ComPtr<ID2D1HwndRenderTarget> target_;
    std::unique_ptr<Canvas> canvas_;
};

}  // namespace ui
