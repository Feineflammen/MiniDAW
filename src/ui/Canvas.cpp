#include "ui/Canvas.h"

namespace ui {

namespace {

D2D1_RECT_F toD2D(const Rect& r) { return D2D1::RectF(r.x, r.y, r.x + r.w, r.y + r.h); }

Microsoft::WRL::ComPtr<IDWriteTextFormat> makeFormat(IDWriteFactory* dwrite, const wchar_t* family,
                                                     float size, DWRITE_FONT_WEIGHT weight) {
    Microsoft::WRL::ComPtr<IDWriteTextFormat> format;
    if (SUCCEEDED(dwrite->CreateTextFormat(family, nullptr, weight, DWRITE_FONT_STYLE_NORMAL,
                                           DWRITE_FONT_STRETCH_NORMAL, size, L"", &format))) {
        format->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
        format->SetWordWrapping(DWRITE_WORD_WRAPPING_NO_WRAP);
    }
    return format;
}

}  // namespace

Canvas::Canvas(ID2D1RenderTarget* target, IDWriteFactory* dwrite) : target_(target) {
    target_->CreateSolidColorBrush(D2D1::ColorF(0.0f, 0.0f, 0.0f), &brush_);
    normal_ = makeFormat(dwrite, L"Segoe UI", 12.5f, DWRITE_FONT_WEIGHT_NORMAL);
    small_ = makeFormat(dwrite, L"Segoe UI", 10.5f, DWRITE_FONT_WEIGHT_NORMAL);
    large_ = makeFormat(dwrite, L"Consolas", 19.0f, DWRITE_FONT_WEIGHT_NORMAL);
}

ID2D1SolidColorBrush* Canvas::brush(Color color) {
    brush_->SetColor(D2D1::ColorF(color.r, color.g, color.b, color.a));
    return brush_.Get();
}

void Canvas::clear(Color color) { target_->Clear(D2D1::ColorF(color.r, color.g, color.b, color.a)); }

void Canvas::fill(const Rect& rect, Color color) { target_->FillRectangle(toD2D(rect), brush(color)); }

void Canvas::fillRounded(const Rect& rect, float radius, Color color) {
    target_->FillRoundedRectangle(D2D1::RoundedRect(toD2D(rect), radius, radius), brush(color));
}

void Canvas::stroke(const Rect& rect, Color color, float width) {
    target_->DrawRectangle(toD2D(rect), brush(color), width);
}

void Canvas::strokeRounded(const Rect& rect, float radius, Color color, float width) {
    target_->DrawRoundedRectangle(D2D1::RoundedRect(toD2D(rect), radius, radius), brush(color), width);
}

void Canvas::line(float x1, float y1, float x2, float y2, Color color, float width) {
    target_->DrawLine(D2D1::Point2F(x1, y1), D2D1::Point2F(x2, y2), brush(color), width);
}

void Canvas::text(const std::wstring& text, const Rect& rect, Color color, Font font, Align align) {
    IDWriteTextFormat* format =
        font == Font::Small ? small_.Get() : font == Font::Large ? large_.Get() : normal_.Get();
    if (!format || text.empty()) return;
    format->SetTextAlignment(align == Align::Center  ? DWRITE_TEXT_ALIGNMENT_CENTER
                             : align == Align::Right ? DWRITE_TEXT_ALIGNMENT_TRAILING
                                                     : DWRITE_TEXT_ALIGNMENT_LEADING);
    target_->DrawText(text.c_str(), static_cast<UINT32>(text.size()), format, toD2D(rect), brush(color),
                      D2D1_DRAW_TEXT_OPTIONS_CLIP);
}

void Canvas::pushClip(const Rect& rect) {
    target_->PushAxisAlignedClip(toD2D(rect), D2D1_ANTIALIAS_MODE_ALIASED);
}

void Canvas::popClip() { target_->PopAxisAlignedClip(); }

}  // namespace ui
