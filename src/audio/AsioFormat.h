#pragma once

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstring>

// Sampleformate der ASIO-Treiber und ihre Umrechnung von und nach float.
// Ohne Windows-Abhaengigkeit, damit sich die Umrechnung ueberall testen laesst.
// Nur die "LSB"-Formate (Little Endian) kommen unter Windows vor.
namespace asio_format {

enum : long {
    kInt16 = 16,      // ASIOSTInt16LSB
    kInt24 = 17,      // ASIOSTInt24LSB, drei Bytes je Sample
    kInt32 = 18,      // ASIOSTInt32LSB
    kFloat32 = 19,    // ASIOSTFloat32LSB
    kFloat64 = 20,    // ASIOSTFloat64LSB
    kInt32in16 = 24,  // 32 Bit breit, aber nur 16, 18, 20 oder 24 Bit genutzt (rechtsbuendig)
    kInt32in18 = 25,
    kInt32in20 = 26,
    kInt32in24 = 27,
};

// Bytes je Sample, 0 = Format wird nicht unterstuetzt.
inline int bytesPerSample(long type) {
    switch (type) {
        case kInt16: return 2;
        case kInt24: return 3;
        case kInt32: case kFloat32: case kInt32in16: case kInt32in18: case kInt32in20: case kInt32in24: return 4;
        case kFloat64: return 8;
        default: return 0;
    }
}

inline bool supported(long type) { return bytesPerSample(type) != 0; }

namespace detail {

// Nutzbare Bits eines ganzzahligen Formats.
inline int intBits(long type) {
    switch (type) {
        case kInt16: case kInt32in16: return 16;
        case kInt32in18: return 18;
        case kInt32in20: return 20;
        case kInt24: case kInt32in24: return 24;
        default: return 32;
    }
}

inline std::int32_t toInt(float value, int bits) {
    const double full = std::ldexp(1.0, bits - 1);  // 2^(bits-1)
    const double scaled = std::clamp(static_cast<double>(value), -1.0, 1.0) * full;
    // long long: Bei 32 Bit wuerde +1.0 sonst ueber den Wertebereich von long laufen.
    const long long top = static_cast<long long>(full);
    return static_cast<std::int32_t>(std::clamp(std::llround(scaled), -top, top - 1));
}

}  // namespace detail

// Wandelt n Samples des Treibers nach float (-1..1).
inline void toFloat(long type, const void* source, float* target, int n) {
    const auto* bytes = static_cast<const unsigned char*>(source);
    switch (type) {
        case kFloat32:
            std::memcpy(target, source, static_cast<std::size_t>(n) * sizeof(float));
            return;
        case kFloat64:
            for (int i = 0; i < n; ++i) {
                double v;
                std::memcpy(&v, bytes + 8 * i, 8);
                target[i] = static_cast<float>(v);
            }
            return;
        case kInt16:
            for (int i = 0; i < n; ++i) {
                std::int16_t v;
                std::memcpy(&v, bytes + 2 * i, 2);
                target[i] = static_cast<float>(v) * (1.0f / 32768.0f);
            }
            return;
        case kInt24:
            for (int i = 0; i < n; ++i) {
                const unsigned char* p = bytes + 3 * i;
                // In die oberen 24 Bit schieben, dann vorzeichenrichtig zurueck.
                const auto raw = static_cast<std::uint32_t>(p[0]) << 8 | static_cast<std::uint32_t>(p[1]) << 16 |
                                 static_cast<std::uint32_t>(p[2]) << 24;
                const auto v = static_cast<std::int32_t>(raw) >> 8;
                target[i] = static_cast<float>(v) * (1.0f / 8388608.0f);
            }
            return;
        default: {
            if (!supported(type)) {
                std::fill(target, target + n, 0.0f);
                return;
            }
            const double scale = 1.0 / std::ldexp(1.0, detail::intBits(type) - 1);
            for (int i = 0; i < n; ++i) {
                std::int32_t v;
                std::memcpy(&v, bytes + 4 * i, 4);
                target[i] = static_cast<float>(static_cast<double>(v) * scale);
            }
            return;
        }
    }
}

// Wandelt n float-Samples in das Format des Treibers; Werte ausserhalb -1..1 werden begrenzt.
inline void fromFloat(long type, const float* source, void* target, int n) {
    auto* bytes = static_cast<unsigned char*>(target);
    switch (type) {
        case kFloat32:
            std::memcpy(target, source, static_cast<std::size_t>(n) * sizeof(float));
            return;
        case kFloat64:
            for (int i = 0; i < n; ++i) {
                const double v = source[i];
                std::memcpy(bytes + 8 * i, &v, 8);
            }
            return;
        case kInt16:
            for (int i = 0; i < n; ++i) {
                const auto v = static_cast<std::int16_t>(detail::toInt(source[i], 16));
                std::memcpy(bytes + 2 * i, &v, 2);
            }
            return;
        case kInt24:
            for (int i = 0; i < n; ++i) {
                const auto v = static_cast<std::uint32_t>(detail::toInt(source[i], 24));
                unsigned char* p = bytes + 3 * i;
                p[0] = static_cast<unsigned char>(v & 0xFF);
                p[1] = static_cast<unsigned char>((v >> 8) & 0xFF);
                p[2] = static_cast<unsigned char>((v >> 16) & 0xFF);
            }
            return;
        default: {
            if (!supported(type)) return;
            const int bits = detail::intBits(type);
            for (int i = 0; i < n; ++i) {
                const std::int32_t v = detail::toInt(source[i], bits);
                std::memcpy(bytes + 4 * i, &v, 4);
            }
            return;
        }
    }
}

}  // namespace asio_format
