#pragma once

#include "engine/Mastering.h"
#include "ui/Canvas.h"
#include "ui/PanelView.h"

#include <functional>
#include <initializer_list>
#include <string>
#include <vector>

namespace ui {

// Bedienfeld des eingebauten Mastering-Effekts: Stufen mit Reglern links,
// Lautheit und Spitzenpegel rechts. Zeichnet nur ueber Canvas, damit es sich
// auch fuer Screenshots in ein Bild zeichnen laesst.
class MasteringView final : public PanelView {
public:
    static constexpr float kViewWidth = 900.0f;
    static constexpr float kViewHeight = 540.0f;

    explicit MasteringView(mastering::MasteringEffect& effect) : fx_(effect) {}

    float viewWidth() const override { return kViewWidth; }
    float viewHeight() const override { return kViewHeight; }
    void draw(Canvas& c, float width, float height) override;
    void mouseDown(float x, float y) override;
    void mouseMove(float x, float y, bool fine) override;
    void mouseUp() override;
    void doubleClick(float x, float y) override;
    void wheel(float x, float y, float notches, bool fine) override;
    bool dragging() const override { return dragParam_ >= 0; }

private:
    enum class HitKind { Slider, Toggle, ResetMeters };
    struct Hit {
        Rect rect;
        HitKind kind;
        int param;
        float barWidth;
    };
    const Hit* hitAt(float x, float y) const;
    void set(int param, float value);
    void drawSection(Canvas& c, const Rect& box, const wchar_t* title, int toggle, std::initializer_list<int> sliders,
                     int extraToggle = -1);
    void drawSlider(Canvas& c, const Rect& row, int param, bool enabled);
    void drawMeters(Canvas& c, const Rect& panel);

    mastering::MasteringEffect& fx_;
    std::vector<Hit> hits_;
    int dragParam_ = -1;
    float dragStartX_ = 0.0f, dragStartPos_ = 0.0f, dragBar_ = 1.0f;
};

}  // namespace ui
