#pragma once

#include <string>
#include <vector>

namespace ui {

// Was die Oberflaeche zum Umgang mit Plugins braucht. Die Umsetzung fuer
// VST3 liegt in vst3/PluginManager; die Oberflaeche kennt nur diese Schnittstelle.
class PluginHost {
public:
    virtual ~PluginHost() = default;

    // Namen der gefundenen Plugins (sucht bei jedem Aufruf neu).
    virtual std::vector<std::wstring> pluginNames() = 0;
    // Ordner, in dem gesucht wird, fuer Hinweise an den Benutzer.
    virtual std::wstring pluginFolder() = 0;

    // Plaetze werden ueber Kanalzug (strip: 0..15 Spuren, 16..19 Busse,
    // 20 Master) und Platz (slot: 0 Instrument, 1..4 Effekte) angesprochen.

    // Laedt das Plugin mit diesem Index (aus der zuletzt gelieferten Liste) auf
    // den Platz. Bei false steht der Grund in error.
    virtual bool load(int strip, int slot, int index, std::wstring& error) = 0;
    // Leert den Platz.
    virtual void unload(int strip, int slot) = 0;
    // Oeffnet das Fenster des Plugins auf dem Platz.
    virtual bool openEditor(int strip, int slot, std::wstring& error) = 0;
};

}  // namespace ui
