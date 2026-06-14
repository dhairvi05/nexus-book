<h1 align="center">NexusBook - High-Concurrency Order Book Matching Engine</h1>
A production-grade, low-latency financial matching engine built in **Modern C++20** that handles high-throughput transaction streams via concurrent multi-threaded worker pools and renders real-time analytics dashboards.

This project blends **low-latency data structure engineering, reader-writer synchronization models, and write-ahead logging (WAL) durability design patterns** to achieve high-performance price discovery under extreme market contention.

---

## Features
* **Price-Time Priority Matching Engine**
  * Implements deterministic asset price discovery via custom-sorted data maps (`std::map` with custom comparators).
  * Prioritizes buying blocks descending (`std::greater`) and selling lists ascending (`std::less`) to perfectly mirror institutional trade desks.

* **O(1) Order Modification & Fast Cancellations**
  * Bypasses linear scanning loops by establishing an internal cross-referenced index tracking registry (`std::unordered_map`).
  * Maps unique order IDs directly to memory list iterators, enabling instantaneous $O(1)$ order drops or volume contractions under heavy traffic stress.

* **Data-Safe Shared Mutex Synchronization**
  * Employs a Reader-Writer lock model using `std::shared_mutex` to resolve multithreading resource collisions.
  * Allows parallel, non-blocking snapshot reads (`std::shared_lock`) for dashboard rendering workers while securing immediate, exclusive write access (`std::unique_lock`) for memory mutations.

* **Durable Write-Ahead Logging (WAL)**
  * Enforces the durability principle by flushing structured text transaction records down to local storage streams (`std::ofstream`) *prior* to finalizing in-memory mutations.
  * Provides a resilient, sequential audit trail capable of complete deterministic historical engine state replays.

* **Fixed-Point Cents Precision Arithmetic**
  * Eliminates binary rounding errors native to standard floating-point numbers (`float`/`double`).
  * Quantizes stock prices strictly as `uint32_t` integer pennies (e.g., Apple stock at `$148.51` is managed as `14851`), ensuring absolute mathematical accuracy across millions of execution operations.

---

## Tech Stack
* **Core Execution Framework** ![C++](https://img.shields.io/badge/C++-20-00599C?style=for-the-badge&logo=c%2B%2B&logoColor=white)

* **Build Orchestration Pipeline** ![CMake](https://img.shields.io/badge/CMake-3.16+-064F8C?style=for-the-badge&logo=cmake&logoColor=white)

* **Operating Environment** ![Ubuntu](https://img.shields.io/badge/Ubuntu-Linux-E95420?style=for-the-badge&logo=ubuntu&logoColor=white)
  ![WSL](https://img.shields.io/badge/WSL-Windows%20Subsystem%20for%20Linux-0078D4?style=for-the-badge&logo=windows&logoColor=white)

---

## Project Architecture & Workflow
1. **Ingest & Flood:** High-Frequency Trading (HFT) thread pools inject randomized order packets (Bids vs. Asks) directly into the execution matrix via a strict atomic transaction generator.
2. **Durable Log:** The core engine traps incoming operations, serializing metadata values directly to `orderbook.wal` before committing changes to volatile system memory.
3. **Queue Assignment:** Orders are routed to their respective price point nodes using linked sequence vectors (`std::list`) to anchor arrival order chronology.
4. **Price Discovery Loop:** The engine sweeps opposing queue boundary nodes. If `Best Bid >= Best Ask`, a transaction cross executes—draining inventory blocks from both nodes until the market spread returns to equilibrium.
5. **Asynchronous Snapshots:** A detached visualization thread worker awakens every 150 milliseconds, grabs a low-overhead shared state matrix, and safely extracts a thread-safe snapshot.
6. **Flicker-Free Display:** The rendering layer packages order book metrics into an internal string stream buffer (`std::ostringstream`) before making an single flush call to clean console visuals.

---

## Project Setup

Ensure you have your C++ compilers and standard build chains installed on your Linux or WSL distribution (`sudo apt install build-essential cmake`).

1. Clone the repository:
```bash
git clone [https://github.com/dhairvi05/nexus-book.git](https://github.com/dhairvi05/nexus-book.git)
cd nexus-book
