#pragma once

#include <array>
#include <atomic>
#include <cstddef>
#include <type_traits>

// Die Auffuellung durch alignas ist gewollt, MSVC warnt sonst (C4324).
#ifdef _MSC_VER
#pragma warning(push)
#pragma warning(disable : 4324)
#endif

// Lock-freier Ringpuffer fuer genau einen schreibenden und einen lesenden Thread.
// Feste Groesse (Zweierpotenz), keine Allokation, push/pop warten nie.
template <typename T, std::size_t Capacity>
class SpscQueue {
    static_assert(Capacity >= 2 && (Capacity & (Capacity - 1)) == 0,
                  "Capacity muss eine Zweierpotenz sein");
    static_assert(std::is_trivially_copyable_v<T>, "T muss trivial kopierbar sein");

public:
    // Nur vom schreibenden Thread. false, wenn die Queue voll ist.
    bool push(const T& value) noexcept {
        const std::size_t w = write_.load(std::memory_order_relaxed);
        const std::size_t r = read_.load(std::memory_order_acquire);
        if (w - r == Capacity) return false;
        buffer_[w & (Capacity - 1)] = value;
        write_.store(w + 1, std::memory_order_release);
        return true;
    }

    // Nur vom lesenden Thread. false, wenn die Queue leer ist.
    bool pop(T& out) noexcept {
        const std::size_t r = read_.load(std::memory_order_relaxed);
        const std::size_t w = write_.load(std::memory_order_acquire);
        if (r == w) return false;
        out = buffer_[r & (Capacity - 1)];
        read_.store(r + 1, std::memory_order_release);
        return true;
    }

private:
    // Getrennte Cache-Zeilen, damit sich die beiden Threads nicht ausbremsen.
    alignas(64) std::atomic<std::size_t> write_{0};
    alignas(64) std::atomic<std::size_t> read_{0};
    alignas(64) std::array<T, Capacity> buffer_{};
};

#ifdef _MSC_VER
#pragma warning(pop)
#endif
