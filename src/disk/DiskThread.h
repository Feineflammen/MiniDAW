#pragma once

#include "engine/Engine.h"

#include <atomic>
#include <chrono>
#include <thread>

// Laesst die Vorlese-Arbeit der Engine in einem eigenen Thread laufen.
class DiskThread {
public:
    DiskThread() = default;
    ~DiskThread() { stop(); }
    DiskThread(const DiskThread&) = delete;
    DiskThread& operator=(const DiskThread&) = delete;

    void start(Engine& engine) {
        stop();
        running_.store(true);
        thread_ = std::thread([this, &engine] {
            while (running_.load(std::memory_order_relaxed)) {
                // Solange es Arbeit gibt, durcharbeiten; sonst kurz schlafen.
                // Der Ringpuffer haelt ueber eine Sekunde vor.
                if (!engine.serviceDisk())
                    std::this_thread::sleep_for(std::chrono::milliseconds(5));
            }
        });
    }

    void stop() {
        running_.store(false);
        if (thread_.joinable()) thread_.join();
    }

private:
    std::thread thread_;
    std::atomic<bool> running_{false};
};
