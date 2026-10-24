#include <gtest/gtest.h>
#include "Graph.hpp"

#include <chrono>
#include <cmath>
#include <thread>

// ── Helpers ───────────────────────────────────────────────────────────────────

// Builds a small graph with a known profitable triangle:
//   USDT → ETH → BTC → USDT
// The rates are chosen so the cycle multiplier is > 1 even after fees,
// making it a genuine negative-weight cycle in log-space.
Graph buildArbitrageGraph(double fee = 0.0) {
    Graph g;
    // USDT → ETH: buy ETH at ask 100 USDT → rate = 1/100 = 0.01
    g.updateExchangeRate("USDT", "ETH",  0.01,   fee);
    // ETH → BTC: sell ETH for BTC at bid 0.075 BTC/ETH
    g.updateExchangeRate("ETH",  "BTC",  0.075,  fee);
    // BTC → USDT: sell BTC for USDT at bid 1400 USDT/BTC
    // 0.01 * 0.075 * 1400 = 1.05  → 5% theoretical profit
    g.updateExchangeRate("BTC",  "USDT", 1400.0, fee);
    return g;
}

// Builds a graph with no profitable cycle.
// Rates are perfectly consistent: ETH=3200, BTC=60000, ETH/BTC=3200/60000
// Any triangle USDT→ETH→BTC→USDT yields exactly 1.0 before fees,
// and less than 1.0 after fees — so no arbitrage exists.
Graph buildNoArbitrageGraph(double fee = 0.001) {
    Graph g;
    const double ethPrice = 3200.0;
    const double btcPrice = 60000.0;
    const double ethBtc   = ethPrice / btcPrice; // 0.05333...

    g.updateExchangeRate("USDT", "ETH",  1.0 / ethPrice, fee);
    g.updateExchangeRate("ETH",  "USDT", ethPrice,        fee);
    g.updateExchangeRate("ETH",  "BTC",  ethBtc,          fee);
    g.updateExchangeRate("BTC",  "ETH",  1.0 / ethBtc,    fee);
    g.updateExchangeRate("BTC",  "USDT", btcPrice,         fee);
    g.updateExchangeRate("USDT", "BTC",  1.0 / btcPrice,  fee);
    return g;
}

// ── Currency registry tests ───────────────────────────────────────────────────

TEST(GraphTest, AddNewCurrencyIncrementsVertexCount) {
    Graph g;
    EXPECT_EQ(g.getNumVertices(), 0);
    g.addCurrency("USDT");
    EXPECT_EQ(g.getNumVertices(), 1);
    g.addCurrency("ETH");
    EXPECT_EQ(g.getNumVertices(), 2);
}

TEST(GraphTest, AddDuplicateCurrencyDoesNotIncrement) {
    Graph g;
    g.addCurrency("USDT");
    g.addCurrency("USDT");
    EXPECT_EQ(g.getNumVertices(), 1);
}

TEST(GraphTest, UpdateExchangeRateAddsEdge) {
    Graph g;
    g.updateExchangeRate("USDT", "ETH", 0.01);
    EXPECT_EQ(g.getEdges().size(), 1u);
}

TEST(GraphTest, UpdateExchangeRateUpdatesExistingEdge) {
    Graph g;
    g.updateExchangeRate("USDT", "ETH", 0.01);
    g.updateExchangeRate("USDT", "ETH", 0.02); // update, not insert
    EXPECT_EQ(g.getEdges().size(), 1u);
    EXPECT_DOUBLE_EQ(g.getEdges()[0].weight, 0.02);
}

// ── Arbitrage detection tests ─────────────────────────────────────────────────

TEST(GraphTest, DetectsKnownArbitrageCycle) {
    // Redirect stdout to capture output
    testing::internal::CaptureStdout();

    Graph g = buildArbitrageGraph(0.0);
    // threshold=0, freshness=very large so no edge is filtered
    g.findArbitrage("USDT", 0.0, 999999999, 0);

    std::string output = testing::internal::GetCapturedStdout();
    EXPECT_NE(output.find("ARBITRAGE"), std::string::npos)
        << "Expected arbitrage to be detected but got:\n" << output;
}

TEST(GraphTest, StaleQuotesAreIgnored) {
    // Same profitable triangle, but every quote is older than the
    // freshness limit — a cycle built from stale prices must not fire.
    Graph g = buildArbitrageGraph(0.0);
    std::this_thread::sleep_for(std::chrono::milliseconds(50));

    testing::internal::CaptureStdout();
    g.findArbitrage("USDT", 0.0, /*maxQuoteAgeMs=*/10, 0);
    std::string output = testing::internal::GetCapturedStdout();

    EXPECT_EQ(output.find("ARBITRAGE"), std::string::npos)
        << "Arbitrage reported from quotes older than maxQuoteAgeMs";
}

TEST(GraphTest, NoFalsePositiveOnFairMarket) {
    testing::internal::CaptureStdout();

    Graph g = buildNoArbitrageGraph(0.001);
    g.findArbitrage("USDT", 0.0, 999999999, 0);

    std::string output = testing::internal::GetCapturedStdout();
    EXPECT_EQ(output.find("ARBITRAGE"), std::string::npos)
        << "False positive: arbitrage reported on fair market rates";
}

TEST(GraphTest, FeesEliminateSmallArbitrage) {
    testing::internal::CaptureStdout();

    // 5% theoretical profit, but with 2% fee per leg (3 legs = ~6%) it vanishes
    Graph g = buildArbitrageGraph(0.02);
    g.findArbitrage("USDT", 0.0, 999999999, 0);

    std::string output = testing::internal::GetCapturedStdout();
    EXPECT_EQ(output.find("ARBITRAGE"), std::string::npos)
        << "Arbitrage should be eliminated by high fees";
}

TEST(GraphTest, ProfitThresholdFiltersSmallOpportunities) {
    testing::internal::CaptureStdout();

    Graph g = buildArbitrageGraph(0.0); // 5% theoretical profit
    // Set threshold above 5% — should be filtered out
    g.findArbitrage("USDT", 10.0, 999999999, 0);

    std::string output = testing::internal::GetCapturedStdout();
    EXPECT_EQ(output.find("ARBITRAGE"), std::string::npos)
        << "Opportunity below threshold should not be reported";
}

TEST(GraphTest, MissingSourceCurrencyHandledGracefully) {
    testing::internal::CaptureStderr();
    Graph g;
    g.updateExchangeRate("ETH", "BTC", 0.05);
    // Should not crash — just print an error
    EXPECT_NO_THROW(g.findArbitrage("USDT", 0.0, 999999999, 0));
    testing::internal::GetCapturedStderr();
}

TEST(GraphTest, ZeroRateIsRejectedAndDoesNotCrash) {
    testing::internal::CaptureStdout();
    testing::internal::CaptureStderr();

    Graph g;
    g.updateExchangeRate("USDT", "ETH",  0.0,   0.001); // invalid
    g.updateExchangeRate("ETH",  "USDT", 3200.0, 0.001);

    EXPECT_NO_THROW(g.findArbitrage("USDT", 0.0, 999999999, 0));

    testing::internal::GetCapturedStdout();
    testing::internal::GetCapturedStderr();
}
// ── Closest cycle (bestTriangle) ─────────────────────────────────────────────

TEST(BestTriangleTest, FairMarketCostsExactlyTheFees) {
    // Perfectly consistent rates: the only thing a loop can lose is the
    // fee on each of its three legs → (1 - 0.001)^3 - 1 = -0.2997%.
    Graph g = buildNoArbitrageGraph(0.001);
    auto best = g.bestTriangle("USDT", 999999999);

    ASSERT_TRUE(best.has_value());
    const double expectedPct = (std::pow(1.0 - 0.001, 3) - 1.0) * 100.0;
    EXPECT_NEAR(best->profitPct, expectedPct, 1e-9);
    EXPECT_EQ(best->route.rfind("USDT->", 0), 0u);                 // starts at source
    EXPECT_EQ(best->route.substr(best->route.size() - 4), "USDT"); // ends at source
}

TEST(BestTriangleTest, MatchesKnownProfitableCycle) {
    // buildArbitrageGraph: 0.01 * 0.075 * 1400 = 1.05 → +5%.
    Graph g = buildArbitrageGraph(0.0);
    auto best = g.bestTriangle("USDT", 999999999);

    ASSERT_TRUE(best.has_value());
    EXPECT_NEAR(best->profitPct, 5.0, 1e-9);
    EXPECT_EQ(best->route, "USDT->ETH->BTC->USDT");
}

TEST(BestTriangleTest, PicksTheBetterOfTwoDirections) {
    // Both loop directions exist with slightly inconsistent ETH/BTC rates:
    //   USDT->ETH->BTC->USDT ≈ -0.92%,  USDT->BTC->ETH->USDT ≈ +0.14%.
    // bestTriangle must pick the second one.
    Graph g;
    g.updateExchangeRate("USDT", "ETH", 1.0 / 3200.0, 0.001);
    g.updateExchangeRate("ETH",  "BTC", 0.0530,       0.001);
    g.updateExchangeRate("BTC",  "USDT", 60000.0,     0.001);
    g.updateExchangeRate("USDT", "BTC", 1.0 / 60000.0, 0.001);
    g.updateExchangeRate("BTC",  "ETH", 1.0 / 0.0531,  0.001);
    g.updateExchangeRate("ETH",  "USDT", 3200.0,       0.001);

    auto best = g.bestTriangle("USDT", 999999999);
    ASSERT_TRUE(best.has_value());
    EXPECT_EQ(best->route, "USDT->BTC->ETH->USDT");
    // (1/60000)(1/0.0531)(3200) * 0.999^3 - 1
    const double expectedPct =
        ((1.0 / 60000.0) * (1.0 / 0.0531) * 3200.0 * std::pow(0.999, 3) - 1.0) * 100.0;
    EXPECT_NEAR(best->profitPct, expectedPct, 1e-9);
}

TEST(BestTriangleTest, IgnoresStaleQuotes) {
    Graph g = buildArbitrageGraph(0.0);
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
    EXPECT_FALSE(g.bestTriangle("USDT", /*maxQuoteAgeMs=*/10).has_value());
}

TEST(BestTriangleTest, UnknownSourceReturnsNothing) {
    Graph g = buildNoArbitrageGraph();
    EXPECT_FALSE(g.bestTriangle("EUR", 999999999).has_value());
}

TEST(StatsTest, SummaryShowsClosestCycleEvenWithNoOpportunity) {
    StatsCollector stats;
    stats.recordClosestCycle("USDT->ETH->BTC->USDT", -0.3);
    stats.recordClosestCycle("USDT->SOL->BTC->USDT", -0.2134); // better
    stats.recordClosestCycle("USDT->ADA->BTC->USDT", -0.5);

    testing::internal::CaptureStdout();
    stats.printSummary();
    std::string out = testing::internal::GetCapturedStdout();

    EXPECT_NE(out.find("Opportunities found: 0"), std::string::npos);
    EXPECT_NE(out.find("Closest cycle:       -0.2134% net  (USDT->SOL->BTC->USDT)"),
              std::string::npos) << out;
}
