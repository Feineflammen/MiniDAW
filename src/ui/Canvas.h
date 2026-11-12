#pragma once

#include <d2d1.h>
#include <dwrite.h>
#include <wrl/client.h>

#include <string>

namespace ui {

struct Color {
    float r, g, b, a;
};

constexpr Color rgb(unsigned hex, float alpha = 1.0f) {
    return {static_cast<float>((hex >> 16) & 0xFF) / 255.0f,
            static_cast<float>((hex >> 8) & 0xFF) / 255.0f,
            static_cast<float>(hex & 0xFF) / 255.0f, alpha};
}

struct Rect {
    float x = 0.0f, y = 0.0f, w = 0.0f, h = 0.0f;
    float right() const { return x + w; }
    float bottom() const { return y + h; }
    bool contains(float px, float py) const { return px >= x && px < x + w && py >= y && py < y + h; }
};

enum class Font { Normal, Small, Large };
enum class Align { Left, Center, Right };

// Duenne Zeichenschicht ueber Direct2D und DirectWrite. Die Ansicht zeichnet
// nur hierueber und weiss nicht, ob das Ziel ein Fenster oder ein Bild ist.
class Canvas {
public:
    Canvas(ID2D1RenderTarget* target, IDWriteFactory* dwrite);

    void clear(Color color);
    void fill(const Rect& rect, Color color);
    void fillRounded(const Rect& rect, float radius, Color color);
    void stroke(const Rect& rect, Color color, float width = 1.0f);
    void strokeRounded(const Rect& rect, float radius, Color color, float width = 1.0f);
    void line(float x1, float y1, float x2, float y2, Color color, float width = 1.0f);
    void text(const std::wstring& text, const Rect& rect, Color color, Font font = Font::Normal,
              Align align = Align::Left);
    void pushClip(const Rect& rect);
    void popClip();

private:
    ID2D1SolidColorBrush* brush(Color color);

    ID2D1RenderTarget* target_;
    Microsoft::WRL::ComPtr<ID2D1SolidColorBrush> brush_;
    Microsoft::WRL::ComPtr<IDWriteTextFormat> normal_, small_, large_;
};

}  // namespace ui
