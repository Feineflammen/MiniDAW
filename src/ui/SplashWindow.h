#pragma once

#include <windows.h>

#include <string>

namespace ui {

// Ladebild beim Start und Fenster "Ueber MiniDAW": das Titelbild aus den
// Ressourcen der exe, links abgedunkelt, Text darueber. Reines Win32/GDI,
// das Bild entpackt WIC (COM muss initialisiert sein).
class SplashWindow {
public:
    SplashWindow() = default;
    ~SplashWindow();
    SplashWindow(const SplashWindow&) = delete;
    SplashWindow& operator=(const SplashWindow&) = delete;

    // Ladebild ohne Rahmen mitten auf dem Bildschirm, bis close() kommt.
    void show(HINSTANCE instance);
    // Zeile unten links ("Suche Plugins ..."); wird sofort gezeichnet.
    void setStatus(const std::wstring& text);
    void close();

    // Fenster "Ueber MiniDAW" ueber dem Hauptfenster; Klick oder Taste schliesst es.
    static void showAbout(HINSTANCE instance, HWND owner);

    // Screenshot-Modus: zeichnet Ladebild (about = false) oder Ueber-Fenster in eine PNG-Datei.
    static bool renderPng(HINSTANCE instance, bool about, const std::wstring& path);

    struct State;  // Innenleben, fuer die Fensterprozedur

private:
    HWND hwnd_ = nullptr;
};

}  // namespace ui
