#pragma once

#include "engine/FaderCurve.h"
#include "ui/Canvas.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <string>

// Gemeinsame Farben und Regler-Kurven der Oberflaeche.
namespace ui::theme {

constexpr Color kBg = rgb(0x1B1C1F);
constexpr Color kPanel = rgb(0x2A2C31);
constexpr Color kRowA = rgb(0x212226);
constexpr Color kRowB = rgb(0x25262B);
constexpr Color kLine = rgb(0x3A3C43);
constexpr Color kGrid = rgb(0x2F3137);
constexpr Color kText = rgb(0xDADCE2);
constexpr Color kTextDim = rgb(0x8D9099);
constexpr Color kAccent = rgb(0x4C9AFF);
constexpr Color kPlayhead = rgb(0xFF5A5A);
constexpr Color kMuteOn = rgb(0xE8A33D);
constexpr Color kRecord = rgb(0xE5484D);
constexpr Color kSoloOn = rgb(0xE6D34A);
constexpr Color kButton = rgb(0x3A3D45);
constexpr Color kMeter = rgb(0x4CD27A);
constexpr Color kMeterHot = rgb(0xFF5A5A);
constexpr Color kDark = rgb(0x15161A);
constexpr Color kBusColor = rgb(0xC9A227);
constexpr Color kMasterColor = rgb(0xDADCE2);

constexpr unsigned kClipColors[] = {0x3E7CB8, 0x3F9E7A, 0xB07A3C, 0x8A5BB5,
                                    0xB5525F, 0x4D9AA8, 0x8C9440, 0xA8608F};

// Fader-Kennlinie (gemeinsam mit der Engine, siehe engine/FaderCurve.h).
constexpr float kMinDb = fader::kMinDb;
constexpr float kMaxDb = fader::kMaxDb;
inline float gainToPosition(float gain) { return fader::gainToPosition(gain); }
inline float positionToGain(float position) { return fader::positionToGain(position); }

inline std::wstring formatDb(float gain) {
    if (gain <= 0.0f) return L"-inf";
    wchar_t buffer[32];
    std::swprintf(buffer, 32, L"%+.1f", 20.0 * std::log10(static_cast<double>(gain)));
    return buffer;
}

}  // namespace ui::theme

namespace ui::theme {

// Waagerechter Regler: Schiene, gefuellter Bereich ab fillFrom, Griff.
inline void drawSlider(Canvas& c, const Rect& rect, float position, float fillFrom) {
    const float mid = rect.y + rect.h * 0.5f;
    c.fillRounded({rect.x, mid - 1.5f, rect.w, 3.0f}, 1.5f, kDark);
    const float a = rect.x + std::min(position, fillFrom) * rect.w;
    const float b = rect.x + std::max(position, fillFrom) * rect.w;
    if (b - a > 0.5f) c.fillRounded({a, mid - 1.5f, b - a, 3.0f}, 1.5f, kAccent);
    c.fillRounded({rect.x + position * rect.w - 3.5f, rect.y, 7.0f, rect.h}, 2.0f, kText);
}

}  // namespace ui::theme
