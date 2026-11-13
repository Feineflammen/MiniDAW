#pragma once

#include <algorithm>
#include <cstdint>
#include <vector>

// Fertig aufbereitete Automation eines Parameters: Punkte nach Zeit sortiert,
// dazwischen linear. Wird vom UI-Thread gebaut und danach nicht mehr geaendert.
struct AutomationCurve {
    struct Point {
        std::int64_t frame = 0;
        float value = 0.0f;
    };
    std::vector<Point> points;

    // Wert an einer Stelle der Zeitachse; vor dem ersten und nach dem letzten
    // Punkt gilt dessen Wert.
    float valueAt(std::int64_t frame) const noexcept {
        if (points.empty()) return 0.0f;
        if (frame <= points.front().frame) return points.front().value;
        if (frame >= points.back().frame) return points.back().value;
        const auto next = std::upper_bound(points.begin(), points.end(), frame,
                                           [](std::int64_t f, const Point& p) { return f < p.frame; });
        const Point& b = *next;
        const Point& a = *(next - 1);
        if (b.frame == a.frame) return b.value;
        const double t = static_cast<double>(frame - a.frame) / static_cast<double>(b.frame - a.frame);
        return a.value + static_cast<float>(t) * (b.value - a.value);
    }
};
