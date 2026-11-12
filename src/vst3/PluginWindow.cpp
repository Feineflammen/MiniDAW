#include "vst3/PluginWindow.h"

#include "pluginterfaces/gui/iplugviewcontentscalesupport.h"

using namespace Steinberg;

namespace vst3 {

namespace {
const wchar_t* const kClassName = L"MiniDawPluginWindow";

void registerClass() {
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
}
}  // namespace

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

    registerClass();
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

    view_->setFrame(this);

    // Plugins, die es unterstuetzen, erfahren die Bildschirmskalierung.
    if (FUnknownPtr<IPlugViewContentScaleSupport> scale{view_}) {
        const UINT dpi = GetDpiForWindow(hwnd_);
        scale->setContentScaleFactor(static_cast<float>(dpi) / 96.0f);
    }

    ViewRect size{};
    if (view_->getSize(&size) == kResultOk && size.right > size.left && size.bottom > size.top)
        setClientSize(size.right - size.left, size.bottom - size.top);

    if (view_->attached(hwnd_, kPlatformTypeHWND) != kResultOk) {
        view_->setFrame(nullptr);
        view_->release();
        view_ = nullptr;
        DestroyWindow(hwnd_);
        hwnd_ = nullptr;
        error = "Das Plugin konnte seine Oberflaeche nicht anzeigen";
        return false;
    }

    // Manche Plugins kennen ihre Groesse erst jetzt.
    if (view_->getSize(&size) == kResultOk && size.right > size.left && size.bottom > size.top)
        setClientSize(size.right - size.left, size.bottom - size.top);

    ShowWindow(hwnd_, SW_SHOWNORMAL);
    return true;
}

void PluginWindow::close() {
    if (view_) {
        view_->setFrame(nullptr);
        view_->removed();
        view_->release();
        view_ = nullptr;
    }
    if (hwnd_) {
        const HWND window = hwnd_;
        hwnd_ = nullptr;
        SetWindowLongPtrW(window, GWLP_USERDATA, 0);
        DestroyWindow(window);
    }
}

void PluginWindow::setClientSize(int width, int height) {
    if (!hwnd_) return;
    RECT rect{0, 0, width, height};
    AdjustWindowRectEx(&rect, static_cast<DWORD>(GetWindowLongPtrW(hwnd_, GWL_STYLE)), FALSE,
                       static_cast<DWORD>(GetWindowLongPtrW(hwnd_, GWL_EXSTYLE)));
    resizing_ = true;
    SetWindowPos(hwnd_, nullptr, 0, 0, rect.right - rect.left, rect.bottom - rect.top,
                 SWP_NOMOVE | SWP_NOZORDER | SWP_NOACTIVATE);
    resizing_ = false;
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
                // Der Benutzer zieht am Rahmen: dem Plugin die neue Groesse geben.
                if (self->view_ && !self->resizing_ && wParam != SIZE_MINIMIZED) {
                    ViewRect rect{0, 0, static_cast<int32>(LOWORD(lParam)), static_cast<int32>(HIWORD(lParam))};
                    self->view_->checkSizeConstraint(&rect);
                    self->view_->onSize(&rect);
                }
                return 0;
            default:
                break;
        }
    }
    return DefWindowProcW(hwnd, message, wParam, lParam);
}

}  // namespace vst3
