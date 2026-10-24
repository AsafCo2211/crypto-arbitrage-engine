#pragma once

/**
 * @file Graph.hpp
 * @brief Market graph and Bellman-Ford arbitrage detection engine.
 *
 * This is the core of the arbitrage detector. It models the cryptocurrency
 * market as a directed weighted graph and searches for negative-weight cycles
 * using the Bellman-Ford algorithm.
 *
 * ### Graph model
 *
 * - Each **currency** (BTC, ETH, USDT...) is a node.
 * - Each **trading pair** contributes two directed edges:
 *   - QUOTE → BASE with rate `1/ask` (buying base)
 *   - BASE → QUOTE with rate `bid`   (selling base)
 *
 * ### Arbitrage as a negative-weight cycle
 *
 * A triangular arbitrage route (e.g. USDT → ETH → BTC → USDT) is profitable
 * if the product of exchange rates along the route exceeds 1.0. To detect this
 * with Bellman-Ford, each edge weight is transformed:
 *
 * @code
 * weight = -log(rate × (1 - fee))
 * @endcode
 *
 * Under this transformation:
 * - A path's total weight = `-log(product of all rates)`
 * - A **negative-weight cycle** ↔ product of rates > 1.0 ↔ **profit**
 *
 * Bellman-Ford finds negative-weight cycles in O(V × E) time, making it
 * well-suited to the small graphs (10–20 currencies) used here.
 *
 * ### Guard rails
 *
 * Raw market data is noisy. Several layers prevent false positives:
 * - **Quote freshness**: edges not updated within `maxQuoteAgeMs` are skipped
 * - **Profit threshold**: cycles below `minProfitPercent` are not reported
 * - **Dedup/cooldown**: the same route is suppressed for `cooldownSeconds`
 * - **Zero-rate guard**: edges with `actualRate <= 0` skip `log()` entirely
 */

#include <iostream>
#include <vector>
#include <string>
#include <unordered_map>
#include <chrono>
#include <cmath>
#include <limits>
#include <algorithm>
#include <optional>
#include "StatsCollector.hpp"
#include "SimulationEngine.hpp"

using Clock     = std::chrono::steady_clock;
using TimePoint = std::chrono::time_point<Clock>;

/**
 * @brief A directed exchange-rate edge between two currencies.
 *
 * Represents one conversion direction in the market graph. The weight
 * stored here is the **raw exchange rate** — the log transformation is
 * applied on the fly during Bellman-Ford to avoid storing stale weights.
 */
struct Edge {
    int       source;      ///< Numeric ID of the currency being converted from
    int       destination; ///< Numeric ID of the currency being converted to
    double    weight;      ///< Raw exchange rate (e.g. 1 ETH = 3200 USDT → 3200)
    double    fee;         ///< Taker fee for this leg (e.g. 0.001 = 0.1%)
    TimePoint lastUpdated; ///< Timestamp of the most recent price update
};

/**
 * @brief Directed weighted graph of exchange rates with Bellman-Ford arbitrage detection.
 *
 * Internally, currencies are assigned integer IDs for O(1) edge lookups.
 * A hash map (`edgeIndex`) maps "src->dst" strings to indices in the edge
 * vector, enabling O(1) in-place updates when new prices arrive.
 *
 * ### Typical usage
 * @code
 * Graph market;
 * market.attachStats(stats);
 * market.attachSim(simEngine);
 *
 * // On every price tick (from BinanceClient):
 * market.updateExchangeRate("USDT", "ETH", 1.0 / ask, 0.001);
 * market.updateExchangeRate("ETH", "USDT", bid, 0.001);
 *
 * // On scanner thread:
 * market.findArbitrage("USDT", 0.1, 3000, 10, {100, 500, 1000});
 * @endcode
 */
/**
 * @brief A three-leg cycle and its net result after fees.
 *
 * profitPct is negative when the loop loses money — which, on a liquid
 * exchange, is almost always.
 */
struct CycleQuote {
    std::string route;     ///< e.g. "USDT->ETH->BTC->USDT"
    double      profitPct; ///< Net profit in %, after the fee on every leg
};

class Graph {
private:
    int numVertices;
    std::vector<Edge> edges;
    std::unordered_map<std::string, size_t> edgeIndex; ///< "src->dst" → edges[] index

    std::unordered_map<std::string, int> currencyToIndex; ///< "ETH" → 1
    std::vector<std::string>             indexToCurrency; ///< 1 → "ETH"

    /// Maps cycle signature → last reported time (for dedup/cooldown)
    std::unordered_map<std::string, TimePoint> lastReported;

    StatsCollector*   stats = nullptr; ///< Optional stats recorder
    SimulationEngine* sim   = nullptr; ///< Optional slippage simulator

    static constexpr bool DEBUG_GRAPH_UPDATES = false;

    /// Builds the internal edge key for the edge index map.
    std::string makeEdgeKey(int src, int dst) const {
        return std::to_string(src) + "->" + std::to_string(dst);
    }

    /**
     * @brief Builds a human-readable route string for a cycle.
     *
     * The cycle vector is stored in reverse order (from the Bellman-Ford
     * predecessor walk), so this iterates in reverse to produce a natural
     * left-to-right route string.
     *
     * @param cycle Vector of node IDs forming the cycle (reverse order)
     * @return Route string, e.g. "USDT->ETH->BTC->USDT"
     */
    std::string cycleSignature(const std::vector<int>& cycle) const {
        std::string sig;
        for (int i = static_cast<int>(cycle.size()) - 1; i >= 0; --i) {
            sig += indexToCurrency[cycle[i]];
            if (i > 0) sig += "->";
        }
        return sig;
    }

public:
    Graph() : numVertices(0) {}

    /**
     * @brief Attaches a StatsCollector to record scan and opportunity events.
     * @param collector Reference to the shared stats object (must outlive this)
     */
    void attachStats(StatsCollector& collector) { stats = &collector; }

    /**
     * @brief Attaches a SimulationEngine to run slippage simulation on each detected cycle.
     * @param engine Reference to the simulation engine (must outlive this)
     */
    void attachSim(SimulationEngine& engine) { sim = &engine; }

    /**
     * @brief Adds a currency to the graph if it does not already exist.
     *
     * Assigns the next available integer ID and stores the bidirectional
     * mapping between the name and its ID.
     *
     * @param currency Currency name, e.g. "ETH"
     * @return Numeric ID of the currency (new or existing)
     */
    int addCurrency(const std::string& currency) {
        if (currencyToIndex.find(currency) == currencyToIndex.end()) {
            currencyToIndex[currency] = numVertices;
            indexToCurrency.push_back(currency);
            numVertices++;
        }
        return currencyToIndex[currency];
    }

    /**
     * @brief Adds or updates a directed exchange-rate edge.
     *
     * If an edge from @p from to @p to already exists, its weight, fee, and
     * timestamp are updated in-place (O(1) via the edge index map). Otherwise,
     * a new edge is appended and registered.
     *
     * The timestamp is always refreshed to the current time so quote-freshness
     * checks in findArbitrage() work correctly.
     *
     * @param from Source currency, e.g. "USDT"
     * @param to   Destination currency, e.g. "ETH"
     * @param rate Raw exchange rate (e.g. 0.000312 for USDT→ETH at 3200 USDT/ETH)
     * @param fee  Taker fee for this leg (e.g. 0.001 = 0.1%)
     */
    void updateExchangeRate(const std::string& from, const std::string& to,
                            double rate, double fee = 0.0) {
        int u = addCurrency(from);
        int v = addCurrency(to);

        std::string key = makeEdgeKey(u, v);
        auto it = edgeIndex.find(key);

        if (it != edgeIndex.end()) {
            Edge& edge    = edges[it->second];
            edge.weight      = rate;
            edge.fee         = fee;
            edge.lastUpdated = Clock::now();

            if (DEBUG_GRAPH_UPDATES) {
                std::cout << "[GRAPH] Updated: " << from << " -> " << to
                          << " | rate=" << rate << " | fee=" << fee << '\n';
            }
            return;
        }

        edges.push_back({u, v, rate, fee, Clock::now()});
        edgeIndex[key] = edges.size() - 1;

        if (DEBUG_GRAPH_UPDATES) {
            std::cout << "[GRAPH] Added: " << from << " -> " << to
                      << " | rate=" << rate << " | fee=" << fee << '\n';
        }
    }

    /**
     * @brief Evaluates every three-leg loop source → A → B → source and
     *        returns the one with the best net result, profitable or not.
     *
     * Bellman-Ford only answers "is there a profitable cycle?". When the
     * answer is no — the normal case on a liquid market — this answers
     * "how close did the market get?", which is what makes a live run
     * informative. Only edges fresher than maxQuoteAgeMs are used.
     *
     * Cost is O(E²) edge pairs with hash lookups for the closing leg —
     * about 1,600 pairs for the default 20-symbol market.
     *
     * @return std::nullopt if the source is unknown or no fresh triangle
     *         through it exists.
     */
    std::optional<CycleQuote> bestTriangle(const std::string& sourceCurrency,
                                           int maxQuoteAgeMs) const {
        auto srcIt = currencyToIndex.find(sourceCurrency);
        if (srcIt == currencyToIndex.end()) return std::nullopt;
        const int  s   = srcIt->second;
        const auto now = Clock::now();

        auto fresh = [&](const Edge& e) {
            return std::chrono::duration_cast<std::chrono::milliseconds>(
                       now - e.lastUpdated).count() <= maxQuoteAgeMs;
        };
        auto netRate = [](const Edge& e) { return e.weight * (1.0 - e.fee); };

        std::optional<CycleQuote> best;
        double bestMultiplier = 0.0;

        for (const auto& first : edges) {                 // s → a
            if (first.source != s || !fresh(first)) continue;
            const int a = first.destination;

            for (const auto& second : edges) {            // a → b
                if (second.source != a || second.destination == s ||
                    !fresh(second)) continue;
                const int b = second.destination;

                auto closeIt = edgeIndex.find(makeEdgeKey(b, s)); // b → s
                if (closeIt == edgeIndex.end()) continue;
                const Edge& third = edges[closeIt->second];
                if (!fresh(third)) continue;

                double m = netRate(first) * netRate(second) * netRate(third);
                if (!best || m > bestMultiplier) {
                    bestMultiplier = m;
                    best = CycleQuote{
                        indexToCurrency[s] + "->" + indexToCurrency[a] + "->" +
                        indexToCurrency[b] + "->" + indexToCurrency[s],
                        (m - 1.0) * 100.0};
                }
            }
        }
        return best;
    }

    int getNumVertices()                                  const { return numVertices; }
    const std::vector<Edge>& getEdges()                   const { return edges; }
    const std::vector<std::string>& getIndexToCurrency()  const { return indexToCurrency; }

    /// Prints all edges for debugging. Not called in production.
    void printGraph() const {
        std::cout << "\n--- Market Graph ---\n";
        for (const auto& edge : edges) {
            std::cout << indexToCurrency[edge.source] << " -> "
                      << indexToCurrency[edge.destination]
                      << " (Rate: " << edge.weight << ")\n";
        }
        std::cout << "--------------------\n\n";
    }

    /**
     * @brief Runs Bellman-Ford and reports any arbitrage cycles found.
     *
     * ### Algorithm
     *
     * **Step 1 — Relax edges V-1 times:**
     * For each edge, if the path through @p sourceCurrency to `edge.source`
     * plus the edge weight is shorter than the known distance to `edge.destination`,
     * update the distance and record the predecessor.
     *
     * Edges that are stale (older than @p maxQuoteAgeMs) or have a non-positive
     * effective rate are skipped to prevent false positives.
     *
     * **Step 2 — Detect negative-weight cycles:**
     * If any edge can still be relaxed after V-1 iterations, a negative-weight
     * cycle exists. The cycle is extracted by following predecessor links.
     *
     * **Step 3 — Report and simulate:**
     * If the cycle's profit exceeds @p minProfitPercent and the same route
     * has not been reported within @p cooldownSeconds, it is printed to stdout,
     * logged to CSV via StatsCollector, and passed to SimulationEngine.
     *
     * @param sourceCurrency   Starting node for Bellman-Ford (e.g. "USDT")
     * @param minProfitPercent Minimum net profit to report (%)
     * @param maxQuoteAgeMs    Skip edges not updated within this window (ms)
     * @param cooldownSeconds  Suppress re-reporting the same route within this window (s)
     * @param simNotionals     Trade sizes to pass to SimulationEngine (empty = skip sim)
     */
    void findArbitrage(const std::string& sourceCurrency,
                       double minProfitPercent        = 0.1,
                       int    maxQuoteAgeMs           = 3000,
                       int    cooldownSeconds         = 10,
                       const std::vector<double>& simNotionals = {}) {

        if (currencyToIndex.find(sourceCurrency) == currencyToIndex.end()) {
            std::cerr << "[ERROR] Source currency not found: " << sourceCurrency << '\n';
            return;
        }

        if (stats) {
            stats->recordScan();
            if (auto closest = bestTriangle(sourceCurrency, maxQuoteAgeMs))
                stats->recordClosestCycle(closest->route, closest->profitPct);
        }

        const int sourceNode = currencyToIndex[sourceCurrency];
        const auto now       = Clock::now();

        // ── Step 1: Relax edges V-1 times ───────────────────────────────────
        std::vector<double> dist(numVertices, std::numeric_limits<double>::infinity());
        std::vector<int>    pred(numVertices, -1);
        dist[sourceNode] = 0.0;

        for (int i = 0; i < numVertices - 1; ++i) {
            for (const auto& edge : edges) {
                auto ageMs = std::chrono::duration_cast<std::chrono::milliseconds>(
                                 now - edge.lastUpdated).count();
                if (ageMs > maxQuoteAgeMs) continue;

                double actualRate = edge.weight * (1.0 - edge.fee);
                if (actualRate <= 0.0) continue; // guard against log(0) / log(negative)

                double w = -std::log(actualRate);

                if (dist[edge.source] != std::numeric_limits<double>::infinity() &&
                    dist[edge.source] + w < dist[edge.destination]) {
                    dist[edge.destination] = dist[edge.source] + w;
                    pred[edge.destination] = edge.source;
                }
            }
        }

        // ── Step 2: Detect negative-weight cycles ────────────────────────────
        for (const auto& edge : edges) {
            auto ageMs = std::chrono::duration_cast<std::chrono::milliseconds>(
                             now - edge.lastUpdated).count();
            if (ageMs > maxQuoteAgeMs) continue;

            double actualRate = edge.weight * (1.0 - edge.fee);
            if (actualRate <= 0.0) continue;

            double w = -std::log(actualRate);

            if (dist[edge.source] == std::numeric_limits<double>::infinity() ||
                dist[edge.source] + w >= dist[edge.destination]) continue;

            // ── Cycle extraction ─────────────────────────────────────────────
            // Walk back V times from the cycle-triggering edge's destination
            // to guarantee we land inside the cycle, not on a path leading into it.
            int curr = edge.destination;
            for (int i = 0; i < numVertices; ++i) {
                if (pred[curr] == -1) {
                    std::cerr << "[WARN] Cycle extraction aborted: broken chain.\n";
                    return;
                }
                curr = pred[curr];
            }

            const int cycleStart = curr;
            std::vector<int> cycle;
            int maxSteps = numVertices;

            do {
                cycle.push_back(curr);
                if (pred[curr] == -1) {
                    std::cerr << "[WARN] Cycle extraction aborted mid-walk.\n";
                    return;
                }
                curr = pred[curr];
                --maxSteps;
            } while (curr != cycleStart && maxSteps > 0);

            if (curr != cycleStart) {
                std::cerr << "[WARN] Cycle extraction aborted: start not reached.\n";
                return;
            }
            cycle.push_back(cycleStart);

            // ── Profit calculation ────────────────────────────────────────────
            double profitMultiplier = 1.0;
            bool   edgeMissing      = false;

            for (int i = static_cast<int>(cycle.size()) - 1; i > 0; --i) {
                int from = cycle[i];
                int to   = cycle[i - 1];
                auto it  = edgeIndex.find(makeEdgeKey(from, to));
                if (it == edgeIndex.end()) {
                    std::cerr << "[WARN] Profit calc aborted: missing edge "
                              << indexToCurrency[from] << "->" << indexToCurrency[to] << '\n';
                    edgeMissing = true;
                    break;
                }
                const Edge& e = edges[it->second];
                profitMultiplier *= e.weight * (1.0 - e.fee);
            }

            if (edgeMissing) return;

            double profitPct = (profitMultiplier - 1.0) * 100.0;

            // ── Profit threshold ──────────────────────────────────────────────
            if (profitPct < minProfitPercent) return;

            // ── Dedup / cooldown ──────────────────────────────────────────────
            std::string sig = cycleSignature(cycle);
            auto dedupIt    = lastReported.find(sig);
            if (dedupIt != lastReported.end()) {
                auto elapsed = std::chrono::duration_cast<std::chrono::seconds>(
                                   now - dedupIt->second).count();
                if (elapsed < cooldownSeconds) return;
            }
            lastReported[sig] = now;

            // ── Report ────────────────────────────────────────────────────────
            std::cout << "\n[$$$] ARBITRAGE OPPORTUNITY DETECTED! [$$$]\n";
            std::cout << "[ROUTE]  " << sig << '\n';
            std::cout << "[PROFIT] Theoretical: " << profitPct << "%\n\n";

            // ── Slippage simulation ───────────────────────────────────────────
            if (sim && !simNotionals.empty()) {
                std::vector<std::pair<std::string, std::string>> steps;
                for (int i = static_cast<int>(cycle.size()) - 1; i > 0; --i) {
                    steps.emplace_back(indexToCurrency[cycle[i]],
                                       indexToCurrency[cycle[i - 1]]);
                }
                sim->simulate(steps, simNotionals, minProfitPercent);
            }

            std::cout << "-------------------------------------------\n\n";

            if (stats) stats->recordOpportunity(sig, profitPct);

            return;
        }
    }
};