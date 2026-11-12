#include "ui/MainWindow.h"

#include <shellapi.h>
#include <wincodec.h>
#include <windowsx.h>

#include <vector>

using Microsoft::WRL::ComPtr;

namespace ui {

namespace {

const wchar_t* const kClassName = L"MiniDawMainWindow";

// Rechnertastatur als Klaviatur, nach Tastenposition (Scancode) statt
// Beschriftung, damit es auf jedem Tastaturlayout gleich liegt:
// untere Reihe A S D F G H J K L = weisse Tasten ab C,
// obere Reihe W E   T Z U   O    = schwarze Tasten.
int semitoneForScancode(int scancode) {
    switch (scancode) {
        case 0x1E: return 0;   // A  -> C
        case 0x11: return 1;   // W  -> C#
        case 0x1F: return 2;   // S  -> D
        case 0x12: return 3;   // E  -> D#
        case 0x20: return 4;   // D  -> E
        case 0x21: return 5;   // F  -> F
        case 0x14: return 6;   // T  -> F#
        case 0x22: return 7;   // G  -> G
        case 0x15: return 8;   // Z (deutsch) / Y -> G#
        case 0x23: return 9;   // H  -> A
        case 0x16: return 10;  // U  -> A#
        case 0x24: return 11;  // J  -> H
        case 0x25: return 12;  // K  -> C
        case 0x18: return 13;  // O  -> C#
        case 0x26: return 14;  // L  -> D
        default: return -1;
    }
}
constexpr UINT_PTR kFrameTimer = 1;

bool createFactories(ComPtr<ID2D1Factory>& d2d, ComPtr<IDWriteFactory>& dwrite, std::string& error) {
    if (FAILED(D2D1CreateFactory(D2D1_FACTORY_TYPE_SINGLE_THREADED, d2d.GetAddressOf()))) {
        error = "Direct2D nicht verfuegbar";
        return false;
    }
    if (FAILED(DWriteCreateFactory(DWRITE_FACTORY_TYPE_SHARED, __uuidof(IDWriteFactory),
                                   reinterpret_cast<IUnknown**>(dwrite.GetAddressOf())))) {
        error = "DirectWrite nicht verfuegbar";
        return false;
    }
    return true;
}

}  // namespace

bool MainWindow::create(HINSTANCE instance, int showCommand, std::string& error) {
    if (!createFactories(d2d_, dwrite_, error)) return false;

    WNDCLASSEXW wc{};
    wc.cbSize = sizeof wc;
    wc.style = CS_HREDRAW | CS_VREDRAW | CS_DBLCLKS;
    wc.lpfnWndProc = &MainWindow::windowProc;
    wc.hInstance = instance;
    wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    wc.hIcon = LoadIconW(nullptr, IDI_APPLICATION);
    wc.lpszClassName = kClassName;
    if (!RegisterClassExW(&wc)) { error = "Fensterklasse nicht registrierbar"; return false; }

    hwnd_ = CreateWindowExW(0, kClassName, L"MiniDAW", WS_OVERLAPPEDWINDOW, CW_USEDEFAULT, CW_USEDEFAULT,
                            1300, 860, nullptr, nullptr, instance, this);
    if (!hwnd_) { error = "Fenster nicht anlegbar"; return false; }

    // Fenstergroesse an die Bildschirmskalierung anpassen.
    dpi_ = static_cast<float>(GetDpiForWindow(hwnd_));
    if (dpi_ <= 0.0f) dpi_ = 96.0f;
    SetWindowPos(hwnd_, nullptr, 0, 0, MulDiv(1300, static_cast<int>(dpi_), 96),
                 MulDiv(860, static_cast<int>(dpi_), 96), SWP_NOMOVE | SWP_NOZORDER | SWP_NOACTIVATE);

    DragAcceptFiles(hwnd_, TRUE);
    SetTimer(hwnd_, kFrameTimer, 33, nullptr);
    ShowWindow(hwnd_, showCommand);
    UpdateWindow(hwnd_);
    return true;
}

int MainWindow::run() {
    MSG msg{};
    while (GetMessageW(&msg, nullptr, 0, 0) > 0) {
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }
    return static_cast<int>(msg.wParam);
}

LRESULT CALLBACK MainWindow::windowProc(HWND hwnd, UINT message, WPARAM wParam, LPARAM lParam) {
    MainWindow* self = nullptr;
    if (message == WM_NCCREATE) {
        const auto* create = reinterpret_cast<const CREATESTRUCTW*>(lParam);
        self = static_cast<MainWindow*>(create->lpCreateParams);
        self->hwnd_ = hwnd;
        SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(self));
    } else {
        self = reinterpret_cast<MainWindow*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
    }
    if (self) return self->handle(message, wParam, lParam);
    return DefWindowProcW(hwnd, message, wParam, lParam);
}

LRESULT MainWindow::handle(UINT message, WPARAM wParam, LPARAM lParam) {
    switch (message) {
        case WM_PAINT:
            paint();
            return 0;

        case WM_ERASEBKGND:
            return 1;

        case WM_SIZE:
            if (target_) target_->Resize(D2D1::SizeU(LOWORD(lParam), HIWORD(lParam)));
            InvalidateRect(hwnd_, nullptr, FALSE);
            return 0;

        case WM_DPICHANGED: {
            dpi_ = static_cast<float>(HIWORD(wParam));
            const auto* suggested = reinterpret_cast<const RECT*>(lParam);
            SetWindowPos(hwnd_, nullptr, suggested->left, suggested->top,
                         suggested->right - suggested->left, suggested->bottom - suggested->top,
                         SWP_NOZORDER | SWP_NOACTIVATE);
            canvas_.reset();
            target_.Reset();
            return 0;
        }

        case WM_TIMER:
            if (wParam == kFrameTimer) {
                view_.tick();
                InvalidateRect(hwnd_, nullptr, FALSE);
            }
            return 0;

        case WM_LBUTTONDOWN:
        case WM_LBUTTONDBLCLK:
            SetFocus(hwnd_);
            SetCapture(hwnd_);
            view_.mouseDown(toDip(GET_X_LPARAM(lParam)), toDip(GET_Y_LPARAM(lParam)),
                            message == WM_LBUTTONDBLCLK);
            InvalidateRect(hwnd_, nullptr, FALSE);
            return 0;

        case WM_MOUSEMOVE:
            view_.mouseMove(toDip(GET_X_LPARAM(lParam)), toDip(GET_Y_LPARAM(lParam)));
            if (wParam & MK_LBUTTON) InvalidateRect(hwnd_, nullptr, FALSE);
            return 0;

        case WM_LBUTTONUP:
            view_.mouseUp(toDip(GET_X_LPARAM(lParam)), toDip(GET_Y_LPARAM(lParam)));
            ReleaseCapture();
            InvalidateRect(hwnd_, nullptr, FALSE);
            return 0;

        case WM_MOUSEWHEEL: {
            POINT pt{GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam)};  // Bildschirmkoordinaten
            ScreenToClient(hwnd_, &pt);
            view_.wheel(toDip(pt.x), toDip(pt.y),
                        static_cast<float>(GET_WHEEL_DELTA_WPARAM(wParam)) / static_cast<float>(WHEEL_DELTA),
                        (GET_KEYSTATE_WPARAM(wParam) & MK_CONTROL) != 0);
            InvalidateRect(hwnd_, nullptr, FALSE);
            return 0;
        }

        case WM_KEYDOWN: {
            const int scancode = static_cast<int>((lParam >> 16) & 0xFF);
            const bool repeat = (lParam & (LPARAM{1} << 30)) != 0;
            const int semitone = semitoneForScancode(scancode);
            if (semitone >= 0) {
                if (!repeat) view_.noteKey(semitone, true);
                return 0;
            }
            if (scancode == 0x2C || scancode == 0x2D) {  // die beiden Tasten links unten: Oktave -/+
                if (!repeat) view_.shiftOctave(scancode == 0x2C ? -1 : 1);
                InvalidateRect(hwnd_, nullptr, FALSE);
                return 0;
            }
            if (repeat && wParam != VK_DELETE) return 0;
            if (wParam == VK_SPACE) view_.key(Key::Space);
            else if (wParam == VK_HOME) view_.key(Key::Home);
            else if (wParam == VK_DELETE) view_.key(Key::Delete);
            else if (wParam == 'R') view_.key(Key::Record);
            else break;
            InvalidateRect(hwnd_, nullptr, FALSE);
            return 0;
        }

        case WM_KEYUP: {
            const int semitone = semitoneForScancode(static_cast<int>((lParam >> 16) & 0xFF));
            if (semitone >= 0) {
                view_.noteKey(semitone, false);
                return 0;
            }
            break;
        }

        case WM_KILLFOCUS:
            view_.releaseAllNotes();  // sonst bleiben Noten haengen, wenn das Fenster den Fokus verliert
            break;

        case WM_DROPFILES: {
            const auto drop = reinterpret_cast<HDROP>(wParam);
            POINT pt{};
            DragQueryPoint(drop, &pt);
            std::vector<std::filesystem::path> paths;
            const UINT count = DragQueryFileW(drop, 0xFFFFFFFF, nullptr, 0);
            for (UINT i = 0; i < count; ++i) {
                const UINT length = DragQueryFileW(drop, i, nullptr, 0);
                std::wstring name(static_cast<std::size_t>(length) + 1, L'\0');
                DragQueryFileW(drop, i, name.data(), length + 1);
                name.resize(length);
                paths.emplace_back(name);
            }
            DragFinish(drop);
            view_.dropFiles(toDip(pt.x), toDip(pt.y), paths);
            SetForegroundWindow(hwnd_);
            InvalidateRect(hwnd_, nullptr, FALSE);
            return 0;
        }

        case WM_DESTROY:
            KillTimer(hwnd_, kFrameTimer);
            PostQuitMessage(0);
            return 0;

        default:
            break;
    }
    return DefWindowProcW(hwnd_, message, wParam, lParam);
}

void MainWindow::paint() {
    if (!target_) {
        RECT rc{};
        GetClientRect(hwnd_, &rc);
        const auto props = D2D1::RenderTargetProperties(D2D1_RENDER_TARGET_TYPE_DEFAULT,
                                                        D2D1::PixelFormat(), dpi_, dpi_);
        const auto hwndProps = D2D1::HwndRenderTargetProperties(
            hwnd_, D2D1::SizeU(static_cast<UINT32>(rc.right), static_cast<UINT32>(rc.bottom)));
        if (FAILED(d2d_->CreateHwndRenderTarget(props, hwndProps, &target_))) {
            ValidateRect(hwnd_, nullptr);
            return;
        }
        canvas_ = std::make_unique<Canvas>(target_.Get(), dwrite_.Get());
    }

    target_->BeginDraw();
    const D2D1_SIZE_F size = target_->GetSize();  // in geraeteunabhaengigen Pixeln
    view_.draw(*canvas_, size.width, size.height);
    if (target_->EndDraw() == D2DERR_RECREATE_TARGET) {
        canvas_.reset();
        target_.Reset();
    }
    ValidateRect(hwnd_, nullptr);
}

bool renderToPng(MainView& view, int width, int height, const std::filesystem::path& path,
                 std::string& error) {
    ComPtr<ID2D1Factory> d2d;
    ComPtr<IDWriteFactory> dwrite;
    if (!createFactories(d2d, dwrite, error)) return false;

    ComPtr<IWICImagingFactory> wic;
    if (FAILED(CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&wic)))) {
        error = "WIC nicht verfuegbar";
        return false;
    }

    ComPtr<IWICBitmap> bitmap;
    if (FAILED(wic->CreateBitmap(static_cast<UINT>(width), static_cast<UINT>(height),
                                 GUID_WICPixelFormat32bppPBGRA, WICBitmapCacheOnLoad, &bitmap))) {
        error = "Bitmap nicht anlegbar";
        return false;
    }

    const auto props = D2D1::RenderTargetProperties(
        D2D1_RENDER_TARGET_TYPE_SOFTWARE,
        D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_PREMULTIPLIED), 96.0f, 96.0f);
    ComPtr<ID2D1RenderTarget> target;
    if (FAILED(d2d->CreateWicBitmapRenderTarget(bitmap.Get(), props, &target))) {
        error = "Zeichenziel nicht anlegbar";
        return false;
    }

    {
        Canvas canvas(target.Get(), dwrite.Get());
        target->BeginDraw();
        view.draw(canvas, static_cast<float>(width), static_cast<float>(height));
        if (FAILED(target->EndDraw())) { error = "Zeichnen fehlgeschlagen"; return false; }
    }

    ComPtr<IWICStream> stream;
    ComPtr<IWICBitmapEncoder> encoder;
    ComPtr<IWICBitmapFrameEncode> frame;
    WICPixelFormatGUID format = GUID_WICPixelFormat32bppBGRA;
    const bool ok =
        SUCCEEDED(wic->CreateStream(&stream)) &&
        SUCCEEDED(stream->InitializeFromFilename(path.c_str(), GENERIC_WRITE)) &&
        SUCCEEDED(wic->CreateEncoder(GUID_ContainerFormatPng, nullptr, &encoder)) &&
        SUCCEEDED(encoder->Initialize(stream.Get(), WICBitmapEncoderNoCache)) &&
        SUCCEEDED(encoder->CreateNewFrame(&frame, nullptr)) &&
        SUCCEEDED(frame->Initialize(nullptr)) &&
        SUCCEEDED(frame->SetSize(static_cast<UINT>(width), static_cast<UINT>(height))) &&
        SUCCEEDED(frame->SetPixelFormat(&format)) &&
        SUCCEEDED(frame->WriteSource(bitmap.Get(), nullptr)) &&
        SUCCEEDED(frame->Commit()) &&
        SUCCEEDED(encoder->Commit());
    if (!ok) error = "PNG nicht schreibbar";
    return ok;
}

}  // namespace ui
