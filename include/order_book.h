#pragma once

#include <cstdint>
#include <string>
#include <list>
#include <map>
#include <unordered_map>
#include <deque>
#include <shared_mutex>
#include <mutex>
#include <fstream>
#include <vector>

// ============================================================================
// Order Record
// ============================================================================
struct Order {
    uint64_t orderId;
    std::string symbol;
    bool isBuy;          // true = Bid, false = Ask
    uint32_t price;      // fixed-point: real_price * 100
    uint32_t quantity;   // remaining unfilled quantity
    uint64_t timestamp;  // epoch nanoseconds
};

// ============================================================================
// Price Level: all orders resting at a single price point.
// Using std::list gives O(1) push_back and O(1) erase via iterator.
// ============================================================================
struct PriceLevel {
    std::list<Order> orders;

    // Total resting quantity at this price level (sum of order quantities).
    uint64_t totalQuantity() const {
        uint64_t total = 0;
        for (const auto& o : orders) {
            total += o.quantity;
        }
        return total;
    }

    bool empty() const {
        return orders.empty();
    }
};

// ============================================================================
// Trade record produced by the matching engine
// ============================================================================
struct Trade {
    uint64_t buyOrderId;
    uint64_t sellOrderId;
    uint32_t price;
    uint32_t quantity;
    uint64_t timestamp;
};

// ============================================================================
// Snapshot structures used for read-only access (dashboard, depth queries)
// ============================================================================
struct PriceLevelSnapshot {
    uint32_t price;
    uint64_t quantity;
};

struct BookSnapshot {
    std::string symbol;
    std::vector<PriceLevelSnapshot> bids; // best bid first
    std::vector<PriceLevelSnapshot> asks; // best ask first
    uint64_t totalMatchedVolume;
    std::deque<Trade> recentTrades;
};

// ============================================================================
// Order Book Engine
// ============================================================================
class OrderBookEngine {
public:
    explicit OrderBookEngine(std::string symbol, std::string walPath = "orderbook.wal");
    ~OrderBookEngine();

    // Non-copyable, non-movable (owns a mutex and file stream)
    OrderBookEngine(const OrderBookEngine&) = delete;
    OrderBookEngine& operator=(const OrderBookEngine&) = delete;

    // Core API -------------------------------------------------------------
    bool addOrder(uint64_t id, const std::string& symbol, bool isBuy,
                  uint32_t price, uint32_t qty);

    bool cancelOrder(uint64_t id);

    bool modifyOrder(uint64_t id, uint32_t newPrice, uint32_t newQty);

    // Read-only access -------------------------------------------------------
    BookSnapshot getSnapshot(size_t depth = 10, size_t tradeHistory = 5) const;

    uint64_t getTotalMatchedVolume() const;

    // Returns true if both sides have at least one level, and fills outBid/outAsk.
    bool getBestBidAsk(uint32_t& outBid, uint32_t& outAsk) const;

    // For test harnesses: returns a snapshot copy of currently live order IDs.
    std::vector<uint64_t> getActiveOrderIds() const;

    const std::string& getSymbol() const { return symbol_; }

private:
    // Internal matching loop. Caller must already hold the unique lock.
    void matchOrders();

    // Write-ahead log. Caller must already hold the unique lock
    // (writeAheadLog itself does not lock the book mutex).
    void writeAheadLog(const std::string& operation, const Order& order);

    // Helper: erase an order (by side+price+iterator) and clean up the
    // price level / index if it becomes empty. Caller holds unique lock.
    void eraseOrderLocked(bool isBuy, uint32_t price,
                          std::list<Order>::iterator it);

private:
    std::string symbol_;

    // Bids sorted descending (best bid = highest price = begin())
    std::map<uint32_t, PriceLevel, std::greater<uint32_t>> bids_;

    // Asks sorted ascending (best ask = lowest price = begin())
    std::map<uint32_t, PriceLevel, std::less<uint32_t>> asks_;

    // orderId -> (side, price, iterator into that PriceLevel's list)
    struct IndexEntry {
        bool isBuy;
        uint32_t price;
        std::list<Order>::iterator it;
    };
    std::unordered_map<uint64_t, IndexEntry> orderIndex_;

    // Rolling trade history (bounded ring buffer)
    std::deque<Trade> tradeHistory_;
    static constexpr size_t kMaxTradeHistory = 1000;

    uint64_t totalMatchedVolume_ = 0;

    // Reader-writer lock guarding all book state above.
    mutable std::shared_mutex mutex_;

    // Write-ahead log file (writer access only, sequential appends)
    std::ofstream walFile_;
    mutable std::mutex walMutex_;
};
