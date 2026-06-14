#pragma once

#include "order_book.h"
#include <atomic>
#include <chrono>

// ============================================================================
// Dashboard: asynchronous terminal renderer
//
// Runs on its own thread, periodically takes a read-only snapshot of the
// order book (via shared_lock inside OrderBookEngine::getSnapshot) and
// renders a fixed-layout ANSI terminal canvas.
// ============================================================================
class Dashboard {
public:
    // refreshIntervalMs should be in the 100-200ms range per spec.
    Dashboard(const OrderBookEngine& engine, int refreshIntervalMs = 150);

    // Entry point to be run on a dedicated thread (e.g. std::thread(&Dashboard::run, &dash)).
    void run();

    // Signal the dashboard loop to stop. Safe to call from another thread.
    void stop();

private:
    void renderFrame(const BookSnapshot& snapshot, int depthRows);

    static std::string formatPrice(uint32_t fixedPointPrice);
    static std::string formatTimestamp(uint64_t epochNanos);

private:
    const OrderBookEngine& engine_;
    int refreshIntervalMs_;
    std::atomic<bool> running_;
};

// ANSI escape code helpers (exposed for reuse/testing)
namespace ansi {
    constexpr const char* kClearScreen = "\033[2J\033[H";
    constexpr const char* kReset       = "\033[0m";
    constexpr const char* kGreen       = "\033[32m";
    constexpr const char* kRed         = "\033[31m";
    constexpr const char* kBold        = "\033[1m";
    constexpr const char* kCyan        = "\033[36m";
    constexpr const char* kYellow      = "\033[33m";
}
