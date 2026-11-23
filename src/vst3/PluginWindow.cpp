#include "vst3/PluginWindow.h"

#include "pluginterfaces/gui/iplugviewcontentscalesupport.h"

#include <windowsx.h>

#include <algorithm>

using namespace Steinberg;

namespace vst3 {

namespace {
const wchar_t* const kClassName = L"MiniDawPluginWindow";
const wchar_t* const kHostClassName = L"MiniDawPluginHost";
constexpr UINT_PTR kLearnTimer = 1;
constexpr int kBarDip = 34;

COLORREF color(unsigned rgb) { return RGB((rgb >> 16) & 0xFF, (rgb >> 8) & 0xFF, rgb & 0xFF); }

void registerClasses() {
    static bool done = false;
    if (done) return;
    done = true;
    WNDCLASSEXW wc{};
    wc.cbSize = sizeof wc;
    wc.lpfnWndProc = DefWindowProcW;  // wird nach dem Anlegen ersetzt
    wc.hInstance = GetModuleHandleW(nullptr);
    wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    wc.hbrBackground = static_cast<HBRUSH>(GetStockObject(BLACK_BRUSH));
    wc.lpszClassName = kClassName;
    RegisterClassExW(&wc);

    WNDCLASSEXW host{};
    host.cbSize = sizeof host;
    host.lpfnWndProc = DefWindowProcW;
    host.hInstance = GetModuleHandleW(nullptr);
    host.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    host.hbrBackground = static_cast<HBRUSH>(GetStockObject(BLACK_BRUSH));
    host.lpszClassName = kHostClassName;
    RegisterClassExW(&host);
}

std::wstring controllerLabel(int controller) {
    if (const wchar_t* name = midilearn::controllerName(controller)) return name;
    return L"CC " + std::to_wstring(controller);
}
}  // namespace

int PluginWindow::barHeight() const {
    const UINT dpi = hwnd_ ? GetDpiForWindow(hwnd_) : 96;
    return MulDiv(kBarDip, static_cast<int>(dpi), 96);
}

bool PluginWindow::open(std::string& error) {
    if (hwnd_) {
        ShowWindow(hwnd_, SW_SHOWNORMAL);
        SetForegroundWindow(hwnd_);
        return true;
    }

    view_ = plugin_.createView();
    if (!view_) { error = "Das Plugin hat keine eigene Oberflaeche"; return false; }
    if (view_->isPlatformTypeSupported(kPlatformTypeHWND) != kResultOk) {
        view_->release();
        view_ = nullptr;
        error = "Die Oberflaeche des Plugins passt nicht zu Windows-Fenstern";
        return false;
    }

    registerClasses();
    const bool resizable = view_->canResize() == kResultTrue;
    const DWORD style = WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | WS_CLIPCHILDREN | (resizable ? WS_THICKFRAME : 0);
    hwnd_ = CreateWindowExW(0, kClassName, plugin_.name().c_str(), style, CW_USEDEFAULT, CW_USEDEFAULT, 640, 400,
                            owner_, nullptr, GetModuleHandleW(nullptr), nullptr);
    if (!hwnd_) {
        view_->release();
        view_ = nullptr;
        error = "Fenster fuer das Plugin nicht anlegbar";
        return false;
    }
    SetWindowLongPtrW(hwnd_, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(this));
    SetWindowLongPtrW(hwnd_, GWLP_WNDPROC, reinterpret_cast<LONG_PTR>(&PluginWindow::windowProc));
    host_ = CreateWindowExW(0, kHostClassName, L"", WS_CHILD | WS_VISIBLE | WS_CLIPCHILDREN, 0, barHeight(), 640,
                            400 - barHeight(), hwnd_, nullptr, GetModuleHandleW(nullptr), nullptr);
    font_ = CreateFontW(-MulDiv(9, static_cast<int>(GetDpiForWindow(hwnd_)), 72), 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE,
                        DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY, DEFAULT_PITCH,
                        L"Segoe UI");

    view_->setFrame(this);

    // Plugins, die es unterstuetzen, erfahren die Bildschirmskalierung.
    if (FUnknownPtr<IPlugViewContentScaleSupport> scale{view_}) {
        const UINT dpi = GetDpiForWindow(hwnd_);
        scale->setContentScaleFactor(static_cast<float>(dpi) / 96.0f);
    }

    ViewRect size{};
    if (view_->getSize(&size) == kResultOk && size.right > size.left && size.bottom > size.top)
        setClientSize(size.right - size.left, size.bottom - size.top);

    if (!host_ || view_->attached(host_, kPlatformTypeHWND) != kResultOk) {
        view_->setFrame(nullptr);
        view_->release();
        view_ = nullptr;
        close();
        error = "Das Plugin konnte seine Oberflaeche nicht anzeigen";
        return false;
    }

    // Manche Plugins kennen ihre Groesse erst jetzt.
    if (view_->getSize(&size) == kResultOk && size.right > size.left && size.bottom > size.top)
        setClientSize(size.right - size.left, size.bottom - size.top);

    SetTimer(hwnd_, kLearnTimer, 100, nullptr);
    ShowWindow(hwnd_, SW_SHOWNORMAL);
    return true;
}

void PluginWindow::close() {
    plugin_.stopLearning();
    if (view_) {
        view_->setFrame(nullptr);
        view_->removed();
        view_->release();
        view_ = nullptr;
    }
    if (hwnd_) {
        const HWND window = hwnd_;
        hwnd_ = nullptr;
        host_ = nullptr;  // wird mit dem Fenster zerstoert
        KillTimer(window, kLearnTimer);
        SetWindowLongPtrW(window, GWLP_USERDATA, 0);
        DestroyWindow(window);
    }
    if (font_) {
        DeleteObject(font_);
        font_ = nullptr;
    }
}

void PluginWindow::setClientSize(int width, int height) {
    if (!hwnd_) return;
    RECT rect{0, 0, width, height + barHeight()};
    AdjustWindowRectEx(&rect, static_cast<DWORD>(GetWindowLongPtrW(hwnd_, GWL_STYLE)), FALSE,
                       static_cast<DWORD>(GetWindowLongPtrW(hwnd_, GWL_EXSTYLE)));
    resizing_ = true;
    SetWindowPos(hwnd_, nullptr, 0, 0, rect.right - rect.left, rect.bottom - rect.top,
                 SWP_NOMOVE | SWP_NOZORDER | SWP_NOACTIVATE);
    layoutHost();
    resizing_ = false;
}

void PluginWindow::layoutHost() {
    if (!hwnd_ || !host_) return;
    RECT client{};
    GetClientRect(hwnd_, &client);
    const int bar = barHeight();
    SetWindowPos(host_, nullptr, 0, bar, client.right, std::max(0L, client.bottom - bar), SWP_NOZORDER | SWP_NOACTIVATE);
}

// Was die Leiste neben den Knoepfen sagt.
std::wstring PluginWindow::statusText() const {
    if (plugin_.learning()) {
        if (!plugin_.learnHasParam() && !plugin_.learnHasController())
            return L"Bewege einen Regler im Plugin und einen am Keyboard (Spur scharf schalten, R) …";
        if (!plugin_.learnHasParam()) return L"Controller erkannt – jetzt den Regler im Plugin bewegen …";
        return L"Regler erkannt – jetzt den Regler am Keyboard bewegen …";
    }
    if (!lastResult_.empty()) return lastResult_;
    const auto list = plugin_.assignments();
    std::wstring text;
    if (list.empty()) {
        text = plugin_.hasOwnMidiMapping() ? L"Das Plugin ordnet Controller auch selbst zu."
                                           : L"Keine eigenen Zuweisungen.";
    } else {
        text = L"Zuweisungen: ";
        for (std::size_t i = 0; i < list.size() && i < 4; ++i) {
            if (i) text += L", ";
            std::wstring param = plugin_.parameterName(list[i].param);
            if (param.empty()) param = L"Parameter " + std::to_wstring(list[i].param);
            text += controllerLabel(list[i].controller) + L" → " + param;
        }
        if (list.size() > 4) text += L" …";
    }
    return text;
}

void PluginWindow::paintBar(HDC dc) {
    RECT client{};
    GetClientRect(hwnd_, &client);
    const int bar = barHeight();
    const UINT dpi = GetDpiForWindow(hwnd_);
    const auto px = [dpi](int dip) { return MulDiv(dip, static_cast<int>(dpi), 96); };

    // Doppelt gepuffert, damit nichts flackert.
    HDC mem = CreateCompatibleDC(dc);
    HBITMAP bitmap = CreateCompatibleBitmap(dc, client.right, bar);
    HGDIOBJ oldBitmap = SelectObject(mem, bitmap);
    HGDIOBJ oldFont = SelectObject(mem, font_);
    SetBkMode(mem, TRANSPARENT);

    RECT all{0, 0, client.right, bar};
    HBRUSH background = CreateSolidBrush(color(0x2A2C31));
    FillRect(mem, &all, background);
    DeleteObject(background);

    const auto button = [&](RECT& r, int x, int width, const wchar_t* text, unsigned fill, unsigned ink) {
        r = {x, px(5), x + width, bar - px(5)};
        HBRUSH brush = CreateSolidBrush(color(fill));
        HGDIOBJ oldBrush = SelectObject(mem, brush);
        HGDIOBJ oldPen = SelectObject(mem, GetStockObject(NULL_PEN));
        RoundRect(mem, r.left, r.top, r.right, r.bottom, px(6), px(6));
        SelectObject(mem, oldPen);
        SelectObject(mem, oldBrush);
        DeleteObject(brush);
        SetTextColor(mem, color(ink));
        RECT t = r;
        DrawTextW(mem, text, -1, &t, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
    };
    const bool learning = plugin_.learning();
    button(learnButton_, px(8), px(120), learning ? L"Abbrechen" : L"MIDI zuweisen", learning ? 0xE8A33D : 0x3A3D45,
           learning ? 0x15161A : 0xDADCE2);
    int textLeft = learnButton_.right + px(12);
    clearButton_ = {};
    if (!learning && !plugin_.assignments().empty()) {
        button(clearButton_, client.right - px(130), px(122), L"Zuweisungen löschen", 0x3A3D45, 0xDADCE2);
    }
    const int textRight = clearButton_.right > 0 ? clearButton_.left - px(12) : client.right - px(10);
    SetTextColor(mem, color(learning ? 0xE8A33D : 0x8D9099));
    RECT t{textLeft, 0, textRight, bar};
    const std::wstring status = statusText();
    DrawTextW(mem, status.c_str(), -1, &t, DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);

    BitBlt(dc, 0, 0, client.right, bar, mem, 0, 0, SRCCOPY);
    SelectObject(mem, oldFont);
    SelectObject(mem, oldBitmap);
    DeleteObject(bitmap);
    DeleteDC(mem);
}

void PluginWindow::click(int x, int y) {
    const POINT p{x, y};
    if (PtInRect(&learnButton_, p)) {
        lastResult_.clear();
        if (plugin_.learning()) plugin_.stopLearning();
        else plugin_.startLearning();
    } else if (PtInRect(&clearButton_, p)) {
        plugin_.clearAssignments();
        lastResult_.clear();
        if (onChange) onChange();
    }
    RECT bar{0, 0, 10000, barHeight()};
    InvalidateRect(hwnd_, &bar, FALSE);
}

tresult PLUGIN_API PluginWindow::queryInterface(const TUID interfaceId, void** obj) {
    if (FUnknownPrivate::iidEqual(interfaceId, IPlugFrame::iid) || FUnknownPrivate::iidEqual(interfaceId, FUnknown::iid)) {
        *obj = static_cast<IPlugFrame*>(this);
        return kResultOk;
    }
    *obj = nullptr;
    return kNoInterface;
}

tresult PLUGIN_API PluginWindow::resizeView(IPlugView* view, ViewRect* newSize) {
    if (!view || !newSize || view != view_) return kInvalidArgument;
    setClientSize(newSize->right - newSize->left, newSize->bottom - newSize->top);
    view->onSize(newSize);
    return kResultOk;
}

LRESULT CALLBACK PluginWindow::windowProc(HWND hwnd, UINT message, WPARAM wParam, LPARAM lParam) {
    auto* self = reinterpret_cast<PluginWindow*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
    if (self) {
        switch (message) {
            case WM_CLOSE:
                self->close();
                return 0;
            case WM_SIZE:
                self->layoutHost();
                // Der Benutzer zieht am Rahmen: dem Plugin die neue Groesse geben.
                if (self->view_ && !self->resizing_ && wParam != SIZE_MINIMIZED) {
                    const int height = std::max(0, static_cast<int>(HIWORD(lParam)) - self->barHeight());
                    ViewRect rect{0, 0, static_cast<int32>(LOWORD(lParam)), static_cast<int32>(height)};
                    self->view_->checkSizeConstraint(&rect);
                    self->view_->onSize(&rect);
                }
                InvalidateRect(hwnd, nullptr, FALSE);
                return 0;
            case WM_ERASEBKGND:
                return 1;
            case WM_PAINT: {
                PAINTSTRUCT ps;
                HDC dc = BeginPaint(hwnd, &ps);
                self->paintBar(dc);
                EndPaint(hwnd, &ps);
                return 0;
            }
            case WM_LBUTTONDOWN:
                self->click(GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam));
                return 0;
            case WM_TIMER:
                if (wParam == kLearnTimer) {
                    // Lernen abschliessen, sobald Regler und Controller bekannt sind.
                    midilearn::Assignment created;
                    if (self->plugin_.finishLearning(&created)) {
                        std::wstring param = self->plugin_.parameterName(created.param);
                        if (param.empty()) param = L"Parameter " + std::to_wstring(created.param);
                        self->lastResult_ = L"Zugewiesen: " + controllerLabel(created.controller) + L" → " + param;
                        if (self->onChange) self->onChange();
                    }
                    RECT bar{0, 0, 10000, self->barHeight()};
                    InvalidateRect(hwnd, &bar, FALSE);
                }
                return 0;
            default:
                break;
        }
    }
    return DefWindowProcW(hwnd, message, wParam, lParam);
}

}  // namespace vst3
