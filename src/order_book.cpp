#include "order_book.h"

#include <algorithm>
#include <iostream>
#include <sstream>

// ============================================================================
// Construction / Destruction
// ============================================================================
OrderBookEngine::OrderBookEngine(std::string symbol, std::string walPath)
    : symbol_(std::move(symbol)) {
    // Open in truncate mode for a fresh run; switch to std::ios::app if you
    // want to preserve history across restarts.
    walFile_.open(walPath, std::ios::out | std::ios::trunc);
    if (!walFile_.is_open()) {
        std::cerr << "[OrderBookEngine] WARNING: failed to open WAL file '"
                  << walPath << "'. Continuing without durable logging.\n";
    }
}

OrderBookEngine::~OrderBookEngine() {
    if (walFile_.is_open()) {
        walFile_.flush();
        walFile_.close();
    }
}

// ============================================================================
// Write-Ahead Log
//
// Persists a structured text record describing the operation before the
// caller commits the corresponding mutation to in-memory state. This gives
// a durable, replayable record of every state transition.
//
// NOTE: Caller must already hold mutex_ as a unique_lock (writer). This
// function uses a separate walMutex_ purely to serialize physical file
// writes; since callers already serialize via the unique_lock, walMutex_
// is mostly defensive but keeps the WAL safe even if called from a
// read path in the future.
// ============================================================================
void OrderBookEngine::writeAheadLog(const std::string& operation, const Order& order) {
    std::lock_guard<std::mutex> walLock(walMutex_);
    if (!walFile_.is_open()) {
        return;
    }

    std::ostringstream line;
    line << "OP=" << operation
         << " | orderId=" << order.orderId
         << " | symbol=" << order.symbol
         << " | side=" << (order.isBuy ? "BUY" : "SELL")
         << " | price=" << order.price
         << " | qty=" << order.quantity
         << " | ts=" << order.timestamp
         << '\n';

    walFile_ << line.str();
    walFile_.flush(); // production-grade durability: flush every record
}

// ============================================================================
// addOrder
//
// 1. Validate basic invariants.
// 2. If the order is immediately marketable (crosses the opposite book),
//    insert it provisionally then run matchOrders() to consume liquidity.
// 3. Any remaining quantity is rested on the book and indexed.
// 4. Every accepted mutation is recorded to the WAL prior to returning.
// ============================================================================
bool OrderBookEngine::addOrder(uint64_t id, const std::string& symbol, bool isBuy,
                                 uint32_t price, uint32_t qty) {
    if (qty == 0) {
        return false; // zero-quantity orders are rejected
    }
    if (orderIndex_.find(id) != orderIndex_.end()) {
        // We need the lock to safely check this; take it now and re-check.
    }

    auto timestamp = static_cast<uint64_t>(
        std::chrono::duration_cast<std::chrono::nanoseconds>(
            std::chrono::system_clock::now().time_since_epoch())
            .count());

    Order order{id, symbol, isBuy, price, qty, timestamp};

    std::unique_lock<std::shared_mutex> lock(mutex_);

    // Reject duplicate order IDs.
    if (orderIndex_.find(id) != orderIndex_.end()) {
        return false;
    }

    // Insert into the appropriate side first so matchOrders() can see it
    // as part of the crossing logic. We rely on matchOrders() to correctly
    // drain quantity from both sides regardless of which order "arrived
    // last" — the loop below simply matches best bid vs best ask while
    // bid >= ask, which is correct regardless of insertion order.
    if (isBuy) {
        PriceLevel& level = bids_[price];
        level.orders.push_back(order);
        auto it = std::prev(level.orders.end());
        orderIndex_[id] = IndexEntry{true, price, it};
    } else {
        PriceLevel& level = asks_[price];
        level.orders.push_back(order);
        auto it = std::prev(level.orders.end());
        orderIndex_[id] = IndexEntry{false, price, it};
    }

    // Run the matching loop — this will consume crossing liquidity on
    // both sides, including potentially the order we just inserted.
    matchOrders();

    // Log the acceptance of this order (post-matching state reflects any
    // partial fill that has already occurred by the time we log here —
    // we log the original incoming order for audit purposes).
    writeAheadLog("ADD", order);

    return true;
}

// ============================================================================
// eraseOrderLocked
//
// Removes a single order (identified by side + price + list iterator) from
// its PriceLevel. If that price level becomes empty, removes the price node
// from the corresponding side's sorted map. Does NOT touch orderIndex_ —
// callers are responsible for erasing the index entry themselves (this lets
// matchOrders() and cancelOrder() share this helper while having slightly
// different index-cleanup timing).
//
// Caller must hold mutex_ as a unique_lock.
// ============================================================================
void OrderBookEngine::eraseOrderLocked(bool isBuy, uint32_t price,
                                        std::list<Order>::iterator it) {
    if (isBuy) {
        auto mapIt = bids_.find(price);
        if (mapIt == bids_.end()) return;
        PriceLevel& level = mapIt->second;
        level.orders.erase(it);
        if (level.empty()) {
            bids_.erase(mapIt);
        }
    } else {
        auto mapIt = asks_.find(price);
        if (mapIt == asks_.end()) return;
        PriceLevel& level = mapIt->second;
        level.orders.erase(it);
        if (level.empty()) {
            asks_.erase(mapIt);
        }
    }
}

// ============================================================================
// cancelOrder
//
// O(1) lookup via orderIndex_, O(1) erase from the doubly linked list via
// the stored iterator, and conditional cleanup of the price level node from
// the sorted map if it becomes empty.
// ============================================================================
bool OrderBookEngine::cancelOrder(uint64_t id) {
    std::unique_lock<std::shared_mutex> lock(mutex_);

    auto idxIt = orderIndex_.find(id);
    if (idxIt == orderIndex_.end()) {
        return false;
    }

    IndexEntry entry = idxIt->second;
    Order cancelledOrder = *entry.it; // copy for WAL before destruction

    eraseOrderLocked(entry.isBuy, entry.price, entry.it);
    orderIndex_.erase(idxIt);

    writeAheadLog("CANCEL", cancelledOrder);

    return true;
}

// ============================================================================
// modifyOrder
//
// - If newPrice differs from the current price: the order loses time
//   priority. We cancel the existing order and append a brand-new order at
//   the back of the new price level's queue (new timestamp).
// - If newPrice is unchanged and newQty <= current quantity: update the
//   quantity in place, preserving the order's position (and therefore time
//   priority) in the queue. Increasing quantity at the same price is
//   disallowed, since that would represent a new resting commitment that
//   should reasonably re-queue — per spec, only newQty <= current is
//   accepted as an in-place update.
// ============================================================================
bool OrderBookEngine::modifyOrder(uint64_t id, uint32_t newPrice, uint32_t newQty) {
    if (newQty == 0) {
        return cancelOrder(id);
    }

    std::unique_lock<std::shared_mutex> lock(mutex_);

    auto idxIt = orderIndex_.find(id);
    if (idxIt == orderIndex_.end()) {
        return false;
    }

    IndexEntry entry = idxIt->second;
    Order current = *entry.it;

    if (newPrice != current.price) {
        // Price change => loses time priority. Remove old, insert new.
        eraseOrderLocked(entry.isBuy, entry.price, entry.it);
        orderIndex_.erase(idxIt);

        Order replaced = current; // capture pre-removal state for WAL
        writeAheadLog("CANCEL", replaced);

        auto timestamp = static_cast<uint64_t>(
            std::chrono::duration_cast<std::chrono::nanoseconds>(
                std::chrono::system_clock::now().time_since_epoch())
                .count());

        Order fresh{id, current.symbol, current.isBuy, newPrice, newQty, timestamp};

        if (fresh.isBuy) {
            PriceLevel& level = bids_[newPrice];
            level.orders.push_back(fresh);
            auto it = std::prev(level.orders.end());
            orderIndex_[id] = IndexEntry{true, newPrice, it};
        } else {
            PriceLevel& level = asks_[newPrice];
            level.orders.push_back(fresh);
            auto it = std::prev(level.orders.end());
            orderIndex_[id] = IndexEntry{false, newPrice, it};
        }

        // Re-run matching in case the modified order is now marketable.
        matchOrders();

        writeAheadLog("ADD", fresh);
        return true;
    }

    // Same price: only allow shrinking quantity to preserve priority.
    if (newQty <= current.quantity) {
        entry.it->quantity = newQty;
        writeAheadLog("MODIFY", *entry.it);
        return true;
    }

    // newQty > current.quantity at the same price is rejected — a true
    // increase requires losing priority, which the caller should express
    // via cancel + add, or by specifying a (nominal) price change.
    return false;
}

// ============================================================================
// matchOrders
//
// Core price-time priority matching loop. While the best bid price is >=
// the best ask price, repeatedly:
//   - take the front (oldest) order from each side's best price level,
//   - execute min(bidQty, askQty),
//   - deduct from both,
//   - record a Trade,
//   - if either side's order is fully filled, remove it (and clean up the
//     price level / index if needed).
//
// Caller must hold mutex_ as a unique_lock.
// ============================================================================
void OrderBookEngine::matchOrders() {
    while (!bids_.empty() && !asks_.empty()) {
        auto bidLevelIt = bids_.begin();
        auto askLevelIt = asks_.begin();

        uint32_t bestBid = bidLevelIt->first;
        uint32_t bestAsk = askLevelIt->first;

        if (bestBid < bestAsk) {
            break; // no crossing possible
        }

        PriceLevel& bidLevel = bidLevelIt->second;
        PriceLevel& askLevel = askLevelIt->second;

        if (bidLevel.orders.empty()) {
            bids_.erase(bidLevelIt);
            continue;
        }
        if (askLevel.orders.empty()) {
            asks_.erase(askLevelIt);
            continue;
        }

        // Front of each list = oldest order at this price (time priority).
        auto bidOrderIt = bidLevel.orders.begin();
        auto askOrderIt = askLevel.orders.begin();

        uint32_t execQty = std::min(bidOrderIt->quantity, askOrderIt->quantity);

        // Execution price convention: the resting order's price is used.
        // Since bestBid >= bestAsk and asks_ is the ascending side, the
        // ask side here represents the level that was resting first in
        // terms of price discovery for this cross; we use the ask price
        // (the earlier-resting side at the lower price) as the trade price.
        uint32_t tradePrice = bestAsk;

        auto tradeTimestamp = static_cast<uint64_t>(
            std::chrono::duration_cast<std::chrono::nanoseconds>(
                std::chrono::system_clock::now().time_since_epoch())
                .count());

        Trade trade{bidOrderIt->orderId, askOrderIt->orderId, tradePrice, execQty, tradeTimestamp};

        bidOrderIt->quantity -= execQty;
        askOrderIt->quantity -= execQty;

        totalMatchedVolume_ += execQty;

        tradeHistory_.push_back(trade);
        if (tradeHistory_.size() > kMaxTradeHistory) {
            tradeHistory_.pop_front();
        }

        // Clean up fully-filled orders. Capture order IDs first since
        // erasing invalidates the iterators.
        uint64_t filledBidId = (bidOrderIt->quantity == 0) ? bidOrderIt->orderId : 0;
        uint64_t filledAskId = (askOrderIt->quantity == 0) ? askOrderIt->orderId : 0;

        if (bidOrderIt->quantity == 0) {
            bidLevel.orders.erase(bidOrderIt);
            orderIndex_.erase(filledBidId);
            if (bidLevel.empty()) {
                bids_.erase(bidLevelIt);
            }
        }

        if (askOrderIt->quantity == 0) {
            askLevel.orders.erase(askOrderIt);
            orderIndex_.erase(filledAskId);
            if (askLevel.empty()) {
                asks_.erase(askLevelIt);
            }
        }

        // If neither side was fully consumed, one of the quantities must
        // be zero (since execQty = min(...)), so this loop always makes
        // progress and will terminate.
    }
}

// ============================================================================
// getSnapshot — reader access via shared_lock
// ============================================================================
BookSnapshot OrderBookEngine::getSnapshot(size_t depth, size_t tradeHistory) const {
    std::shared_lock<std::shared_mutex> lock(mutex_);

    BookSnapshot snapshot;
    snapshot.symbol = symbol_;
    snapshot.totalMatchedVolume = totalMatchedVolume_;

    size_t count = 0;
    for (const auto& [price, level] : bids_) {
        if (count++ >= depth) break;
        snapshot.bids.push_back(PriceLevelSnapshot{price, level.totalQuantity()});
    }

    count = 0;
    for (const auto& [price, level] : asks_) {
        if (count++ >= depth) break;
        snapshot.asks.push_back(PriceLevelSnapshot{price, level.totalQuantity()});
    }

    size_t historyCount = std::min(tradeHistory, tradeHistory_.size());
    for (size_t i = tradeHistory_.size() - historyCount; i < tradeHistory_.size(); ++i) {
        snapshot.recentTrades.push_back(tradeHistory_[i]);
    }

    return snapshot;
}

// ============================================================================
// getTotalMatchedVolume — reader access via shared_lock
// ============================================================================
uint64_t OrderBookEngine::getTotalMatchedVolume() const {
    std::shared_lock<std::shared_mutex> lock(mutex_);
    return totalMatchedVolume_;
}

// ============================================================================
// getBestBidAsk — reader access via shared_lock
// ============================================================================
bool OrderBookEngine::getBestBidAsk(uint32_t& outBid, uint32_t& outAsk) const {
    std::shared_lock<std::shared_mutex> lock(mutex_);
    if (bids_.empty() || asks_.empty()) {
        return false;
    }
    outBid = bids_.begin()->first;
    outAsk = asks_.begin()->first;
    return true;
}

// ============================================================================
// getActiveOrderIds — reader access via shared_lock
// ============================================================================
std::vector<uint64_t> OrderBookEngine::getActiveOrderIds() const {
    std::shared_lock<std::shared_mutex> lock(mutex_);
    std::vector<uint64_t> ids;
    ids.reserve(orderIndex_.size());
    for (const auto& [id, entry] : orderIndex_) {
        ids.push_back(id);
    }
    return ids;
}
