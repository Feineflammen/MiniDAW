#pragma once

#include "engine/Engine.h"
#include "model/ProjectController.h"
#include "ui/Canvas.h"

#include <array>
#include <cstdint>
#include <filesystem>
#include <string>
#include <utility>
#include <vector>

namespace ui {

enum class Key { Space, Home, Delete };

// Die gesamte Oberflaeche: Transportleiste, Spurkoepfe, Zeitachse.
// Kennt kein Fenster. Koordinaten sind geraeteunabhaengige Pixel (96 pro Zoll).
class MainView {
public:
    MainView(ProjectController& controller, Engine& engine);

    void setDeviceInfo(std::wstring info) { deviceInfo_ = std::move(info); }
    void selectClip(int track, int index) { selTrack_ = track; selIndex_ = index; }

    void tick();  // einmal pro Bild: Pegel abholen, Ansicht nachfuehren
    void draw(Canvas& canvas, float width, float height);

    void mouseDown(float x, float y, bool doubleClick);
    void mouseMove(float x, float y);
    void mouseUp(float x, float y);
    void wheel(float x, float y, float notches, bool ctrl);
    void key(Key k);
    void dropFiles(float x, float y, const std::vector<std::filesystem::path>& paths);

private:
    enum class Drag { None, MasterGain, TrackGain, TrackPan, Scrub, Clip };

    // Geometrie
    float frameToX(double frame) const;
    double xToFrame(float x) const;
    int rowAt(float y) const;
    float rowY(int track) const;
    Rect muteRect(int track) const;
    Rect soloRect(int track) const;
    Rect gainRect(int track) const;
    Rect panRect(int track) const;
    Rect meterRect(int track) const;

    // Zeichnen
    void drawTopBar(Canvas& c);
    void drawRuler(Canvas& c);
    void drawTimeline(Canvas& c);
    void drawHeaders(Canvas& c);
    void drawClip(Canvas& c, const ClipModel& clip, int track, std::int64_t start, bool selected,
                  float alpha);
    void drawSlider(Canvas& c, const Rect& rect, float position, float fillFrom);

    // Bedienung
    void applyDrag(float x, float y);
    void locateTo(float x);
    void togglePlay();
    std::int64_t snap(std::int64_t start, std::int64_t length, int skipTrack, int skipIndex) const;
    double rulerStep() const;
    void setStatus(const std::wstring& message);

    ProjectController& controller_;
    Engine& engine_;

    float width_ = 1280.0f, height_ = 800.0f;
    double viewStart_ = 0.0;         // Frame am linken Rand der Zeitachse
    double framesPerPixel_ = 882.0;  // Zoom

    int selTrack_ = -1, selIndex_ = -1;

    Drag drag_ = Drag::None;
    int dragTrack_ = -1;
    float dragStartX_ = 0.0f, dragStartY_ = 0.0f;
    bool dragMoved_ = false;
    std::int64_t grabOffset_ = 0;
    std::int64_t ghostStart_ = 0;
    int ghostTrack_ = -1;

    std::array<float, kProjectTracks> meters_{};
    float masterMeter_ = 0.0f;

    std::wstring deviceInfo_;
    std::wstring status_;
    int statusTicks_ = 0;
};

}  // namespace ui
