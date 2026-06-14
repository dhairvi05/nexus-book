#include "order_book.h"
#include "dashboard.h"

#include <thread>
#include <atomic>
#include <random>
#include <vector>
#include <mutex>
#include <chrono>
#include <iostream>

// ============================================================================
// High-concurrency stress test harness
//
// - 4 producer threads continuously submit random valid Buy/Sell orders.
// - 2 consumer threads continuously cancel/modify random active orders.
// - 1 dashboard thread renders the live book state.
//
// All threads run for a fixed duration, then everything is joined cleanly.
// ============================================================================

namespace {

constexpr const char* kSymbol = "AAPL";

// Price band: 145.00 .. 155.00 in fixed-point (price * 100)
constexpr uint32_t kMinPrice = 14500;
constexpr uint32_t kMaxPrice = 15500;

constexpr uint32_t kMinQty = 1;
constexpr uint32_t kMaxQty = 500;

// Global atomic order ID generator shared across producer threads.
std::atomic<uint64_t> g_nextOrderId{1};

// Shared registry of "known" order IDs that consumer threads can target.
// Protected by its own mutex since it's an auxiliary structure separate
// from the engine's internal locking.
std::vector<uint64_t> g_knownOrderIds;
std::mutex g_knownOrderIdsMutex;

void registerOrderId(uint64_t id) {
    std::lock_guard<std::mutex> lock(g_knownOrderIdsMutex);
    g_knownOrderIds.push_back(id);
    // Bound memory growth in long runs.
    constexpr size_t kMaxTracked = 20000;
    if (g_knownOrderIds.size() > kMaxTracked) {
        g_knownOrderIds.erase(g_knownOrderIds.begin(),
                               g_knownOrderIds.begin() + (g_knownOrderIds.size() - kMaxTracked));
    }
}

bool pickRandomKnownOrderId(std::mt19937& rng, uint64_t& outId) {
    std::lock_guard<std::mutex> lock(g_knownOrderIdsMutex);
    if (g_knownOrderIds.empty()) {
        return false;
    }
    std::uniform_int_distribution<size_t> dist(0, g_knownOrderIds.size() - 1);
    outId = g_knownOrderIds[dist(rng)];
    return true;
}

// ----------------------------------------------------------------------
// Producer thread: continuously submits random valid orders.
// ----------------------------------------------------------------------
void producerLoop(OrderBookEngine& engine, std::atomic<bool>& running, int threadIndex) {
    std::mt19937 rng(static_cast<unsigned>(std::chrono::steady_clock::now().time_since_epoch().count())
                      + threadIndex * 7919u);

    std::uniform_int_distribution<uint32_t> priceDist(kMinPrice, kMaxPrice);
    std::uniform_int_distribution<uint32_t> qtyDist(kMinQty, kMaxQty);
    std::bernoulli_distribution sideDist(0.5);

    while (running.load(std::memory_order_relaxed)) {
        uint64_t id = g_nextOrderId.fetch_add(1, std::memory_order_relaxed);
        bool isBuy = sideDist(rng);
        uint32_t price = priceDist(rng);
        uint32_t qty = qtyDist(rng);

        if (engine.addOrder(id, kSymbol, isBuy, price, qty)) {
            registerOrderId(id);
        }

        std::this_thread::sleep_for(std::chrono::microseconds(500));
    }
}

// ----------------------------------------------------------------------
// Consumer thread: continuously cancels or modifies random active orders.
// ----------------------------------------------------------------------
void consumerLoop(OrderBookEngine& engine, std::atomic<bool>& running, int threadIndex) {
    std::mt19937 rng(static_cast<unsigned>(std::chrono::steady_clock::now().time_since_epoch().count())
                      + threadIndex * 104729u);

    std::uniform_int_distribution<uint32_t> priceDist(kMinPrice, kMaxPrice);
    std::uniform_int_distribution<uint32_t> qtyDist(kMinQty, kMaxQty);
    std::bernoulli_distribution actionDist(0.5); // true = cancel, false = modify

    while (running.load(std::memory_order_relaxed)) {
        uint64_t targetId;
        if (pickRandomKnownOrderId(rng, targetId)) {
            if (actionDist(rng)) {
                engine.cancelOrder(targetId);
            } else {
                uint32_t newPrice = priceDist(rng);
                uint32_t newQty = qtyDist(rng);
                engine.modifyOrder(targetId, newPrice, newQty);
            }
        }

        std::this_thread::sleep_for(std::chrono::microseconds(800));
    }
}

} // namespace

// ============================================================================
// main
// ============================================================================
int main() {
    constexpr int kRunDurationSeconds = 15;
    constexpr int kNumProducers = 4;
    constexpr int kNumConsumers = 2;

    OrderBookEngine engine(kSymbol, "orderbook.wal");

    std::atomic<bool> running{true};

    // Dashboard runs on its own thread until `running` flips false.
    Dashboard dashboard(engine, /*refreshIntervalMs=*/150);
    std::thread dashboardThread([&dashboard]() { dashboard.run(); });

    // Spawn producer threads.
    std::vector<std::thread> producers;
    producers.reserve(kNumProducers);
    for (int i = 0; i < kNumProducers; ++i) {
        producers.emplace_back(producerLoop, std::ref(engine), std::ref(running), i);
    }

    // Spawn consumer threads.
    std::vector<std::thread> consumers;
    consumers.reserve(kNumConsumers);
    for (int i = 0; i < kNumConsumers; ++i) {
        consumers.emplace_back(consumerLoop, std::ref(engine), std::ref(running), i);
    }

    // Run the stress test for a fixed duration.
    std::this_thread::sleep_for(std::chrono::seconds(kRunDurationSeconds));

    // Signal shutdown.
    running.store(false, std::memory_order_relaxed);
    dashboard.stop();

    // Join everything cleanly — data-safe shutdown.
    for (auto& t : producers) t.join();
    for (auto& t : consumers) t.join();
    dashboardThread.join();

    // Final summary printed after the dashboard's terminal output settles.
    BookSnapshot finalSnapshot = engine.getSnapshot(10, 5);
    std::cout << "\n================ FINAL SUMMARY ================\n";
    std::cout << "Symbol: " << finalSnapshot.symbol << "\n";
    std::cout << "Total Matched Volume: " << finalSnapshot.totalMatchedVolume << "\n";
    std::cout << "Active Bid Levels: " << finalSnapshot.bids.size() << "\n";
    std::cout << "Active Ask Levels: " << finalSnapshot.asks.size() << "\n";
    std::cout << "Total Trades in Rolling History: " << finalSnapshot.recentTrades.size() << "\n";
    std::cout << "================================================\n";

    return 0;
}
