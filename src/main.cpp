/**
 * @file main.cpp
 * @brief Entry point and component wiring for the Crypto Arbitrage Engine.
 *
 * Responsible for:
 * 1. Loading the configuration from config/config.json
 * 2. Building the symbol registry and registering order book edges
 * 3. Constructing and wiring all engine components
 * 4. Starting the WebSocket connection and spawning worker threads
 * 5. Printing a final stats summary on exit
 *
 * ### Threading model
 *
 * | Thread          | Role                                                    |
 * |-----------------|---------------------------------------------------------|
 * | Main thread     | Waits for Ctrl+C / SIGTERM or the --duration deadline   |
 * | WebSocket thread| IXWebSocket internal — fires callbacks on every tick    |
 * | Scanner thread  | Wakes on marketDirty flag, runs Bellman-Ford            |
 * | Summary thread  | Sleeps in 100ms increments, prints stats every 30s      |
 *
 * The market graph is shared between the WebSocket and scanner threads.
 * Access is serialised by `marketMutex`. The `marketDirty` atomic flag
 * allows the WebSocket thread to signal the scanner without taking the lock.
 */

#include <iostream>
#include <string>
#include <atomic>
#include <mutex>
#include <thread>
#include <chrono>
#include <unordered_map>
#include <csignal>
#include <cstdlib>
#include "Graph.hpp"
#include "BinanceClient.hpp"
#include "Config.hpp"
#include "StatsCollector.hpp"
#include "OrderBook.hpp"
#include "SimulationEngine.hpp"

namespace {
// Set from the signal handler; std::sig_atomic_t is the only type the
// standard guarantees is safe to write there.
volatile std::sig_atomic_t g_stopRequested = 0;

void onStopSignal(int) { g_stopRequested = 1; }

void printUsage(const char* prog) {
    std::cout << "Usage: " << prog << " [--duration SECONDS]\n"
              << "  --duration SECONDS  stop automatically after SECONDS"
                 " (default: run until Ctrl+C)\n";
}
} // namespace

int main(int argc, char* argv[]) {
    // ── Command line ─────────────────────────────────────────────────────────
    int durationSeconds = 0; // 0 = run until Ctrl+C / SIGTERM
    for (int i = 1; i < argc; ++i) {
        std::string arg = argv[i];
        if (arg == "--duration" && i + 1 < argc) {
            durationSeconds = std::atoi(argv[++i]);
            if (durationSeconds <= 0) {
                std::cerr << "--duration must be a positive number of seconds\n";
                return 1;
            }
        } else if (arg == "--help" || arg == "-h") {
            printUsage(argv[0]);
            return 0;
        } else {
            std::cerr << "Unknown argument: " << arg << "\n";
            printUsage(argv[0]);
            return 1;
        }
    }

    std::signal(SIGINT,  onStopSignal);
    std::signal(SIGTERM, onStopSignal);

    std::cout << "[SYSTEM] Crypto Arbitrage Engine starting...\n";

    // ── Load config ──────────────────────────────────────────────────────────
    // Fails loudly if the file is missing, malformed, or has invalid values.
    // Nothing else runs until config is validated.
    Config cfg;
    try {
        cfg = Config::load("config/config.json");
    } catch (const std::exception& e) {
        std::cerr << e.what() << '\n';
        return 1;
    }

    std::cout << "[CONFIG] fee="        << cfg.fee
              << " | minProfit="        << cfg.minProfitPercent << "%"
              << " | maxQuoteAge="      << cfg.maxQuoteAgeMs    << "ms"
              << " | cooldown="         << cfg.cooldownSeconds  << "s"
              << " | scanInterval="     << cfg.scanIntervalMs   << "ms"
              << " | source="           << cfg.sourceCurrency
              << " | symbols="          << cfg.symbols.size()
              << " | simNotionals=";
    for (size_t i = 0; i < cfg.simulationNotionals.size(); ++i) {
        std::cout << "$" << cfg.simulationNotionals[i];
        if (i + 1 < cfg.simulationNotionals.size()) std::cout << "/";
    }
    std::cout << "\n\n";

    // ── Build symbol registry and order book edge map ─────────────────────────
    // For each symbol, we register:
    //   - A symbolRegistry entry (uppercase symbol → {base, quote}) used by
    //     BinanceClient to parse incoming WebSocket messages
    //   - An edge lookup entry in OrderBookStore used by SimulationEngine to
    //     find which book and side corresponds to each currency conversion
    std::unordered_map<std::string, SymbolInfo> symbolRegistry;
    OrderBookStore bookStore;
    std::string    streams;

    for (size_t i = 0; i < cfg.symbols.size(); ++i) {
        const auto& s = cfg.symbols[i];

        std::string upper = s.stream;
        for (char& c : upper)
            c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));

        symbolRegistry[upper] = {s.base, s.quote};
        bookStore.registerSymbol(upper, s.base, s.quote);

        streams += s.stream + "@depth5";
        if (i < cfg.symbols.size() - 1) streams += '/';
    }

    // ── Construct and wire all components ─────────────────────────────────────
    StatsCollector   stats;
    SimulationEngine simEngine(bookStore, cfg.fee);
    Graph            market;
    BinanceClient    binance(cfg.fee);
    std::mutex       marketMutex;
    std::atomic<bool> marketDirty{false};
    std::atomic<bool> running{true};

    stats.initCsv("opportunities.csv");

    // Attach optional dependencies — all pointers, so null-safe if not set
    market.attachStats(stats);
    market.attachSim(simEngine);
    binance.attachStats(stats);
    binance.attachOrderBookStore(bookStore);
    binance.setSymbolRegistry(symbolRegistry);

    std::string url = "wss://stream.binance.com:9443/stream?streams=" + streams;
    binance.connect(url, market, marketMutex, marketDirty);

    // ── Scanner thread ────────────────────────────────────────────────────────
    // Sleeps until marketDirty is set by the WebSocket callback, then runs
    // Bellman-Ford with the latest prices. All configuration comes from cfg —
    // no hardcoded thresholds here.
    std::thread scanner([&]() {
        while (running.load(std::memory_order_relaxed)) {
            if (marketDirty.exchange(false, std::memory_order_relaxed)) {
                std::lock_guard<std::mutex> lock(marketMutex);
                market.findArbitrage(
                    cfg.sourceCurrency,
                    cfg.minProfitPercent,
                    cfg.maxQuoteAgeMs,
                    cfg.cooldownSeconds,
                    cfg.simulationNotionals
                );
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(cfg.scanIntervalMs));
        }
    });

    // ── Summary thread ────────────────────────────────────────────────────────
    // Sleeps in 100ms increments (not a single 30s sleep) so that when the
    // engine stops, the thread wakes within ~100ms instead of blocking
    // for the remainder of the 30-second window.
    std::thread summaryThread([&]() {
        int ticksLeft = 300; // 300 × 100ms = 30 seconds
        while (running.load(std::memory_order_relaxed)) {
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
            if (!running.load(std::memory_order_relaxed)) break;
            if (--ticksLeft == 0) {
                stats.printSummary();
                ticksLeft = 300;
            }
        }
    });

    std::cout << "[SYSTEM] Engine running — listening to "
              << cfg.symbols.size() << " pairs.\n";
    std::cout << "[SYSTEM] Stats summary every 30s. ";
    if (durationSeconds > 0)
        std::cout << "Stopping automatically after " << durationSeconds << "s.\n\n";
    else
        std::cout << "Press Ctrl+C to stop.\n\n";

    // Wait for a stop signal or the --duration deadline. Polling every
    // 100ms keeps the signal handler trivial (it only sets a flag).
    const auto deadline = std::chrono::steady_clock::now()
                        + std::chrono::seconds(durationSeconds);
    while (!g_stopRequested &&
           (durationSeconds == 0 || std::chrono::steady_clock::now() < deadline)) {
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }

    // ── Graceful shutdown ─────────────────────────────────────────────────────
    std::cout << "\n[SYSTEM] Stopping...\n";
    binance.disconnect();
    running.store(false, std::memory_order_relaxed);
    if (scanner.joinable())       scanner.join();
    if (summaryThread.joinable()) summaryThread.join();

    std::cout << "\n[SYSTEM] Final stats:\n";
    stats.printSummary();
    std::cout << "[SYSTEM] Engine stopped.\n";

    return 0;
}