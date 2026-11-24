#include "engine/OfflineRender.h"

#include "disk/Resampler.h"

#include <algorithm>
#include <cstdint>
#include <memory>
#include <vector>

bool renderOffline(Engine& engine, std::int64_t frames, int blockSize, int sampleRate,
                   const std::filesystem::path& path, WavFormat format, std::string& error,
                   const std::function<bool(double)>& progress, int fileRate) {
    if (frames <= 0 || blockSize <= 0) { error = "Es gibt nichts zu exportieren."; return false; }
    if (fileRate <= 0) fileRate = sampleRate;

    WavWriter writer;
    if (!writer.open(path, 2, fileRate, error, format)) return false;

    std::unique_ptr<Resampler> resampler;
    if (fileRate != sampleRate) resampler = std::make_unique<Resampler>(sampleRate, fileRate, 2);
    std::vector<float> converted;
    // Dither fuer 16 Bit: Summe zweier gleichverteilter Zufallszahlen, fester
    // Startwert, damit derselbe Export dieselbe Datei ergibt.
    std::uint32_t seed = 0x2545F491u;
    const auto uniform = [&seed] {
        seed = seed * 1664525u + 1013904223u;
        return static_cast<float>(seed >> 8) * (1.0f / 16777216.0f);
    };
    const auto writeBlock = [&](std::vector<float>& data, std::int64_t count) {
        if (format == WavFormat::Pcm16)
            for (std::int64_t i = 0; i < 2 * count; ++i)
                data[static_cast<std::size_t>(i)] += (uniform() - uniform()) * (1.0f / 32767.0f);
        return writer.write(data.data(), count);
    };

    const auto block = static_cast<std::size_t>(blockSize);
    std::vector<float> left(block), right(block), interleaved(block * 2);
    float* out[2] = {left.data(), right.data()};
    const auto serviceAll = [&engine] {
        while (engine.serviceDisk()) {}
    };
    const std::int64_t positionBefore = engine.position();

    // Vorbereiten: an den Anfang, Clips vorlesen lassen. Die Bloecke bei
    // stehendem Transport stossen das Vorlesen an und werden verworfen.
    engine.stop();
    engine.setOffline(true);
    engine.locate(0);
    for (int i = 0; i < 3; ++i) {
        engine.process(nullptr, 0, out, 2, blockSize);
        serviceAll();
    }

    engine.play();
    bool ok = true;
    std::int64_t done = 0;
    int blocks = 0;
    while (done < frames) {
        serviceAll();
        engine.process(nullptr, 0, out, 2, blockSize);
        const auto n = static_cast<std::size_t>(std::min<std::int64_t>(blockSize, frames - done));
        for (std::size_t i = 0; i < n; ++i) {
            interleaved[2 * i] = left[i];
            interleaved[2 * i + 1] = right[i];
        }
        bool written;
        if (resampler) {
            converted.clear();
            resampler->push(interleaved.data(), static_cast<std::int64_t>(n), converted);
            written = writeBlock(converted, static_cast<std::int64_t>(converted.size() / 2));
        } else {
            written = writeBlock(interleaved, static_cast<std::int64_t>(n));
        }
        if (!written) {
            error = "Fehler beim Schreiben der Datei.";
            ok = false;
            break;
        }
        done += static_cast<std::int64_t>(n);
        if (progress && ++blocks % 64 == 0 && !progress(static_cast<double>(done) / static_cast<double>(frames))) {
            error = "Abgebrochen.";
            ok = false;
            break;
        }
    }

    if (ok && resampler) {
        converted.clear();
        resampler->finish(converted);
        if (!writeBlock(converted, static_cast<std::int64_t>(converted.size() / 2))) {
            error = "Fehler beim Schreiben der Datei.";
            ok = false;
        }
    }

    // Aufraeumen: Transport anhalten, alte Position wiederherstellen.
    engine.stop();
    engine.process(nullptr, 0, out, 2, blockSize);
    engine.setOffline(false);
    engine.locate(positionBefore);
    engine.process(nullptr, 0, out, 2, blockSize);

    if (!writer.finish() && ok) {
        error = "Fehler beim Schreiben der Datei.";
        ok = false;
    }
    if (!ok) {
        std::error_code ec;
        std::filesystem::remove(path, ec);
    }
    return ok;
}
