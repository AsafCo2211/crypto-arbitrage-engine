#pragma once

/**
 * @file StatsCollector.hpp
 * @brief Thread-safe runtime statistics collector and CSV exporter.
 *
 * Observability is a first-class concern in this engine. StatsCollector
 * answers the question: "What has the engine been doing for the past N minutes?"
 *
 * It maintains two independent sets of counters:
 * - **Window counters**: reset every 30 seconds — show activity rate per window
 * - **Cumulative counters**: never reset — show totals for the entire run
 *
 * Both sets are printed together in every periodic summary.
 *
 * ### Threading model
 *
 * Three threads interact with this object concurrently:
 * - **WebSocket thread** → recordTick(), recordRejectedQuote()
 * - **Scanner thread**   → recordScan(), recordOpportunity()
 * - **Summary thread**   → printSummary() (reads + resets window counters)
 *
 * All access is serialised by a single internal mutex.
 *
 * ### CSV export
 *
 * Every detected opportunity is appended to a CSV file immediately after
 * detection. The file is opened in append mode so multiple runs accumulate
 * data. Each row is flushed to disk instantly to prevent data loss if the
 * process is killed.
 */

#include <iostream>
#include <fstream>
#include <string>
#include <mutex>
#include <chrono>
#include <iomanip>
#include <sstream>
#include <filesystem>

using SysClock = std::chrono::system_clock;

/**
 * @brief Thread-safe collector for engine runtime metrics and CSV output.
 *
 * ### Typical usage
 * @code
 * StatsCollector stats;
 * stats.initCsv("opportunities.csv");
 *
 * // In WebSocket callback:
 * stats.recordTick();
 *
 * // In scanner thread:
 * stats.recordScan();
 * stats.recordOpportunity("USDT->ETH->BTC->USDT", 0.21);
 *
 * // In summary thread every 30s:
 * stats.printSummary();
 * @endcode
 */
class StatsCollector {
private:
    mutable std::mutex mutex;

    // ── Per-window counters (reset after each printSummary) ───────────────────
    long long wTicks         = 0; ///< Price updates received in this window
    long long wRejected      = 0; ///< Quotes rejected (ask/bid <= 0) this window
    long long wScans         = 0; ///< Bellman-Ford scans run this window
    long long wOpportunities = 0; ///< Opportunities reported this window
    double    wBestProfit    = 0.0;
    double    wTotalProfit   = 0.0;
    bool        wHasClosest  = false; ///< Any triangle evaluated this window?
    double      wClosestPct  = 0.0;   ///< Best net result of any triangle (may be < 0)
    std::string wClosestRoute;

    // ── Cumulative counters (never reset) ─────────────────────────────────────
    long long cTicks         = 0; ///< Total price updates since startup
    long long cRejected      = 0; ///< Total quotes rejected since startup
    long long cScans         = 0; ///< Total Bellman-Ford scans since startup
    long long cOpportunities = 0; ///< Total opportunities reported since startup
    double    cBestProfit    = 0.0;
    double    cTotalProfit   = 0.0;
    bool        cHasClosest  = false;
    double      cClosestPct  = 0.0;
    std::string cClosestRoute;

    // ── CSV ───────────────────────────────────────────────────────────────────
    std::ofstream csvFile;
    bool          csvOpen = false;

    /**
     * @brief Returns the current wall-clock time as "YYYY-MM-DD HH:MM:SS".
     * Used to timestamp CSV rows.
     */
    static std::string wallClockNow() {
        auto now   = SysClock::now();
        auto timer = SysClock::to_time_t(now);
        std::tm bt{};
#ifdef _WIN32
        localtime_s(&bt, &timer);
#else
        localtime_r(&timer, &bt);
#endif
        std::ostringstream oss;
        oss << std::put_time(&bt, "%Y-%m-%d %H:%M:%S");
        return oss.str();
    }

public:

    /**
     * @brief Opens (or creates) the CSV file and writes the header row.
     *
     * The file is opened in **append** mode so consecutive runs accumulate
     * rows rather than overwriting. The header is written only if the file
     * is empty (i.e. a brand-new file).
     *
     * Prints the **absolute path** to stdout so the user knows exactly
     * where to find the output file.
     *
     * @param path Path to the CSV file (default: "opportunities.csv")
     */
    void initCsv(const std::string& path = "opportunities.csv") {
        std::lock_guard<std::mutex> lock(mutex);

        csvFile.open(path, std::ios::out | std::ios::app);
        if (!csvFile.is_open()) {
            std::cerr << "[STATS] Could not open CSV file: " << path << '\n';
            return;
        }

        csvFile.seekp(0, std::ios::end);
        if (csvFile.tellp() == 0) {
            csvFile << "timestamp,route,profit_pct\n";
        }

        csvOpen = true;
        std::error_code ec;
        auto abs = std::filesystem::absolute(path, ec);
        std::cout << "[STATS] CSV logging to: "
                  << (ec ? path : abs.string()) << '\n';
    }

    // ── Recording methods ─────────────────────────────────────────────────────

    /**
     * @brief Records a valid price tick received from Binance.
     *
     * Called after a WebSocket message has been successfully parsed and
     * written to the market graph. Increments both window and cumulative
     * tick counters.
     */
    void recordTick() {
        std::lock_guard<std::mutex> lock(mutex);
        ++wTicks; ++cTicks;
    }

    /**
     * @brief Records a quote that was rejected due to invalid prices.
     *
     * A quote is rejected when ask <= 0 or bid <= 0, which would cause
     * log(0) or log(negative) in Bellman-Ford, corrupting the distance array.
     *
     * In normal market conditions this count stays at 0. A non-zero value
     * may indicate a data quality issue with the exchange feed.
     */
    void recordRejectedQuote() {
        std::lock_guard<std::mutex> lock(mutex);
        ++wRejected; ++cRejected;
    }

    /**
     * @brief Records one execution of the Bellman-Ford scan.
     *
     * Called at the start of every findArbitrage() invocation, regardless
     * of whether an opportunity is found. Useful for measuring scan throughput.
     */
    void recordScan() {
        std::lock_guard<std::mutex> lock(mutex);
        ++wScans; ++cScans;
    }

    /**
     * @brief Records a detected arbitrage opportunity and appends it to CSV.
     *
     * Updates profit tracking (best and total for averaging) and immediately
     * writes a row to the CSV file with the current timestamp, route, and
     * profit percentage. The file is flushed after every write.
     *
     * @param route     Human-readable route string, e.g. "USDT->ETH->BTC->USDT"
     * @param profitPct Net profit percentage (e.g. 0.21 for 0.21%)
     */
    void recordOpportunity(const std::string& route, double profitPct) {
        std::lock_guard<std::mutex> lock(mutex);

        ++wOpportunities; ++cOpportunities;
        wTotalProfit += profitPct; cTotalProfit += profitPct;
        if (profitPct > wBestProfit) wBestProfit = profitPct;
        if (profitPct > cBestProfit) cBestProfit = profitPct;

        if (csvOpen) {
            csvFile << wallClockNow() << ','
                    << route          << ','
                    << std::fixed << std::setprecision(4)
                    << profitPct      << '\n';
            csvFile.flush();
        }
    }

    /**
     * @brief Prints a two-part summary and resets window counters.
     *
     * Outputs two sections:
     * 1. **Last 30s** — activity since the previous printSummary() call
     * 2. **Total run** — cumulative activity since the engine started
     *
     * After printing, all window counters are reset to zero so the next
     * call represents a fresh 30-second window.
     *
     * Called by the summary thread every 30 seconds, and once more on exit.
     */
    /**
     * @brief Records the best three-leg cycle seen in one scan, profitable
     *        or not. Keeps the maximum per window and for the whole run.
     */
    void recordClosestCycle(const std::string& route, double profitPct) {
        std::lock_guard<std::mutex> lock(mutex);
        if (!wHasClosest || profitPct > wClosestPct) {
            wHasClosest = true; wClosestPct = profitPct; wClosestRoute = route;
        }
        if (!cHasClosest || profitPct > cClosestPct) {
            cHasClosest = true; cClosestPct = profitPct; cClosestRoute = route;
        }
    }

    void printSummary() {
        std::lock_guard<std::mutex> lock(mutex);

        double wAvg = (wOpportunities > 0) ? wTotalProfit / wOpportunities : 0.0;
        double cAvg = (cOpportunities > 0) ? cTotalProfit / cOpportunities : 0.0;

        std::cout << "\n[STATS] ──────────── Last 30s ────────────\n";
        std::cout << "[STATS] Ticks received:      " << wTicks         << '\n';
        std::cout << "[STATS] Quotes rejected:     " << wRejected      << '\n';
        std::cout << "[STATS] Bellman-Ford scans:  " << wScans         << '\n';
        std::cout << "[STATS] Opportunities found: " << wOpportunities << '\n';
        if (wHasClosest) printClosest(wClosestRoute, wClosestPct);
        if (wOpportunities > 0) {
            std::cout << std::fixed << std::setprecision(4);
            std::cout << "[STATS] Best profit:         " << wBestProfit << "%\n";
            std::cout << "[STATS] Avg  profit:         " << wAvg        << "%\n";
        }

        std::cout << "[STATS] ─────────── Total run ───────────\n";
        std::cout << "[STATS] Ticks received:      " << cTicks         << '\n';
        std::cout << "[STATS] Quotes rejected:     " << cRejected      << '\n';
        std::cout << "[STATS] Bellman-Ford scans:  " << cScans         << '\n';
        std::cout << "[STATS] Opportunities found: " << cOpportunities << '\n';
        if (cHasClosest) printClosest(cClosestRoute, cClosestPct);
        if (cOpportunities > 0) {
            std::cout << std::fixed << std::setprecision(4);
            std::cout << "[STATS] Best profit:         " << cBestProfit << "%\n";
            std::cout << "[STATS] Avg  profit:         " << cAvg        << "%\n";
        }
        std::cout << "[STATS] ─────────────────────────────────\n\n";

        // Reset window counters only — cumulative counters are never reset
        wTicks = wRejected = wScans = wOpportunities = 0;
        wBestProfit = wTotalProfit = 0.0;
        wHasClosest = false; wClosestPct = 0.0; wClosestRoute.clear();
    }

private:
    static void printClosest(const std::string& route, double pct) {
        std::ostringstream v;
        v << std::showpos << std::fixed << std::setprecision(4) << pct << "% net";
        std::cout << "[STATS] Closest cycle:       " << v.str()
                  << "  (" << route << ")\n";
    }
};