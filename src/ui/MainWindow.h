#pragma once

#include "ui/Canvas.h"
#include "ui/MainView.h"

#include <windows.h>

#include <filesystem>
#include <functional>
#include <memory>
#include <string>

namespace ui {

// Das Win32-Fenster. Reicht Eingaben an die MainView weiter und zeichnet sie
// mit Direct2D, etwa 30-mal pro Sekunde.
class MainWindow {
public:
    explicit MainWindow(MainView& view) : view_(view) {}

    bool create(HINSTANCE instance, int showCommand, std::string& error);
    int run();

    // Wird etwa einmal pro Sekunde im UI-Thread gerufen (Geraete neu erkennen und Aehnliches).
    std::function<void()> onPeriodic;

private:
    static LRESULT CALLBACK windowProc(HWND hwnd, UINT message, WPARAM wParam, LPARAM lParam);
    LRESULT handle(UINT message, WPARAM wParam, LPARAM lParam);
    void paint();
    float toDip(int pixels) const { return static_cast<float>(pixels) * 96.0f / dpi_; }

    MainView& view_;
    HWND hwnd_ = nullptr;
    float dpi_ = 96.0f;
    int frameCount_ = 0;
    Microsoft::WRL::ComPtr<ID2D1Factory> d2d_;
    Microsoft::WRL::ComPtr<IDWriteFactory> dwrite_;
    Microsoft::WRL::ComPtr<ID2D1HwndRenderTarget> target_;
    std::unique_ptr<Canvas> canvas_;
};

// Zeichnet die Ansicht ohne Fenster in eine PNG-Datei (fuer automatische Pruefungen).
bool renderToPng(MainView& view, int width, int height, const std::filesystem::path& path,
                 std::string& error);

}  // namespace ui
