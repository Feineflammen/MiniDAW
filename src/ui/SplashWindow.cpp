#include "ui/SplashWindow.h"

#include "resource.h"

#include <dwmapi.h>
#include <wincodec.h>
#include <wrl/client.h>

#include <algorithm>
#include <cmath>
#include <cstdint>

using Microsoft::WRL::ComPtr;

namespace ui {

namespace {

constexpr wchar_t kClassName[] = L"MiniDAWSplash";
constexpr int kWidthDip = 860;   // 16:9 wie das Bild
constexpr int kHeightDip = 484;
constexpr wchar_t kVersion[] = L"Version 0.7";

constexpr wchar_t kAboutText[] =
    L"Kleine Musik-Software für Windows: 16 Spuren, Audio und MIDI, VST3, Mischpult.\n\n"
    L"Idee, Vorgaben und Tests: Feineflammen. Der Programmcode wurde vollständig von einer KI "
    L"geschrieben (Claude von Anthropic), die Grafiken von ChatGPT (OpenAI).\n\n"
    L"Freie Software unter der GNU General Public License, Version 3: kostenlos benutzen, "
    L"weitergeben und verändern; veränderte Fassungen ebenfalls unter der GPLv3 mit "
    L"Quellcode. Kostenlos und ohne Gewähr; vor wichtigen Aufnahmen Sicherungskopien anlegen.\n\n"
    L"Quellcode und Lizenztext: github.com/Feineflammen/daw\n\n"
    L"Enthält das VST 3 SDK (MIT) und das ASIO SDK (GPLv3) von Steinberg. VST und ASIO sind "
    L"Marken der Steinberg Media Technologies GmbH.\n\n"
    L"Impressum: feineflammen.de";

}  // namespace

struct SplashWindow::State {
    bool about = false;
    bool closing = false;
    std::wstring status;
    int width = 0;
    int height = 0;
    float scale = 1.0f;
    HBITMAP image = nullptr;  // fertig skaliert und links abgedunkelt

    ~State() {
        if (image) DeleteObject(image);
    }
};

namespace {

using State = SplashWindow::State;

// Links wird das Bild abgedunkelt, damit der Text darauf lesbar ist.
void darken(std::uint8_t* pixels, int width, int height, bool about) {
    const float reach = about ? 0.62f : 0.55f;   // bis wohin abgedunkelt wird (Anteil der Breite)
    const float strongest = about ? 0.28f : 0.6f;  // Helligkeit ganz links
    const int end = static_cast<int>(static_cast<float>(width) * reach);
    for (int x = 0; x < end; ++x) {
        const float t = static_cast<float>(x) / static_cast<float>(end);
        const float factor = strongest + (1.0f - strongest) * t * t;
        for (int y = 0; y < height; ++y) {
            std::uint8_t* p = pixels + (static_cast<std::size_t>(y) * static_cast<std::size_t>(width) + static_cast<std::size_t>(x)) * 4;
            for (int c = 0; c < 3; ++c) p[c] = static_cast<std::uint8_t>(static_cast<float>(p[c]) * factor);
        }
    }
}

// Titelbild aus den Ressourcen, auf width x height skaliert, als GDI-Bitmap.
HBITMAP loadImage(HINSTANCE instance, int width, int height, bool about) {
    HRSRC resource = FindResourceW(instance, MAKEINTRESOURCEW(IDR_SPLASH), MAKEINTRESOURCEW(10));  // RT_RCDATA
    if (!resource) return nullptr;
    HGLOBAL handle = LoadResource(instance, resource);
    void* data = handle ? LockResource(handle) : nullptr;
    const DWORD size = SizeofResource(instance, resource);
    if (!data || size == 0) return nullptr;

    ComPtr<IWICImagingFactory> wic;
    ComPtr<IWICStream> stream;
    ComPtr<IWICBitmapDecoder> decoder;
    ComPtr<IWICBitmapFrameDecode> frame;
    ComPtr<IWICBitmapScaler> scaler;
    ComPtr<IWICFormatConverter> converter;
    const UINT w = static_cast<UINT>(width);
    const UINT h = static_cast<UINT>(height);
    if (FAILED(CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&wic))) ||
        FAILED(wic->CreateStream(&stream)) ||
        FAILED(stream->InitializeFromMemory(static_cast<BYTE*>(data), size)) ||
        FAILED(wic->CreateDecoderFromStream(stream.Get(), nullptr, WICDecodeMetadataCacheOnDemand, &decoder)) ||
        FAILED(decoder->GetFrame(0, &frame)) || FAILED(wic->CreateBitmapScaler(&scaler)) ||
        FAILED(scaler->Initialize(frame.Get(), w, h, WICBitmapInterpolationModeHighQualityCubic)) ||
        FAILED(wic->CreateFormatConverter(&converter)) ||
        FAILED(converter->Initialize(scaler.Get(), GUID_WICPixelFormat32bppBGR, WICBitmapDitherTypeNone, nullptr, 0.0,
                                     WICBitmapPaletteTypeCustom)))
        return nullptr;

    BITMAPINFO info{};
    info.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    info.bmiHeader.biWidth = width;
    info.bmiHeader.biHeight = -height;  // von oben nach unten
    info.bmiHeader.biPlanes = 1;
    info.bmiHeader.biBitCount = 32;
    info.bmiHeader.biCompression = BI_RGB;
    void* bits = nullptr;
    HBITMAP bitmap = CreateDIBSection(nullptr, &info, DIB_RGB_COLORS, &bits, nullptr, 0);
    if (!bitmap || !bits) return nullptr;
    const UINT stride = w * 4;
    if (FAILED(converter->CopyPixels(nullptr, stride, stride * h, static_cast<BYTE*>(bits)))) {
        DeleteObject(bitmap);
        return nullptr;
    }
    darken(static_cast<std::uint8_t*>(bits), width, height, about);
    return bitmap;
}

HFONT makeFont(float pixels, int weight) {
    return CreateFontW(-static_cast<int>(std::lround(pixels)), 0, 0, 0, weight, FALSE, FALSE, FALSE, DEFAULT_CHARSET,
                       OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY, DEFAULT_PITCH | FF_SWISS,
                       L"Segoe UI");
}

void drawText(HDC dc, const std::wstring& text, RECT rect, float pixels, int weight, COLORREF color, UINT flags) {
    HFONT font = makeFont(pixels, weight);
    HGDIOBJ old = SelectObject(dc, font);
    SetTextColor(dc, color);
    DrawTextW(dc, text.c_str(), static_cast<int>(text.size()), &rect, flags | DT_NOPREFIX);
    SelectObject(dc, old);
    DeleteObject(font);
}

void render(const State& s, HDC dc) {
    if (s.image) {
        HDC source = CreateCompatibleDC(dc);
        HGDIOBJ old = SelectObject(source, s.image);
        BitBlt(dc, 0, 0, s.width, s.height, source, 0, 0, SRCCOPY);
        SelectObject(source, old);
        DeleteDC(source);
    } else {
        RECT all{0, 0, s.width, s.height};
        HBRUSH brush = CreateSolidBrush(RGB(21, 22, 26));
        FillRect(dc, &all, brush);
        DeleteObject(brush);
    }
    SetBkMode(dc, TRANSPARENT);
    const auto px = [&](float dip) { return static_cast<int>(std::lround(dip * s.scale)); };
    const float k = s.scale;
    const COLORREF white = RGB(245, 246, 250);
    const COLORREF soft = RGB(192, 202, 218);

    if (!s.about) {
        drawText(dc, L"MiniDAW", {px(44), px(38), s.width, px(110)}, 50.0f * k, FW_SEMIBOLD, white, DT_SINGLELINE);
        drawText(dc, kVersion, {px(47), px(104), s.width, px(130)}, 16.0f * k, FW_NORMAL, soft, DT_SINGLELINE);
        drawText(dc, s.status, {px(47), s.height - px(54), s.width / 2, s.height - px(26)}, 15.0f * k, FW_NORMAL,
                 RGB(215, 220, 230), DT_SINGLELINE | DT_VCENTER | DT_END_ELLIPSIS);
        return;
    }
    drawText(dc, L"MiniDAW", {px(36), px(24), s.width, px(80)}, 40.0f * k, FW_SEMIBOLD, white, DT_SINGLELINE);
    drawText(dc, kVersion, {px(38), px(76), s.width, px(98)}, 14.0f * k, FW_NORMAL, soft, DT_SINGLELINE);
    drawText(dc, kAboutText, {px(38), px(112), static_cast<int>(static_cast<float>(s.width) * 0.53f), s.height - px(44)},
             13.0f * k, FW_NORMAL, RGB(226, 229, 236), DT_WORDBREAK);
    drawText(dc, L"Klick oder Taste schließt das Fenster.", {px(38), s.height - px(36), s.width / 2, s.height - px(14)},
             12.0f * k, FW_NORMAL, RGB(140, 150, 166), DT_SINGLELINE | DT_VCENTER);
}

void requestClose(HWND hwnd, State* state) {
    if (!state || state->closing) return;
    state->closing = true;
    DestroyWindow(hwnd);
}

LRESULT CALLBACK windowProc(HWND hwnd, UINT message, WPARAM wParam, LPARAM lParam) {
    auto* state = reinterpret_cast<State*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
    switch (message) {
        case WM_NCCREATE: {
            const auto* create = reinterpret_cast<const CREATESTRUCTW*>(lParam);
            SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(create->lpCreateParams));
            break;
        }
        case WM_ERASEBKGND:
            return 1;
        case WM_PAINT: {
            PAINTSTRUCT paint;
            HDC dc = BeginPaint(hwnd, &paint);
            if (state) {
                // Erst in einen Puffer, dann auf einmal: kein Flackern beim Wechsel der Statuszeile.
                HDC buffer = CreateCompatibleDC(dc);
                HBITMAP surface = CreateCompatibleBitmap(dc, state->width, state->height);
                HGDIOBJ old = SelectObject(buffer, surface);
                render(*state, buffer);
                BitBlt(dc, 0, 0, state->width, state->height, buffer, 0, 0, SRCCOPY);
                SelectObject(buffer, old);
                DeleteObject(surface);
                DeleteDC(buffer);
            }
            EndPaint(hwnd, &paint);
            return 0;
        }
        case WM_LBUTTONUP:
        case WM_RBUTTONUP:
        case WM_KEYDOWN:
            if (state && state->about) {
                requestClose(hwnd, state);
                return 0;
            }
            break;
        case WM_ACTIVATE:
            if (state && state->about && LOWORD(wParam) == WA_INACTIVE) requestClose(hwnd, state);
            break;
        case WM_NCDESTROY:
            delete state;
            SetWindowLongPtrW(hwnd, GWLP_USERDATA, 0);
            break;
        default:
            break;
    }
    return DefWindowProcW(hwnd, message, wParam, lParam);
}

void registerClass(HINSTANCE instance) {
    static bool registered = false;
    if (registered) return;
    WNDCLASSEXW wc{};
    wc.cbSize = sizeof wc;
    wc.style = CS_DROPSHADOW;
    wc.lpfnWndProc = &windowProc;
    wc.hInstance = instance;
    wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    wc.hIcon = LoadIconW(instance, MAKEINTRESOURCEW(IDI_MINIDAW));
    wc.lpszClassName = kClassName;
    registered = RegisterClassExW(&wc) != 0;
}

State* makeState(HINSTANCE instance, bool about, float scale) {
    auto* state = new State;
    state->about = about;
    state->scale = scale;
    state->width = static_cast<int>(std::lround(static_cast<float>(kWidthDip) * scale));
    state->height = static_cast<int>(std::lround(static_cast<float>(kHeightDip) * scale));
    state->image = loadImage(instance, state->width, state->height, about);
    return state;
}

// Rahmenloses Fenster mitten auf dem Bildschirm (bzw. dem des Hauptfensters).
HWND createWindow(HINSTANCE instance, HWND owner, bool about) {
    registerClass(instance);
    const UINT dpi = owner ? GetDpiForWindow(owner) : GetDpiForSystem();
    State* state = makeState(instance, about, static_cast<float>(dpi > 0 ? dpi : 96) / 96.0f);

    HMONITOR monitor = owner ? MonitorFromWindow(owner, MONITOR_DEFAULTTONEAREST)
                             : MonitorFromPoint(POINT{0, 0}, MONITOR_DEFAULTTOPRIMARY);
    MONITORINFO info{};
    info.cbSize = sizeof info;
    GetMonitorInfoW(monitor, &info);
    const RECT& work = info.rcWork;
    const int x = work.left + (work.right - work.left - state->width) / 2;
    const int y = work.top + (work.bottom - work.top - state->height) / 2;

    // Das Ladebild bleibt oben, bis das Hauptfenster fertig ist; das Ueber-Fenster gehoert zum Hauptfenster.
    const DWORD exStyle = WS_EX_TOOLWINDOW | (about ? 0u : static_cast<DWORD>(WS_EX_TOPMOST));
    HWND hwnd = CreateWindowExW(exStyle, kClassName, L"MiniDAW", WS_POPUP, x, y, state->width, state->height, owner,
                                nullptr, instance, state);
    if (!hwnd) return nullptr;  // state ist dann schon mit WM_NCDESTROY freigegeben oder nie zugeordnet worden

    const DWM_WINDOW_CORNER_PREFERENCE corner = DWMWCP_ROUND;  // Windows 11: runde Ecken
    DwmSetWindowAttribute(hwnd, DWMWA_WINDOW_CORNER_PREFERENCE, &corner, sizeof corner);
    return hwnd;
}

}  // namespace

SplashWindow::~SplashWindow() { close(); }

void SplashWindow::show(HINSTANCE instance) {
    close();
    hwnd_ = createWindow(instance, nullptr, false);
    if (!hwnd_) return;
    ShowWindow(hwnd_, SW_SHOWNOACTIVATE);
    UpdateWindow(hwnd_);
}

void SplashWindow::setStatus(const std::wstring& text) {
    if (!hwnd_ || !IsWindow(hwnd_)) return;
    if (auto* state = reinterpret_cast<State*>(GetWindowLongPtrW(hwnd_, GWLP_USERDATA))) state->status = text;
    InvalidateRect(hwnd_, nullptr, FALSE);
    UpdateWindow(hwnd_);
    // Nachrichten des Ladebilds abarbeiten, damit Windows es nicht fuer haengend haelt.
    MSG message;
    while (PeekMessageW(&message, hwnd_, 0, 0, PM_REMOVE)) {
        TranslateMessage(&message);
        DispatchMessageW(&message);
    }
}

void SplashWindow::close() {
    if (hwnd_ && IsWindow(hwnd_)) DestroyWindow(hwnd_);
    hwnd_ = nullptr;
}

void SplashWindow::showAbout(HINSTANCE instance, HWND owner) {
    HWND hwnd = createWindow(instance, owner, true);
    if (!hwnd) return;
    ShowWindow(hwnd, SW_SHOW);
    SetForegroundWindow(hwnd);
    SetFocus(hwnd);
}

bool SplashWindow::renderPng(HINSTANCE instance, bool about, const std::wstring& path) {
    State* state = makeState(instance, about, 1.0f);
    if (!about) state->status = L"Suche Plugins …";
    const int width = state->width;
    const int height = state->height;

    BITMAPINFO info{};
    info.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    info.bmiHeader.biWidth = width;
    info.bmiHeader.biHeight = -height;
    info.bmiHeader.biPlanes = 1;
    info.bmiHeader.biBitCount = 32;
    info.bmiHeader.biCompression = BI_RGB;
    void* bits = nullptr;
    HDC screen = GetDC(nullptr);
    HDC dc = CreateCompatibleDC(screen);
    HBITMAP target = CreateDIBSection(screen, &info, DIB_RGB_COLORS, &bits, nullptr, 0);
    ReleaseDC(nullptr, screen);
    bool ok = target && bits && state->image;
    if (ok) {
        HGDIOBJ old = SelectObject(dc, target);
        render(*state, dc);
        GdiFlush();
        SelectObject(dc, old);

        ComPtr<IWICImagingFactory> wic;
        ComPtr<IWICBitmap> bitmap;
        ComPtr<IWICStream> stream;
        ComPtr<IWICBitmapEncoder> encoder;
        ComPtr<IWICBitmapFrameEncode> frame;
        WICPixelFormatGUID format = GUID_WICPixelFormat32bppBGR;
        const UINT stride = static_cast<UINT>(width) * 4;
        ok = SUCCEEDED(CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&wic))) &&
             SUCCEEDED(wic->CreateBitmapFromMemory(static_cast<UINT>(width), static_cast<UINT>(height),
                                                   GUID_WICPixelFormat32bppBGR, stride, stride * static_cast<UINT>(height),
                                                   static_cast<BYTE*>(bits), &bitmap)) &&
             SUCCEEDED(wic->CreateStream(&stream)) &&
             SUCCEEDED(stream->InitializeFromFilename(path.c_str(), GENERIC_WRITE)) &&
             SUCCEEDED(wic->CreateEncoder(GUID_ContainerFormatPng, nullptr, &encoder)) &&
             SUCCEEDED(encoder->Initialize(stream.Get(), WICBitmapEncoderNoCache)) &&
             SUCCEEDED(encoder->CreateNewFrame(&frame, nullptr)) && SUCCEEDED(frame->Initialize(nullptr)) &&
             SUCCEEDED(frame->SetSize(static_cast<UINT>(width), static_cast<UINT>(height))) &&
             SUCCEEDED(frame->SetPixelFormat(&format)) && SUCCEEDED(frame->WriteSource(bitmap.Get(), nullptr)) &&
             SUCCEEDED(frame->Commit()) && SUCCEEDED(encoder->Commit());
    }
    if (target) DeleteObject(target);
    DeleteDC(dc);
    delete state;
    return ok;
}

}  // namespace ui
