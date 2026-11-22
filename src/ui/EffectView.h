#pragma once

#include "engine/BuiltinEffect.h"
#include "ui/PanelView.h"

#include <string>
#include <vector>

namespace ui {

// Bedienfeld fuer EQ, Kompressor, Hall und Echo: oben eine Grafik (EQ-Kurve
// zum Ziehen, Kennlinie, Abklingkurve, Wiederholungen), darunter die Regler in
// Gruppen.
class EffectView final : public PanelView {
public:
    explicit EffectView(fx::BuiltinEffect& effect);

    float viewWidth() const override { return width_; }
    float viewHeight() const override { return height_; }
    void draw(Canvas& c, float width, float height) override;
    void mouseDown(float x, float y) override;
    void mouseMove(float x, float y, bool fine) override;
    void mouseUp() override;
    void doubleClick(float x, float y) override;
    void wheel(float x, float y, float notches, bool fine) override;
    bool dragging() const override { return dragParam_ >= 0 || dragHandle_ >= 0; }

private:
    struct Section {
        const wchar_t* title;
        int toggle;                 // Schalter im Kopf (-1: keiner)
        std::vector<int> sliders;
        std::vector<int> checks;    // Haekchen unter den Reglern
    };
    enum class Kind { Eq, Compressor, Reverb, Delay, Other };
    enum class HitKind { Slider, Toggle };
    struct Hit {
        Rect rect;
        HitKind kind;
        int param;
        float barWidth;
    };
    // Griffe in der EQ-Kurve: Frequenz- und Pegel-Parameter (-1: nur waagerecht), Guete
    struct Handle {
        int freq, gain, q;
    };

    void set(int param, float value);
    bool enabled(int param) const;
    void drawSection(Canvas& c, const Rect& box, const Section& section);
    void drawSlider(Canvas& c, const Rect& row, int param, bool active);
    void drawEqGraph(Canvas& c, const Rect& area);
    void drawCompressorGraph(Canvas& c, const Rect& area);
    void drawReverbGraph(Canvas& c, const Rect& area);
    void drawDelayGraph(Canvas& c, const Rect& area);
    // EQ: Umrechnung Frequenz/Pegel <-> Grafik
    float freqToX(double hz) const;
    double xToFreq(float x) const;
    float gainToY(double db) const;
    double yToGain(float y) const;
    float handleY(const Handle& h) const;
    int handleAt(float x, float y) const;

    fx::BuiltinEffect& fx_;
    Kind kind_ = Kind::Other;
    std::vector<Section> sections_;
    float width_ = 640.0f, height_ = 420.0f, graphH_ = 0.0f;
    Rect graph_{};
    std::vector<Handle> handles_;
    std::vector<Hit> hits_;
    int dragParam_ = -1;
    float dragStartX_ = 0.0f, dragStartPos_ = 0.0f, dragBar_ = 1.0f;
    int dragHandle_ = -1;
};

}  // namespace ui
