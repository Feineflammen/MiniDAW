#pragma once

#include <cstdint>
#include <filesystem>
#include <fstream>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

// Liest WAV-Dateien: PCM mit 16, 24 oder 32 Bit sowie 32-Bit-Float, Mono oder Stereo.
// Bei mehr als zwei Kanaelen werden die ersten beiden benutzt.
//
// Nicht fuer den Audio-Thread: readStereo() greift auf die Platte zu.
class WavFile {
public:
    static std::shared_ptr<WavFile> open(const std::filesystem::path& path, std::string& error);

    int sampleRate() const noexcept { return sampleRate_; }
    int channels() const noexcept { return channels_; }
    int bitsPerSample() const noexcept { return bits_; }
    bool isFloat() const noexcept { return isFloat_; }
    std::int64_t frames() const noexcept { return frames_; }

    // Liest count Frames ab Position frame nach out (L, R verschachtelt, also
    // 2 * count Werte). Mono wird auf beide Seiten gelegt. Was ausserhalb der
    // Datei liegt, wird Stille. Darf aus mehreren Threads gerufen werden.
    void readStereo(std::int64_t frame, int count, float* out);

private:
    WavFile() = default;

    std::mutex mutex_;
    std::ifstream file_;
    std::vector<unsigned char> raw_;
    std::int64_t dataOffset_ = 0;
    std::int64_t frames_ = 0;
    int sampleRate_ = 0;
    int channels_ = 0;
    int bits_ = 0;
    int blockAlign_ = 0;
    bool isFloat_ = false;
};

enum class WavFormat { Pcm16, Pcm24, Float32 };

// Schreibt eine WAV-Datei stueckweise, zum Beispiel waehrend einer Aufnahme
// oder beim Export. Die Laengenangaben im Kopf traegt finish() nach.
class WavWriter {
public:
    bool open(const std::filesystem::path& path, int channels, int sampleRate, std::string& error,
              WavFormat format = WavFormat::Float32);
    bool write(const float* interleaved, std::int64_t frames);
    bool finish();
    bool isOpen() const { return out_.is_open(); }
    std::int64_t frames() const noexcept { return frames_; }

private:
    std::ofstream out_;
    int channels_ = 0;
    std::int64_t frames_ = 0;
    WavFormat format_ = WavFormat::Float32;
    std::vector<unsigned char> buffer_;
};

// Schreibt verschachtelte Float-Daten als WAV-Datei.
bool writeWav(const std::filesystem::path& path, const float* interleaved, std::int64_t frames,
              int channels, int sampleRate, WavFormat format, std::string& error);
