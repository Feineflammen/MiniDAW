#pragma once

#include "vst3/Vst3Host.h"

#include <filesystem>
#include <memory>
#include <string>
#include <vector>

// Windows-Teil des VST3-Hosts: Plugin-Dateien finden und laden.
namespace vst3 {

struct PluginEntry {
    std::wstring name;           // Dateiname ohne Endung
    std::filesystem::path path;  // die .vst3-Datei oder der .vst3-Ordner
};

// Sucht Plugins im Standardordner (Gemeinsame Dateien\VST3), auch in
// Unterordnern. Die Dateien werden dabei nicht geladen.
std::vector<PluginEntry> scanPlugins();

// Der Ordner, in dem gesucht wird (fuer Hinweise an den Benutzer).
std::filesystem::path pluginFolder();

// Laedt das Plugin und richtet es ein. Enthaelt die Datei mehrere Plugins,
// wird das erste genommen. Die Datei bleibt bis zum Programmende geladen.
std::unique_ptr<Vst3Plugin> loadPlugin(const std::filesystem::path& path, double sampleRate, int maxFrames,
                                       std::string& error);

}  // namespace vst3
