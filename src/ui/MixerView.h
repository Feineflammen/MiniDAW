#pragma once

#include "engine/Engine.h"
#include "model/ProjectController.h"
#include "ui/Canvas.h"

#include <array>
#include <functional>

namespace ui {

// Mischpult: ein Kanalzug je Spur, dann die vier Busse und der Master.
// Kennt kein Fenster und keine Plugins; Plugin-Wuensche gehen ueber die
// Rueckrufe an die Hauptansicht.
//
// Ein Kanalzug von oben nach unten: Name, Instrument und vier Effekte,
// Sends zu den Bussen, Ausgang, Panorama, Fader mit Pegel, M und S.
class MixerView {
public:
    MixerView(ProjectController& controller, Engine& engine);

    // Leerer Platz angeklickt (oder Rechtsklick): Auswahl oeffnen, anchor ist das Feld.
    std::function<void(int strip, int slot, const Rect& anchor)> onPickPlugin;
    // Belegter Platz angeklickt: Plugin-Fenster oeffnen.
    std::function<void(int strip, int slot)> onOpenEditor;
    // Hinweis fuer die Statusleiste.
    std::function<void(const std::wstring&)> onStatus;

    // Pegel der Spuren und des Masters fuehrt die Hauptansicht, die Busse diese Ansicht.
    void setMeters(const std::array<float, kProjectTracks>* tracks, float master) {
        trackMeters_ = tracks;
        masterMeter_ = master;
    }
    // Platz, fuer den gerade die Auswahl offen ist (wird hervorgehoben); -1 = keiner.
    void setActiveSlot(int strip, int slot) { activeStrip_ = strip; activeSlot_ = slot; }

    void tick();
    void draw(Canvas& c, const Rect& area);

    // Liefern true, wenn die Ansicht etwas damit angefangen hat.
    bool mouseDown(float x, float y, bool doubleClick);
    bool rightMouseDown(float x, float y);
    void mouseMove(float x, float y, bool fine);
    void mouseUp();
    bool dragging() const noexcept { return drag_ != Drag::None; }
    void wheel(float x, float y, float notches);

private:
    enum class Drag { None, Fader, Pan, Send };
    enum class Part { None, Slot, Send, Output, Pan, Fader, Mute, Solo };

    struct Hit {
        Part part = Part::None;
        int strip = -1;
        int index = 0;  // Platz bzw. Bus
    };

    float stripX(int strip) const;
    float contentWidth() const;
    float scrollWidth() const;
    Rect slotRect(int strip, int slot) const;
    Rect sendRect(int strip, int bus) const;
    Rect outputRect(int strip) const;
    Rect panRect(int strip) const;
    Rect faderRect(int strip) const;
    Rect meterRect(int strip) const;
    Rect muteRect(int strip) const;
    Rect soloRect(int strip) const;
    Hit hitTest(float x, float y) const;

    void drawStrip(Canvas& c, int strip);
    float stripGain(int strip) const;
    float stripPan(int strip) const;
    bool stripMute(int strip) const;
    float stripMeter(int strip) const;
    void setGain(int strip, float gain);
    void setPan(int strip, float pan);
    void stepOutput(int track, int delta);
    // Folgt der Regler gerade einer Automation (und wird nicht geschrieben)?
    bool automationLocked(int strip, int param) const;
    bool automated(int strip, int param) const;

    ProjectController& controller_;
    Engine& engine_;
    Rect area_{};
    float scroll_ = 0.0f;

    const std::array<float, kProjectTracks>* trackMeters_ = nullptr;
    float masterMeter_ = 0.0f;
    std::array<float, kProjectBuses> busMeters_{};

    int activeStrip_ = -1, activeSlot_ = -1;

    Drag drag_ = Drag::None;
    int dragStrip_ = -1;
    int dragBus_ = 0;
    float dragStartY_ = 0.0f;
    float dragStartPos_ = 0.0f;
    float dragLastY_ = 0.0f;
};

}  // namespace ui
