#pragma once

/**
 * @file BinanceClient.hpp
 * @brief Binance WebSocket client for real-time order book data.
 *
 * Connects to the Binance combined stream endpoint and subscribes to
 * @depth5 streams for all configured trading pairs. On every message,
 * it parses the order book snapshot and performs two updates:
 *
 * 1. **Graph update** — writes the best bid/ask to the market graph so
 *    Bellman-Ford can run on the latest prices.
 *
 * 2. **Order book update** — stores all 5 price levels in OrderBookStore
 *    so SimulationEngine can estimate slippage for any trade size.
 *
 * ### Stream format
 *
 * Binance @depth5 messages look like:
 * @code{.json}
 * {
 *   "stream": "ethusdt@depth5",
 *   "data": {
 *     "asks": [["3200.00","0.5"], ["3200.50","1.2"], ...],
 *     "bids": [["3199.50","0.8"], ["3199.00","2.1"], ...]
 *   }
 * }
 * @endcode
 *
 * ### Edge direction convention
 *
 * For a symbol with base=ETH and quote=USDT:
 * - Buying ETH with USDT costs the **ask** price → edge USDT→ETH, rate = 1/ask
 * - Selling ETH for USDT yields the **bid** price → edge ETH→USDT, rate = bid
 *
 * This convention is consistent with how BinanceClient populates both the
 * graph and the order book store.
 */

#include <iostream>
#include <string>
#include <atomic>
#include <mutex>
#include <unordered_map>
#include <ixwebsocket/IXNetSystem.h>
#include <ixwebsocket/IXWebSocket.h>
#include <nlohmann/json.hpp>
#include "Graph.hpp"
#include "Config.hpp"
#include "StatsCollector.hpp"
#include "OrderBook.hpp"

/**
 * @brief Maps an uppercase symbol string to its base and quote currencies.
 *
 * Used as a lookup table in the WebSocket callback to identify which
 * currencies are involved in each incoming price update.
 *
 * Example: "ETHUSDT" → { base="ETH", quote="USDT" }
 */
struct SymbolInfo {
    std::string base;  ///< Base currency, e.g. "ETH"
    std::string quote; ///< Quote currency, e.g. "USDT"
};

/**
 * @brief WebSocket client that streams real-time order book data from Binance.
 *
 * Manages a single persistent WebSocket connection to the Binance combined
 * stream endpoint. Incoming depth snapshots are parsed and dispatched to
 * the market Graph and OrderBookStore.
 *
 * ### Lifecycle
 * 1. Construct with the taker fee (from Config)
 * 2. Call attachStats() and attachOrderBookStore() to wire dependencies
 * 3. Call setSymbolRegistry() with the uppercase symbol map
 * 4. Call connect() — this starts the WebSocket and the callback begins
 *    firing asynchronously on IXWebSocket's internal thread
 */
class BinanceClient {
private:
    ix::WebSocket webSocket;

    static constexpr bool DEBUG_TICKS  = false; ///< Set true to log every price tick
    static constexpr bool DEBUG_ERRORS = true;  ///< Log WebSocket errors and bad quotes

    std::unordered_map<std::string, SymbolInfo> symbolRegistry;

    double          fee;            ///< Taker fee from Config, applied to each graph edge
    StatsCollector* stats     = nullptr; ///< Optional; records ticks and rejections
    OrderBookStore* bookStore = nullptr; ///< Optional; receives depth snapshots

    /**
     * @brief Parses a Binance JSON level array into PriceLevel structs.
     *
     * Binance encodes each level as a two-element string array:
     * @code{.json}
     * ["3200.00", "0.500000"]
     * @endcode
     *
     * Levels with price <= 0 or qty <= 0 are silently skipped.
     *
     * @param arr JSON array of [price_string, qty_string] pairs
     * @return    Vector of PriceLevel structs (may be empty if all invalid)
     */
    static std::vector<PriceLevel> parseLevels(const nlohmann::json& arr) {
        std::vector<PriceLevel> levels;
        levels.reserve(arr.size());
        for (const auto& entry : arr) {
            double price = std::stod(entry[0].get<std::string>());
            double qty   = std::stod(entry[1].get<std::string>());
            if (price > 0.0 && qty > 0.0) {
                levels.push_back({price, qty});
            }
        }
        return levels;
    }

public:
    /**
     * @brief Constructs the client and initialises the IXWebSocket network layer.
     * @param fee Taker fee to apply to every graph edge (e.g. 0.001 = 0.1%)
     */
    explicit BinanceClient(double fee) : fee(fee) {
        ix::initNetSystem();
    }

    /**
     * @brief Attaches a StatsCollector to record ticks and rejected quotes.
     * @param collector Reference to the shared stats object (must outlive this)
     */
    void attachStats(StatsCollector& collector) { stats = &collector; }

    /**
     * @brief Attaches an OrderBookStore to receive 5-level depth snapshots.
     * @param store Reference to the shared order book store (must outlive this)
     */
    void attachOrderBookStore(OrderBookStore& store) { bookStore = &store; }

    /**
     * @brief Sets the symbol registry used to look up base/quote currencies.
     *
     * Keys must be uppercase (e.g. "ETHUSDT"). Typically populated from the
     * symbols list in Config, converted to uppercase in main.cpp.
     *
     * @param registry Map of uppercase symbol → SymbolInfo
     */
    void setSymbolRegistry(const std::unordered_map<std::string, SymbolInfo>& registry) {
        symbolRegistry = registry;
    }

    /**
     * @brief Connects to Binance and begins streaming market data.
     *
     * Registers the message callback and calls webSocket.start(). The callback
     * runs on IXWebSocket's internal thread and is responsible for:
     * - Parsing depth snapshots
     * - Validating ask/bid prices
     * - Updating the market graph (best bid/ask)
     * - Updating the order book store (all 5 levels)
     * - Signalling the scanner thread via the marketDirty flag
     *
     * @param url          Binance combined stream URL with all @depth5 streams
     * @param market       Market graph to update with best bid/ask prices
     * @param marketMutex  Mutex protecting the market graph
     * @param marketDirty  Atomic flag; set to true after each graph update
     */
    void connect(const std::string& url, Graph& market,
                 std::mutex& marketMutex, std::atomic<bool>& marketDirty) {

        webSocket.setUrl(url);

        webSocket.setOnMessageCallback(
            [this, &market, &marketMutex, &marketDirty](const ix::WebSocketMessagePtr& msg) {

            if (msg->type == ix::WebSocketMessageType::Message) {
                try {
                    auto j = nlohmann::json::parse(msg->str);
                    if (!j.contains("stream") || !j.contains("data")) return;

                    // Extract symbol from stream name: "ethusdt@depth5" → "ETHUSDT"
                    std::string streamName = j["stream"].get<std::string>();
                    auto atPos = streamName.find('@');
                    if (atPos == std::string::npos) return;

                    std::string symbolLower = streamName.substr(0, atPos);
                    std::string symbol;
                    symbol.reserve(symbolLower.size());
                    for (char c : symbolLower)
                        symbol += static_cast<char>(std::toupper(static_cast<unsigned char>(c)));

                    auto symIt = symbolRegistry.find(symbol);
                    if (symIt == symbolRegistry.end()) return;

                    const std::string& base  = symIt->second.base;
                    const std::string& quote = symIt->second.quote;

                    const auto& data = j["data"];
                    if (!data.contains("asks") || !data.contains("bids")) return;

                    auto asks = parseLevels(data["asks"]); // ascending price
                    auto bids = parseLevels(data["bids"]); // descending price

                    if (asks.empty() || bids.empty()) return;

                    double bestAsk = asks.front().price;
                    double bestBid = bids.front().price;

                    // Reject non-positive prices and crossed books — see
                    // isSaneTopOfBook() in OrderBook.hpp.
                    if (!isSaneTopOfBook(bestBid, bestAsk)) {
                        if (stats) stats->recordRejectedQuote();
                        return;
                    }

                    // Update order book store for slippage simulation
                    if (bookStore) {
                        bookStore->updateBook(symbol, std::move(asks), std::move(bids));
                    }

                    // Update graph with best bid/ask for Bellman-Ford:
                    //   quote → base : buying base costs ask  → rate = 1/ask
                    //   base  → quote: selling base yields bid → rate = bid
                    {
                        std::lock_guard<std::mutex> lock(marketMutex);
                        market.updateExchangeRate(quote, base, 1.0 / bestAsk, fee);
                        market.updateExchangeRate(base, quote, bestBid,        fee);
                    }
                    marketDirty.store(true, std::memory_order_relaxed);
                    if (stats) stats->recordTick();

                    if (DEBUG_TICKS) {
                        std::cout << "[TICK] " << symbol
                                  << " ask=" << bestAsk << " bid=" << bestBid << '\n';
                    }

                } catch (const std::exception& e) {
                    if (DEBUG_ERRORS)
                        std::cerr << "[ERROR] " << e.what() << '\n';
                }

            } else if (msg->type == ix::WebSocketMessageType::Open) {
                std::cout << "[SYSTEM] Connected to Binance depth stream.\n";

            } else if (msg->type == ix::WebSocketMessageType::Error) {
                std::cerr << "[ERROR] WebSocket: " << msg->errorInfo.reason << '\n';
            }
        });

        std::cout << "[SYSTEM] Connecting to " << url << " ...\n";
        webSocket.start();
    }

    /// Closes the WebSocket and stops its background thread. After this
    /// returns no more callbacks touch the graph or the order book.
    void disconnect() { webSocket.stop(); }
};