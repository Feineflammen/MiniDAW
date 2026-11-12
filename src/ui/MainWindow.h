#pragma once

#include "ui/Canvas.h"
#include "ui/MainView.h"

#include <windows.h>

#include <filesystem>
#include <functional>
#include <memory>
#include <string>

namespace ui {

// Befehle des Menues "Datei".
enum MenuCommand { kFileNew = 1001, kFileOpen, kFileSave, kFileSaveAs, kFileExport, kFileQuit };

// Das Win32-Fenster. Reicht Eingaben an die MainView weiter und zeichnet sie
// mit Direct2D, etwa 30-mal pro Sekunde.
class MainWindow {
public:
    explicit MainWindow(MainView& view) : view_(view) {}

    bool create(HINSTANCE instance, int showCommand, std::string& error);
    int run();

    HWND hwnd() const { return hwnd_; }
    void setTitle(const std::wstring& title);

    // Wird fuer Menuebefehle und ihre Tastenkuerzel gerufen (MenuCommand).
    std::function<void(int)> onCommand;
    // Wird gefragt, bevor das Fenster schliesst. false bricht das Schliessen ab.
    std::function<bool()> onCloseRequest;

    // Wird bei jedem Bild (etwa 30-mal pro Sekunde) im UI-Thread gerufen.
    std::function<void()> onFrame;
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

// Dateiauswahl. filter im Format der Windows-Dialoge ("Name\0*.ext\0...\0").
// Liefert einen leeren Pfad, wenn der Benutzer abbricht. filterIndex (ab 1)
// nennt bei Bedarf den gewaehlten Dateityp.
std::filesystem::path askOpenFile(HWND owner, const wchar_t* title, const wchar_t* filter);
std::filesystem::path askSaveFile(HWND owner, const wchar_t* title, const wchar_t* filter,
                                  const wchar_t* defaultExtension, const std::wstring& suggestedName,
                                  int* filterIndex = nullptr);

// Zeichnet die Ansicht ohne Fenster in eine PNG-Datei (fuer automatische Pruefungen).
bool renderToPng(MainView& view, int width, int height, const std::filesystem::path& path,
                 std::string& error);

}  // namespace ui
