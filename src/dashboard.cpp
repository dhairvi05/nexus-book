#include "dashboard.h"

#include <iostream>
#include <iomanip>
#include <sstream>
#include <thread>
#include <ctime>

// ============================================================================
// Construction
// ============================================================================
Dashboard::Dashboard(const OrderBookEngine& engine, int refreshIntervalMs)
    : engine_(engine), refreshIntervalMs_(refreshIntervalMs), running_(true) {}

void Dashboard::stop() {
    running_.store(false, std::memory_order_relaxed);
}

// ============================================================================
// formatPrice — converts fixed-point integer price (real * 100) back to a
// human-readable decimal string, e.g. 15050 -> "150.50"
// ============================================================================
std::string Dashboard::formatPrice(uint32_t fixedPointPrice) {
    uint32_t whole = fixedPointPrice / 100;
    uint32_t cents = fixedPointPrice % 100;
    std::ostringstream oss;
    oss << whole << '.' << std::setw(2) << std::setfill('0') << cents;
    return oss.str();
}

// ============================================================================
// formatTimestamp — converts epoch nanoseconds to HH:MM:SS.mmm
// ============================================================================
std::string Dashboard::formatTimestamp(uint64_t epochNanos) {
    using namespace std::chrono;
    auto totalNanos = nanoseconds(epochNanos);
    auto totalMillis = duration_cast<milliseconds>(totalNanos);
    auto seconds_part = duration_cast<seconds>(totalMillis);
    auto ms = totalMillis - duration_cast<milliseconds>(seconds_part);

    std::time_t t = static_cast<std::time_t>(seconds_part.count());
    std::tm tmStruct{};
#if defined(_WIN32)
    localtime_s(&tmStruct, &t);
#else
    localtime_r(&t, &tmStruct);
#endif

    std::ostringstream oss;
    oss << std::put_time(&tmStruct, "%H:%M:%S")
        << '.' << std::setw(3) << std::setfill('0') << ms.count();
    return oss.str();
}

// ============================================================================
// renderFrame
//
// Draws a fixed-layout canvas:
//   - Banner: ticker, spread, total matched volume
//   - Two-column depth ladder: Bids (green) | Asks (red)
//   - Live ticker stream of the last N trades
//
// Uses \033[2J\033[H to clear and home the cursor each frame, producing a
// non-flickering, continuously-refreshing terminal UI.
// ============================================================================
void Dashboard::renderFrame(const BookSnapshot& snapshot, int depthRows) {
    std::ostringstream out;

    out << ansi::kClearScreen;

    // ---- Banner -------------------------------------------------------
    out << ansi::kBold << ansi::kCyan
        << "==================== ORDER BOOK ENGINE :: " << snapshot.symbol
        << " ====================" << ansi::kReset << "\n\n";

    uint32_t bestBid = 0;
    uint32_t bestAsk = 0;
    bool haveBid = !snapshot.bids.empty();
    bool haveAsk = !snapshot.asks.empty();
    if (haveBid) bestBid = snapshot.bids.front().price;
    if (haveAsk) bestAsk = snapshot.asks.front().price;

    out << ansi::kBold << "Ticker: " << ansi::kReset << snapshot.symbol << "    ";

    out << ansi::kBold << "Spread: " << ansi::kReset;
    if (haveBid && haveAsk) {
        if (bestAsk >= bestBid) {
            out << "$" << formatPrice(bestAsk - bestBid);
        } else {
            out << ansi::kYellow << "CROSSED" << ansi::kReset;
        }
    } else {
        out << "N/A";
    }
    out << "    ";

    out << ansi::kBold << "Total Matched Volume: " << ansi::kReset
        << snapshot.totalMatchedVolume << "\n\n";

    // ---- Depth Ladder ---------------------------------------------------
    out << ansi::kBold
        << std::left << std::setw(20) << "BIDS (Buy)"
        << " | "
        << std::setw(20) << "ASKS (Sell)"
        << ansi::kReset << "\n";

    out << std::left << std::setw(10) << "Price" << std::setw(10) << "Volume"
        << " | "
        << std::setw(10) << "Price" << std::setw(10) << "Volume"
        << "\n";

    out << std::string(43, '-') << "\n";

    for (int i = 0; i < depthRows; ++i) {
        // Bid column
        if (i < static_cast<int>(snapshot.bids.size())) {
            const auto& level = snapshot.bids[i];
            out << ansi::kGreen
                << std::left << std::setw(10) << formatPrice(level.price)
                << std::setw(10) << level.quantity
                << ansi::kReset;
        } else {
            out << std::left << std::setw(10) << "" << std::setw(10) << "";
        }

        out << " | ";

        // Ask column
        if (i < static_cast<int>(snapshot.asks.size())) {
            const auto& level = snapshot.asks[i];
            out << ansi::kRed
                << std::left << std::setw(10) << formatPrice(level.price)
                << std::setw(10) << level.quantity
                << ansi::kReset;
        } else {
            out << std::left << std::setw(10) << "" << std::setw(10) << "";
        }

        out << "\n";
    }

    // ---- Live Ticker Stream ----------------------------------------------
    out << "\n" << ansi::kBold << ansi::kCyan
        << "---------------- LIVE MATCH STREAM (last "
        << snapshot.recentTrades.size() << ") ----------------"
        << ansi::kReset << "\n";

    if (snapshot.recentTrades.empty()) {
        out << "  (no trades yet)\n";
    } else {
        // Most recent first
        for (auto it = snapshot.recentTrades.rbegin(); it != snapshot.recentTrades.rend(); ++it) {
            out << "  [" << formatTimestamp(it->timestamp) << "] "
                << "BUY#" << it->buyOrderId << " <-> SELL#" << it->sellOrderId
                << "  @ " << ansi::kYellow << "$" << formatPrice(it->price) << ansi::kReset
                << "  x" << it->quantity << "\n";
        }
    }

    out << "\n" << ansi::kBold << "Refreshing every " << refreshIntervalMs_
        << "ms... (Ctrl+C to exit)" << ansi::kReset << "\n";

    // Single flush keeps the redraw atomic from the terminal's perspective,
    // minimizing flicker.
    std::cout << out.str() << std::flush;
}

// ============================================================================
// run — the dashboard's thread entry point
// ============================================================================
void Dashboard::run() {
    constexpr int kDepthRows = 10;

    while (running_.load(std::memory_order_relaxed)) {
        BookSnapshot snapshot = engine_.getSnapshot(kDepthRows, 5);
        renderFrame(snapshot, kDepthRows);

        std::this_thread::sleep_for(std::chrono::milliseconds(refreshIntervalMs_));
    }

    // Final frame on exit so the user sees the terminal state at shutdown.
    BookSnapshot finalSnapshot = engine_.getSnapshot(kDepthRows, 5);
    renderFrame(finalSnapshot, kDepthRows);
    std::cout << "\n" << ansi::kBold << "Dashboard stopped." << ansi::kReset << "\n";
}
