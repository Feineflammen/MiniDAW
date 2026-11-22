#include "ui/EffectWindow.h"

#include "resource.h"

#include <windowsx.h>

namespace ui {

namespace {
const wchar_t* const kClassName = L"MiniDawEffectWindow";
constexpr UINT_PTR kMeterTimer = 1;

void registerClass() {
    static bool done = false;
    if (done) return;
    done = true;
    WNDCLASSEXW wc{};
    wc.cbSize = sizeof wc;
    wc.style = CS_DBLCLKS;
    wc.lpfnWndProc = DefWindowProcW;  // wird nach dem Anlegen ersetzt
    wc.hInstance = GetModuleHandleW(nullptr);
    wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    wc.hIcon = LoadIconW(GetModuleHandleW(nullptr), MAKEINTRESOURCEW(IDI_MINIDAW));
    wc.lpszClassName = kClassName;
    RegisterClassExW(&wc);
}
}  // namespace

EffectWindow::EffectWindow(std::unique_ptr<PanelView> view, HWND owner, std::wstring title)
    : view_(std::move(view)), owner_(owner), title_(std::move(title)) {
    view_->onChange = [this] {
        if (onChange) onChange();
    };
}

bool EffectWindow::open(std::string& error) {
    if (hwnd_) {
        ShowWindow(hwnd_, SW_SHOWNORMAL);
        SetForegroundWindow(hwnd_);
        return true;
    }
    if (!d2d_ && FAILED(D2D1CreateFactory(D2D1_FACTORY_TYPE_SINGLE_THREADED, d2d_.GetAddressOf()))) {
        error = "Direct2D nicht verfuegbar";
        return false;
    }
    if (!dwrite_ && FAILED(DWriteCreateFactory(DWRITE_FACTORY_TYPE_SHARED, __uuidof(IDWriteFactory),
                                               reinterpret_cast<IUnknown**>(dwrite_.GetAddressOf())))) {
        error = "DirectWrite nicht verfuegbar";
        return false;
    }

    registerClass();
    const DWORD style = WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | WS_MINIMIZEBOX;
    hwnd_ = CreateWindowExW(0, kClassName, title_.c_str(), style, CW_USEDEFAULT, CW_USEDEFAULT, 900, 540, owner_,
                            nullptr, GetModuleHandleW(nullptr), nullptr);
    if (!hwnd_) {
        error = "Fenster fuer den Effekt nicht anlegbar";
        return false;
    }
    SetWindowLongPtrW(hwnd_, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(this));
    SetWindowLongPtrW(hwnd_, GWLP_WNDPROC, reinterpret_cast<LONG_PTR>(&EffectWindow::windowProc));

    // Feste Groesse in geraeteunabhaengigen Pixeln, an die Skalierung angepasst.
    dpi_ = static_cast<float>(GetDpiForWindow(hwnd_));
    if (dpi_ <= 0.0f) dpi_ = 96.0f;
    const UINT dpi = static_cast<UINT>(dpi_);
    RECT rect{0, 0, MulDiv(static_cast<int>(view_->viewWidth()), static_cast<int>(dpi), 96),
              MulDiv(static_cast<int>(view_->viewHeight()), static_cast<int>(dpi), 96)};
    AdjustWindowRectExForDpi(&rect, style, FALSE, 0, dpi);
    SetWindowPos(hwnd_, nullptr, 0, 0, rect.right - rect.left, rect.bottom - rect.top,
                 SWP_NOMOVE | SWP_NOZORDER | SWP_NOACTIVATE);

    SetTimer(hwnd_, kMeterTimer, 50, nullptr);  // Messwerte etwa 20-mal pro Sekunde
    ShowWindow(hwnd_, SW_SHOWNORMAL);
    return true;
}

void EffectWindow::close() {
    canvas_.reset();
    target_.Reset();
    if (hwnd_) {
        const HWND window = hwnd_;
        hwnd_ = nullptr;
        KillTimer(window, kMeterTimer);
        SetWindowLongPtrW(window, GWLP_USERDATA, 0);
        SetWindowLongPtrW(window, GWLP_WNDPROC, reinterpret_cast<LONG_PTR>(&DefWindowProcW));
        DestroyWindow(window);
    }
}

LRESULT CALLBACK EffectWindow::windowProc(HWND hwnd, UINT message, WPARAM wParam, LPARAM lParam) {
    auto* self = reinterpret_cast<EffectWindow*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
    if (self) return self->handle(message, wParam, lParam);
    return DefWindowProcW(hwnd, message, wParam, lParam);
}

LRESULT EffectWindow::handle(UINT message, WPARAM wParam, LPARAM lParam) {
    switch (message) {
        case WM_CLOSE:
            close();
            return 0;
        case WM_ERASEBKGND:
            return 1;
        case WM_PAINT:
            paint();
            return 0;
        case WM_SIZE:
            if (target_) target_->Resize(D2D1::SizeU(LOWORD(lParam), HIWORD(lParam)));
            InvalidateRect(hwnd_, nullptr, FALSE);
            return 0;
        case WM_DPICHANGED: {
            dpi_ = static_cast<float>(HIWORD(wParam));
            const auto* suggested = reinterpret_cast<const RECT*>(lParam);
            SetWindowPos(hwnd_, nullptr, suggested->left, suggested->top, suggested->right - suggested->left,
                         suggested->bottom - suggested->top, SWP_NOZORDER | SWP_NOACTIVATE);
            canvas_.reset();
            target_.Reset();
            return 0;
        }
        case WM_TIMER:
            if (wParam == kMeterTimer) InvalidateRect(hwnd_, nullptr, FALSE);
            return 0;
        case WM_LBUTTONDOWN:
            SetCapture(hwnd_);
            view_->mouseDown(toDip(GET_X_LPARAM(lParam)), toDip(GET_Y_LPARAM(lParam)));
            InvalidateRect(hwnd_, nullptr, FALSE);
            return 0;
        case WM_LBUTTONDBLCLK:
            view_->doubleClick(toDip(GET_X_LPARAM(lParam)), toDip(GET_Y_LPARAM(lParam)));
            InvalidateRect(hwnd_, nullptr, FALSE);
            return 0;
        case WM_MOUSEMOVE:
            if (view_->dragging()) {
                view_->mouseMove(toDip(GET_X_LPARAM(lParam)), toDip(GET_Y_LPARAM(lParam)), (wParam & MK_SHIFT) != 0);
                InvalidateRect(hwnd_, nullptr, FALSE);
            }
            return 0;
        case WM_LBUTTONUP:
        case WM_CAPTURECHANGED:
            view_->mouseUp();
            if (message == WM_LBUTTONUP) ReleaseCapture();
            return 0;
        case WM_MOUSEWHEEL: {
            POINT pt{GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam)};
            ScreenToClient(hwnd_, &pt);
            view_->wheel(toDip(pt.x), toDip(pt.y), static_cast<float>(GET_WHEEL_DELTA_WPARAM(wParam)) / WHEEL_DELTA,
                        (GET_KEYSTATE_WPARAM(wParam) & MK_SHIFT) != 0);
            InvalidateRect(hwnd_, nullptr, FALSE);
            return 0;
        }
        default:
            break;
    }
    return DefWindowProcW(hwnd_, message, wParam, lParam);
}

void EffectWindow::paint() {
    if (!target_) {
        RECT rc{};
        GetClientRect(hwnd_, &rc);
        const auto props =
            D2D1::RenderTargetProperties(D2D1_RENDER_TARGET_TYPE_DEFAULT, D2D1::PixelFormat(), dpi_, dpi_);
        const auto hwndProps = D2D1::HwndRenderTargetProperties(
            hwnd_, D2D1::SizeU(static_cast<UINT32>(rc.right), static_cast<UINT32>(rc.bottom)));
        if (FAILED(d2d_->CreateHwndRenderTarget(props, hwndProps, &target_))) {
            ValidateRect(hwnd_, nullptr);
            return;
        }
        canvas_ = std::make_unique<Canvas>(target_.Get(), dwrite_.Get());
    }
    target_->BeginDraw();
    const D2D1_SIZE_F size = target_->GetSize();
    view_->draw(*canvas_, size.width, size.height);
    if (target_->EndDraw() == D2DERR_RECREATE_TARGET) {
        canvas_.reset();
        target_.Reset();
    }
    ValidateRect(hwnd_, nullptr);
}

}  // namespace ui
