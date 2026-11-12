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

    // Laedt das Plugin mit diesem Index (aus der zuletzt gelieferten Liste) auf
    // die Spur. Bei false steht der Grund in error.
    virtual bool load(int track, int index, std::wstring& error) = 0;
    // Entfernt das Plugin der Spur.
    virtual void unload(int track) = 0;
    // Oeffnet das Fenster des Plugins der Spur.
    virtual bool openEditor(int track, std::wstring& error) = 0;
};

}  // namespace ui
