#pragma once

#include "disk/WavFile.h"
#include "engine/Engine.h"

#include <cstdint>
#include <functional>
#include <string>

// Rechnet das Projekt ohne Audiogeraet von Position 0 an in eine WAV-Datei
// (Stereo). Waehrenddessen darf niemand sonst die Engine antreiben: Der
// Audio-Thread und der Disk-Thread muessen angehalten sein.
//
// frames:    Laenge der Datei in Frames
// blockSize: Blocklaenge, hoechstens die bei Engine::prepare() genannte
// progress:  wird regelmaessig mit dem Anteil 0..1 gerufen; liefert es false, wird abgebrochen
//
// Danach steht der Transport, die Position ist unveraendert gegenueber vorher.
bool renderOffline(Engine& engine, std::int64_t frames, int blockSize, int sampleRate,
                   const std::filesystem::path& path, WavFormat format, std::string& error,
                   const std::function<bool(double)>& progress = {});
