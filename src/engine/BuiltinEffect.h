#pragma once

#include "engine/AudioPlugin.h"

#include <atomic>
#include <memory>
#include <string>
#include <vector>

// Gemeinsame Grundlage der eingebauten Effekte (EQ, Kompressor, Hall, Echo,
// Mastering): Parameter, die die Oberflaeche atomar setzt und der Audio-Thread
// zu Beginn jedes Blocks uebernimmt, und Speichern als lesbarer Text.
namespace fx {

struct ParamInfo {
    const char* key;       // Name in der Projektdatei, nie aendern
    const wchar_t* label;  // Anzeige
    const wchar_t* unit;
    float min, max, def;
    bool toggle;
    bool logScale;  // Regler logarithmisch (Frequenz, Zeit)
    const wchar_t* const* choices = nullptr;  // Auswahl statt Zahl: Werte 0..choiceCount-1
    int choiceCount = 0;
};

// Wert eines Reglers zu seiner Stellung 0..1 und zurueck.
float toPosition(const ParamInfo& p, float value) noexcept;
float fromPosition(const ParamInfo& p, float position) noexcept;
// Wert passend begrenzen (Schalter 0/1, Auswahl ganzzahlig).
float clampValue(const ParamInfo& p, float value) noexcept;

class BuiltinEffect : public AudioPlugin {
public:
    // id steht in der Projektdatei (als "<MiniDAW>/id"), name in der Oberflaeche.
    BuiltinEffect(const char* id, const wchar_t* name, const ParamInfo* params, int count, double sampleRate);

    bool isInstrument() const noexcept override { return false; }

    const char* id() const noexcept { return id_; }
    const wchar_t* name() const noexcept { return name_; }
    int paramCount() const noexcept { return count_; }
    const ParamInfo& info(int param) const noexcept { return params_[param]; }
    double sampleRate() const noexcept { return sampleRate_; }

    // UI-Thread (und Laden)
    float param(int param) const noexcept;
    void setParam(int param, float value) noexcept;
    std::vector<char> saveState() const;
    void loadState(const std::vector<char>& state);

protected:
    // Audio-Thread: zu Beginn von process() rufen; danach liefert value() die Werte dieses Blocks.
    // Liefert true beim allerersten Block: Dann sollen geglaettete Werte gleich am Ziel stehen.
    bool pullParams() noexcept;
    float value(int param) const noexcept { return current_[static_cast<std::size_t>(param)]; }

    double sampleRate_;

private:
    const char* id_;
    const wchar_t* name_;
    const ParamInfo* params_;
    int count_;
    std::unique_ptr<std::atomic<float>[]> values_;
    std::vector<float> current_;
    bool first_ = true;
};

// Die eingebauten Effekte in der Reihenfolge der Auswahlliste.
struct BuiltinType {
    const char* id;
    const wchar_t* name;
};
const std::vector<BuiltinType>& builtinTypes();
// Legt einen eingebauten Effekt an; nullptr bei unbekannter id.
std::shared_ptr<BuiltinEffect> createBuiltin(const std::string& id, double sampleRate);

}  // namespace fx
