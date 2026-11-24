#pragma once

#include "engine/Engine.h"
#include "model/ProjectController.h"
#include "ui/Canvas.h"

#include <cstdint>
#include <vector>

namespace ui {

// Noten-Editor fuer einen MIDI-Clip: Klaviatur links, Notenraster in der
// Mitte, Anschlagstaerken unten. Die MainView blendet ihn im unteren Teil des
// Fensters ein und reicht Eingaben weiter, die in sein Feld fallen.
//
// Waehrend eines Ziehvorgangs arbeitet der Editor auf einer eigenen Kopie der
// Noten; erst beim Loslassen geht die Aenderung an den ProjectController.
class PianoRoll {
public:
    PianoRoll(ProjectController& controller, Engine& engine) : controller_(controller), engine_(engine) {}

    bool isOpen() const { return track_ >= 0; }
    int track() const { return track_; }
    int index() const { return index_; }
    void open(int track, int index);
    // Die Samplerate hat sich um factor geaendert (neu / alt): Ausschnitt mitnehmen.
    void sampleRateChanged(double factor) noexcept;
    void close();

    void draw(Canvas& canvas, const Rect& panel);

    void mouseDown(float x, float y, bool rightButton);
    void mouseMove(float x, float y);
    void mouseUp(float x, float y);
    void wheel(float x, float y, float notches, bool ctrl, bool shift);
    bool deleteSelected();  // true, wenn eine Note geloescht wurde
    void showLane(int controller) { lane_ = controller; }  // -1: Anschlag

private:
    enum class Drag { None, Move, Resize, Velocity, Control, ControlErase };

    const MidiClipModel* clip() const;
    const std::vector<MidiNote>& notes() const;
    std::int64_t gridStep() const;
    std::int64_t snap(std::int64_t frame, bool floor) const;
    float xOf(double frame) const;
    double frameAt(float x) const;
    float yOf(int pitch) const;
    int pitchAt(float y) const;
    int noteAt(float x, float y) const;
    void beginEdit();
    void commit();
    void applyVelocity(float x, float y);

    // Untere Spur: Anschlag (lane_ < 0) oder ein Controller
    std::vector<int> laneChoices() const;
    void stepLane(int direction);
    int laneValueAt(float y) const;
    float laneYOf(int value) const;
    void drawLane(Canvas& c, const MidiClipModel& clip);
    void applyControl(float x, float y);
    void eraseControl(float x);
    void commitControls();

    ProjectController& controller_;
    Engine& engine_;

    int track_ = -1;
    int index_ = -1;

    // Aufteilung, bei jedem Zeichnen neu berechnet
    Rect panel_, header_, keys_, grid_, velocity_, laneLabel_, closeRect_, divisionRect_;

    // Ansicht
    bool fitPending_ = false;
    double viewStart_ = 0.0;         // Frame (relativ zum Clip) am linken Rand
    double framesPerPixel_ = 100.0;
    int topPitch_ = 84;              // hoechste sichtbare Note
    int division_ = 16;              // Raster: 4, 8, 16 oder 32 (Notenwert)

    // Bearbeitung
    int selected_ = -1;
    Drag drag_ = Drag::None;
    bool editing_ = false;
    bool changed_ = false;
    std::vector<MidiNote> edit_;
    std::int64_t grabOffset_ = 0;

    int lane_ = -1;                        // -1: Anschlag, sonst Controller (0..129)
    std::vector<MidiControl> controlEdit_; // Kopie waehrend des Zeichnens
    bool controlEditing_ = false;
    std::int64_t lastControlFrame_ = -1;
};

}  // namespace ui
