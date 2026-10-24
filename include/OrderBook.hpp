#pragma once

/**
 * @file OrderBook.hpp
 * @brief Order book data structures and thread-safe store.
 *
 * The engine subscribes to Binance @depth5 streams, which provide the top 5
 * price levels on each side of the order book. This file defines the structures
 * that hold those levels and the thread-safe store that keeps them up to date.
 *
 * ### Why order book depth matters
 *
 * A naive arbitrage detector uses only the best bid/ask price, which assumes
 * infinite liquidity at that price. In reality, large orders "eat through"
 * multiple price levels, worsening the average fill price. By storing 5 levels
 * per side, the SimulationEngine can estimate the true cost of executing a
 * trade at a given notional size.
 *
 * ### Thread safety
 *
 * OrderBookStore is written by the WebSocket callback thread and read by the
 * scanner thread concurrently. Access is synchronised with a `std::shared_mutex`:
 *
 * - **Reads** (findEdgeInfo, getBook) acquire a `std::shared_lock` — multiple
 *   reader threads can proceed in parallel without blocking each other.
 * - **Writes** (registerSymbol, updateBook) acquire a `std::unique_lock` —
 *   exclusive access ensures no reader sees a partially written snapshot.
 *
 * This is a deliberate improvement over a plain `std::mutex`: in a high-throughput
 * scenario the scanner thread reads many symbols per scan cycle while the WebSocket
 * thread writes one symbol at a time. Allowing concurrent reads eliminates the
 * read-read contention that a plain mutex would introduce.
 */

#include <vector>
#include <string>
#include <unordered_map>
#include <mutex>
#include <shared_mutex>
#include <chrono>

using Clock     = std::chrono::steady_clock;
using TimePoint = std::chrono::time_point<Clock>;

/**
 * @brief A single price level in the order book.
 *
 * Represents one row in the order book — a price at which someone is willing
 * to buy or sell, and the total quantity available at that price.
 *
 * Quantities are always expressed in **base currency units**
 * (e.g. ETH for the ETHUSDT book).
 */
struct PriceLevel {
    double price; ///< Price in quote currency (e.g. 3200.50 USDT per ETH)
    double qty;   ///< Available quantity in base currency (e.g. 0.5 ETH)
};

/**
 * @brief Sanity check for a top-of-book quote before it enters the graph.
 *
 * Rejects non-positive prices (log(0) / log(negative) would corrupt the
 * Bellman-Ford distances) and crossed books (bid >= ask). A crossed book
 * would let the engine "buy" below the price it "sells" at, producing a
 * phantom arbitrage cycle out of a stale or out-of-order message.
 */
inline bool isSaneTopOfBook(double bestBid, double bestAsk) {
    return bestBid > 0.0 && bestAsk > 0.0 && bestBid < bestAsk;
}

/**
 * @brief A snapshot of one symbol's order book (up to 5 levels per side).
 *
 * Updated on every @depth5 WebSocket message for the corresponding symbol.
 *
 * - @c asks: sorted **ascending** — best (lowest) ask is @c asks[0]
 * - @c bids: sorted **descending** — best (highest) bid is @c bids[0]
 */
struct OrderBook {
    std::vector<PriceLevel> asks; ///< Sell-side levels, best price first
    std::vector<PriceLevel> bids; ///< Buy-side levels, best price first
    TimePoint lastUpdated;        ///< Timestamp of the most recent update
};

/**
 * @brief Describes which order book and which side to use for a graph edge.
 *
 * The SimulationEngine needs to know, for a given currency conversion
 * (e.g. USDT → ETH), which order book to look at and whether to walk
 * the ask side (buying base) or the bid side (selling base).
 */
struct EdgeBookInfo {
    std::string symbol;  ///< Symbol whose book to use, e.g. "ETHUSDT"
    bool        useAsks; ///< true = buying base (eat asks); false = selling base (eat bids)
};

/**
 * @brief Thread-safe store for all order book snapshots.
 *
 * Maintains two internal maps:
 * - `symbol → OrderBook`: the latest 5-level snapshot per trading pair
 * - `"FROM->TO" → EdgeBookInfo`: pre-computed lookup so the SimulationEngine
 *   can instantly find which book and side to use for any currency conversion
 *
 * ### Usage pattern
 *
 * 1. Call registerSymbol() once per symbol at startup (before any WebSocket data arrives)
 * 2. Call updateBook() from the WebSocket callback thread on every depth update
 * 3. Call findEdgeInfo() / getBook() from the scanner thread during simulation
 * ### Locking strategy
 *
 * Uses `std::shared_mutex` so that concurrent reads (scanner + any future
 * reader threads) proceed in parallel, while writes (WebSocket callback)
 * take an exclusive lock only for the duration of a single book update.
 */
class OrderBookStore {
private:
    mutable std::shared_mutex mutex;

    std::unordered_map<std::string, OrderBook>    books;      ///< symbol → book snapshot
    std::unordered_map<std::string, EdgeBookInfo> edgeLookup; ///< "FROM->TO" → book + side

public:

    /**
     * @brief Registers a symbol and pre-computes its edge-to-book mappings.
     *
     * For a symbol XYZABC (base=XYZ, quote=ABC), two entries are created:
     * - ABC → XYZ : use XYZABC asks (buying base costs ask price)
     * - XYZ → ABC : use XYZABC bids (selling base yields bid price)
     *
     * Must be called once per symbol **before** the WebSocket connects,
     * so that the first depth update can be stored immediately.
     *
     * @param symbol Uppercase symbol string, e.g. "ETHUSDT"
     * @param base   Base currency, e.g. "ETH"
     * @param quote  Quote currency, e.g. "USDT"
     */
    void registerSymbol(const std::string& symbol,
                        const std::string& base,
                        const std::string& quote) {
        std::unique_lock<std::shared_mutex> lock(mutex); // exclusive — write
        edgeLookup[quote + "->" + base] = {symbol, true};
        edgeLookup[base  + "->" + quote] = {symbol, false};
    }

    /**
     * @brief Replaces the stored order book for a symbol with a new snapshot.
     *
     * Called from the WebSocket callback thread on every @depth5 message.
     * Takes ownership of the level vectors via move semantics to avoid copying.
     *
     * @param symbol Uppercase symbol, e.g. "ETHUSDT"
     * @param asks   New ask levels (ascending price order)
     * @param bids   New bid levels (descending price order)
     */
    void updateBook(const std::string& symbol,
                    std::vector<PriceLevel> asks,
                    std::vector<PriceLevel> bids) {
        std::unique_lock<std::shared_mutex> lock(mutex); // exclusive — write
        books[symbol] = {std::move(asks), std::move(bids), Clock::now()};
    }

    /**
     * @brief Looks up which book and side to use for a currency conversion.
     *
     * @param from Source currency (e.g. "USDT")
     * @param to   Destination currency (e.g. "ETH")
     * @param out  Populated with the matching EdgeBookInfo if found
     * @return     true if the edge is registered, false otherwise
     */
    bool findEdgeInfo(const std::string& from, const std::string& to,
                      EdgeBookInfo& out) const {
        std::shared_lock<std::shared_mutex> lock(mutex); // shared — read
        auto it = edgeLookup.find(from + "->" + to);
        if (it == edgeLookup.end()) return false;
        out = it->second;
        return true;
    }

    /**
     * @brief Returns a copy of the current order book for a symbol.
     *
     * @param symbol Uppercase symbol, e.g. "ETHUSDT"
     * @param out    Populated with the book snapshot if found
     * @return       true if the symbol has a stored book, false otherwise
     */
    bool getBook(const std::string& symbol, OrderBook& out) const {
        std::shared_lock<std::shared_mutex> lock(mutex); // shared — read
        auto it = books.find(symbol);
        if (it == books.end()) return false;
        out = it->second;
        return true;
    }
};