#pragma once

#include "ui/Canvas.h"

#include <functional>

namespace ui {

// Bedienfeld in einem eigenen Fenster (eingebaute Effekte). Zeichnet nur ueber
// Canvas, damit es sich auch fuer Screenshots in ein Bild zeichnen laesst.
// Koordinaten in geraeteunabhaengigen Pixeln.
class PanelView {
public:
    virtual ~PanelView() = default;
    virtual float viewWidth() const = 0;
    virtual float viewHeight() const = 0;
    virtual void draw(Canvas& c, float width, float height) = 0;
    virtual void mouseDown(float x, float y) = 0;
    virtual void mouseMove(float x, float y, bool fine) = 0;
    virtual void mouseUp() = 0;
    virtual void doubleClick(float x, float y) = 0;
    virtual void wheel(float x, float y, float notches, bool fine) = 0;
    virtual bool dragging() const = 0;

    // Wird nach jeder Aenderung durch den Benutzer gerufen.
    std::function<void()> onChange;
};

}  // namespace ui
