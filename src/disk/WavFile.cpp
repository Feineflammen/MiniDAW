#include "disk/WavFile.h"

#include <algorithm>
#include <cmath>
#include <cstring>

// Hinweis: Der Code setzt Little-Endian voraus (x86/x64, ARM64 unter Windows).

namespace {

std::uint16_t readU16(const unsigned char* p) {
    std::uint16_t v;
    std::memcpy(&v, p, sizeof v);
    return v;
}

std::uint32_t readU32(const unsigned char* p) {
    std::uint32_t v;
    std::memcpy(&v, p, sizeof v);
    return v;
}

void putU16(std::vector<unsigned char>& out, std::uint16_t v) {
    out.push_back(static_cast<unsigned char>(v & 0xFF));
    out.push_back(static_cast<unsigned char>(v >> 8));
}

void putU32(std::vector<unsigned char>& out, std::uint32_t v) {
    for (int i = 0; i < 4; ++i) out.push_back(static_cast<unsigned char>((v >> (8 * i)) & 0xFF));
}

constexpr std::uint16_t kTagPcm = 1;
constexpr std::uint16_t kTagFloat = 3;
constexpr std::uint16_t kTagExtensible = 0xFFFE;

}  // namespace

std::shared_ptr<WavFile> WavFile::open(const std::filesystem::path& path, std::string& error) {
    std::shared_ptr<WavFile> wav(new WavFile());
    wav->file_.open(path, std::ios::binary);
    if (!wav->file_) { error = "Datei laesst sich nicht oeffnen"; return nullptr; }

    std::error_code ec;
    const auto fileSize = static_cast<std::int64_t>(std::filesystem::file_size(path, ec));
    if (ec) { error = "Dateigroesse nicht lesbar"; return nullptr; }

    unsigned char header[12];
    wav->file_.read(reinterpret_cast<char*>(header), sizeof header);
    if (!wav->file_ || std::memcmp(header, "RIFF", 4) != 0 || std::memcmp(header + 8, "WAVE", 4) != 0) {
        error = "Keine WAV-Datei (RIFF/WAVE-Kennung fehlt)";
        return nullptr;
    }

    bool haveFormat = false;
    bool haveData = false;
    std::int64_t dataSize = 0;
    std::int64_t pos = 12;

    while (pos + 8 <= fileSize) {
        unsigned char chunk[8];
        wav->file_.seekg(pos);
        wav->file_.read(reinterpret_cast<char*>(chunk), sizeof chunk);
        if (!wav->file_) break;
        const std::int64_t size = readU32(chunk + 4);
        const std::int64_t body = pos + 8;

        if (std::memcmp(chunk, "fmt ", 4) == 0) {
            unsigned char fmt[40] = {};
            const auto toRead = static_cast<std::streamsize>(std::min<std::int64_t>(size, 40));
            wav->file_.read(reinterpret_cast<char*>(fmt), toRead);
            if (!wav->file_ || toRead < 16) { error = "Format-Abschnitt ist beschaedigt"; return nullptr; }

            std::uint16_t tag = readU16(fmt);
            wav->channels_ = readU16(fmt + 2);
            wav->sampleRate_ = static_cast<int>(readU32(fmt + 4));
            wav->bits_ = readU16(fmt + 14);
            if (tag == kTagExtensible && toRead >= 26) tag = readU16(fmt + 24);  // Unterformat

            if (tag == kTagFloat && wav->bits_ == 32) {
                wav->isFloat_ = true;
            } else if (tag == kTagPcm && (wav->bits_ == 16 || wav->bits_ == 24 || wav->bits_ == 32)) {
                wav->isFloat_ = false;
            } else {
                error = "Nicht unterstuetztes WAV-Format (erlaubt: PCM 16/24/32 Bit, Float 32 Bit)";
                return nullptr;
            }
            if (wav->channels_ < 1 || wav->sampleRate_ < 1) {
                error = "Ungueltige Kanalzahl oder Samplerate";
                return nullptr;
            }
            wav->blockAlign_ = wav->channels_ * (wav->bits_ / 8);
            haveFormat = true;
        } else if (std::memcmp(chunk, "data", 4) == 0) {
            wav->dataOffset_ = body;
            // Manche Programme schreiben eine falsche Laenge, deshalb an der Dateigroesse begrenzen.
            dataSize = std::min<std::int64_t>(size, fileSize - body);
            haveData = true;
        }

        if (haveFormat && haveData) break;
        pos = body + size + (size & 1);  // Abschnitte sind auf gerade Laengen aufgefuellt
    }

    if (!haveFormat || !haveData) { error = "Format- oder Datenabschnitt fehlt"; return nullptr; }

    wav->frames_ = dataSize / wav->blockAlign_;
    wav->file_.clear();
    return wav;
}

void WavFile::readStereo(std::int64_t frame, int count, float* out) {
    if (count <= 0) return;
    std::fill(out, out + 2 * static_cast<std::size_t>(count), 0.0f);

    // Bereich auf die Datei begrenzen.
    std::int64_t first = frame;
    std::int64_t last = frame + count;  // exklusiv
    if (first < 0) first = 0;
    if (last > frames_) last = frames_;
    if (first >= last) return;

    const auto n = static_cast<std::size_t>(last - first);
    float* dst = out + 2 * static_cast<std::size_t>(first - frame);

    std::lock_guard<std::mutex> lock(mutex_);
    raw_.resize(n * static_cast<std::size_t>(blockAlign_));
    file_.clear();
    file_.seekg(dataOffset_ + first * blockAlign_);
    file_.read(reinterpret_cast<char*>(raw_.data()), static_cast<std::streamsize>(raw_.size()));
    const auto got = static_cast<std::size_t>(file_.gcount()) / static_cast<std::size_t>(blockAlign_);

    const int bytes = bits_ / 8;
    const int rightChannel = channels_ >= 2 ? 1 : 0;

    auto sample = [&](const unsigned char* p) -> float {
        if (isFloat_) {
            float v;
            std::memcpy(&v, p, sizeof v);
            return v;
        }
        switch (bits_) {
            case 16: {
                std::int16_t v;
                std::memcpy(&v, p, sizeof v);
                return static_cast<float>(v) / 32768.0f;
            }
            case 24: {
                std::int32_t v = p[0] | (p[1] << 8) | (p[2] << 16);
                if (v & 0x800000) v -= 0x1000000;  // Vorzeichen erweitern
                return static_cast<float>(v) / 8388608.0f;
            }
            default: {
                std::int32_t v;
                std::memcpy(&v, p, sizeof v);
                return static_cast<float>(static_cast<double>(v) / 2147483648.0);
            }
        }
    };

    for (std::size_t i = 0; i < got; ++i) {
        const unsigned char* p = raw_.data() + i * static_cast<std::size_t>(blockAlign_);
        dst[2 * i] = sample(p);
        dst[2 * i + 1] = sample(p + rightChannel * bytes);
    }
}

bool WavWriter::open(const std::filesystem::path& path, int channels, int sampleRate, std::string& error) {
    if (channels < 1 || sampleRate < 1) { error = "Ungueltige Parameter"; return false; }
    out_.open(path, std::ios::binary | std::ios::trunc);
    if (!out_) { error = "Datei laesst sich nicht anlegen"; return false; }
    channels_ = channels;
    frames_ = 0;

    std::vector<unsigned char> head;
    head.insert(head.end(), {'R', 'I', 'F', 'F'});
    putU32(head, 36);  // wird in finish() korrigiert
    head.insert(head.end(), {'W', 'A', 'V', 'E', 'f', 'm', 't', ' '});
    putU32(head, 16);
    putU16(head, kTagFloat);
    putU16(head, static_cast<std::uint16_t>(channels));
    putU32(head, static_cast<std::uint32_t>(sampleRate));
    putU32(head, static_cast<std::uint32_t>(sampleRate * channels * 4));
    putU16(head, static_cast<std::uint16_t>(channels * 4));
    putU16(head, 32);
    head.insert(head.end(), {'d', 'a', 't', 'a'});
    putU32(head, 0);  // wird in finish() korrigiert
    out_.write(reinterpret_cast<const char*>(head.data()), static_cast<std::streamsize>(head.size()));
    if (!out_) { error = "Schreibfehler"; return false; }
    return true;
}

bool WavWriter::write(const float* interleaved, std::int64_t frames) {
    if (!out_.is_open() || frames <= 0) return out_.is_open();
    out_.write(reinterpret_cast<const char*>(interleaved),
               static_cast<std::streamsize>(frames * channels_ * 4));
    frames_ += frames;
    return static_cast<bool>(out_);
}

bool WavWriter::finish() {
    if (!out_.is_open()) return false;
    const std::int64_t dataSize = std::min<std::int64_t>(frames_ * channels_ * 4, 0xFFFFFFFFll - 36);
    std::vector<unsigned char> size;
    putU32(size, static_cast<std::uint32_t>(36 + dataSize));
    out_.seekp(4);
    out_.write(reinterpret_cast<const char*>(size.data()), 4);
    size.clear();
    putU32(size, static_cast<std::uint32_t>(dataSize));
    out_.seekp(40);
    out_.write(reinterpret_cast<const char*>(size.data()), 4);
    out_.flush();
    const bool ok = static_cast<bool>(out_);
    out_.close();
    return ok;
}

bool writeWav(const std::filesystem::path& path, const float* interleaved, std::int64_t frames,
              int channels, int sampleRate, WavFormat format, std::string& error) {
    if (frames < 0 || channels < 1 || sampleRate < 1) { error = "Ungueltige Parameter"; return false; }

    const int bytes = format == WavFormat::Pcm16 ? 2 : format == WavFormat::Pcm24 ? 3 : 4;
    const std::int64_t dataSize = frames * channels * bytes;
    if (dataSize > 0xFFFFFFFFll - 44) { error = "Datei waere groesser als 4 GB"; return false; }

    std::vector<unsigned char> head;
    head.insert(head.end(), {'R', 'I', 'F', 'F'});
    putU32(head, static_cast<std::uint32_t>(36 + dataSize));
    head.insert(head.end(), {'W', 'A', 'V', 'E', 'f', 'm', 't', ' '});
    putU32(head, 16);
    putU16(head, format == WavFormat::Float32 ? kTagFloat : kTagPcm);
    putU16(head, static_cast<std::uint16_t>(channels));
    putU32(head, static_cast<std::uint32_t>(sampleRate));
    putU32(head, static_cast<std::uint32_t>(sampleRate * channels * bytes));
    putU16(head, static_cast<std::uint16_t>(channels * bytes));
    putU16(head, static_cast<std::uint16_t>(bytes * 8));
    head.insert(head.end(), {'d', 'a', 't', 'a'});
    putU32(head, static_cast<std::uint32_t>(dataSize));

    std::ofstream out(path, std::ios::binary | std::ios::trunc);
    if (!out) { error = "Datei laesst sich nicht anlegen"; return false; }
    out.write(reinterpret_cast<const char*>(head.data()), static_cast<std::streamsize>(head.size()));

    const std::int64_t total = frames * channels;
    std::vector<unsigned char> buffer;
    constexpr std::int64_t kChunk = 16384;

    for (std::int64_t done = 0; done < total; done += kChunk) {
        const std::int64_t n = std::min(kChunk, total - done);
        buffer.clear();
        for (std::int64_t i = 0; i < n; ++i) {
            const float x = interleaved[done + i];
            if (format == WavFormat::Float32) {
                unsigned char b[4];
                std::memcpy(b, &x, 4);
                buffer.insert(buffer.end(), b, b + 4);
            } else if (format == WavFormat::Pcm16) {
                const double c = std::clamp(static_cast<double>(x), -1.0, 1.0);
                putU16(buffer, static_cast<std::uint16_t>(static_cast<std::int16_t>(std::lround(c * 32767.0))));
            } else {
                const double c = std::clamp(static_cast<double>(x), -1.0, 1.0);
                const auto v = static_cast<std::uint32_t>(static_cast<std::int32_t>(std::lround(c * 8388607.0)));
                buffer.push_back(static_cast<unsigned char>(v & 0xFF));
                buffer.push_back(static_cast<unsigned char>((v >> 8) & 0xFF));
                buffer.push_back(static_cast<unsigned char>((v >> 16) & 0xFF));
            }
        }
        out.write(reinterpret_cast<const char*>(buffer.data()), static_cast<std::streamsize>(buffer.size()));
    }

    out.flush();
    if (!out) { error = "Schreibfehler"; return false; }
    return true;
}
