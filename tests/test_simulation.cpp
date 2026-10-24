#include <gtest/gtest.h>
#include "SimulationEngine.hpp"
#include "OrderBook.hpp"

// ── Helpers ───────────────────────────────────────────────────────────────────

// std::mutex is neither copyable nor movable, so OrderBookStore cannot be
// returned by value. Instead we populate a store via a helper function that
// takes it by reference.
void populateDeepStore(OrderBookStore& store) {
    store.registerSymbol("ETHUSDT", "ETH",  "USDT");
    store.registerSymbol("ETHBTC",  "ETH",  "BTC");
    store.registerSymbol("BTCUSDT", "BTC",  "USDT");

    // Deep books — 1000 units at every level so slippage is negligible
    store.updateBook("ETHUSDT",
        {{100.0, 1000.0}, {100.1, 1000.0}},
        {{99.0,  1000.0}, {98.9,  1000.0}});

    store.updateBook("ETHBTC",
        {{0.075,  10000.0}, {0.0751, 10000.0}},
        {{0.074,  10000.0}, {0.0739, 10000.0}});

    store.updateBook("BTCUSDT",
        {{1350.0, 1000.0}, {1351.0, 1000.0}},
        {{1400.0, 1000.0}, {1399.0, 1000.0}});
}

// Route for a USDT→ETH→BTC→USDT triangle
std::vector<std::pair<std::string,std::string>> triangleRoute() {
    return {{"USDT","ETH"}, {"ETH","BTC"}, {"BTC","USDT"}};
}

// ── OrderBookStore tests ──────────────────────────────────────────────────────

TEST(OrderBookTest, RegisterAndRetrieveBook) {
    OrderBookStore store;
    store.registerSymbol("ETHUSDT", "ETH", "USDT");
    store.updateBook("ETHUSDT", {{100.0, 1.0}}, {{99.0, 1.0}});

    OrderBook book;
    EXPECT_TRUE(store.getBook("ETHUSDT", book));
    EXPECT_DOUBLE_EQ(book.asks[0].price, 100.0);
    EXPECT_DOUBLE_EQ(book.bids[0].price, 99.0);
}

TEST(OrderBookTest, UnknownSymbolReturnsFalse) {
    OrderBookStore store;
    OrderBook book;
    EXPECT_FALSE(store.getBook("UNKNOWN", book));
}

TEST(OrderBookTest, EdgeLookupAskSide) {
    OrderBookStore store;
    store.registerSymbol("ETHUSDT", "ETH", "USDT");

    EdgeBookInfo info;
    EXPECT_TRUE(store.findEdgeInfo("USDT", "ETH", info));
    EXPECT_EQ(info.symbol, "ETHUSDT");
    EXPECT_TRUE(info.useAsks);
}

TEST(OrderBookTest, EdgeLookupBidSide) {
    OrderBookStore store;
    store.registerSymbol("ETHUSDT", "ETH", "USDT");

    EdgeBookInfo info;
    EXPECT_TRUE(store.findEdgeInfo("ETH", "USDT", info));
    EXPECT_EQ(info.symbol, "ETHUSDT");
    EXPECT_FALSE(info.useAsks);
}

// ── SimulationEngine tests ────────────────────────────────────────────────────

TEST(SimulationTest, DeepLiquidityProducesResultForEachNotional) {
    OrderBookStore store;
    populateDeepStore(store);
    SimulationEngine sim(store, 0.001);

    testing::internal::CaptureStdout();
    auto results = sim.simulate(triangleRoute(), {100.0, 500.0, 1000.0}, 0.0);
    testing::internal::GetCapturedStdout();

    EXPECT_EQ(results.size(), 3u);
}

TEST(SimulationTest, ZeroFeeMaximisesReturn) {
    OrderBookStore store;
    populateDeepStore(store);
    SimulationEngine simNoFee(store, 0.0);
    SimulationEngine simWithFee(store, 0.01);

    testing::internal::CaptureStdout();
    auto r0   = simNoFee.simulate(triangleRoute(),   {100.0}, -999.0);
    auto rFee = simWithFee.simulate(triangleRoute(), {100.0}, -999.0);
    testing::internal::GetCapturedStdout();

    ASSERT_FALSE(r0.empty());
    ASSERT_FALSE(rFee.empty());
    EXPECT_GT(r0[0].netProfitPct, rFee[0].netProfitPct)
        << "Zero-fee run should always outperform a fee run";
}

TEST(SimulationTest, InsufficientLiquidityFlaggedCorrectly) {
    OrderBookStore store;
    store.registerSymbol("ETHUSDT", "ETH", "USDT");
    store.registerSymbol("ETHBTC",  "ETH", "BTC");
    store.registerSymbol("BTCUSDT", "BTC", "USDT");

    // Very shallow: only $5 worth of ETH available
    store.updateBook("ETHUSDT", {{100.0, 0.05}},    {{99.0,   0.05}});
    store.updateBook("ETHBTC",  {{0.075, 1000.0}},  {{0.074,  1000.0}});
    store.updateBook("BTCUSDT", {{1400.0, 1000.0}}, {{1400.0, 1000.0}});

    SimulationEngine sim(store, 0.001);

    testing::internal::CaptureStdout();
    auto results = sim.simulate(triangleRoute(), {1000.0}, 0.0);
    testing::internal::GetCapturedStdout();

    ASSERT_FALSE(results.empty());
    EXPECT_TRUE(results[0].insufficientLiquidity)
        << "Should detect that $1000 cannot be filled in a $5-deep book";
}

TEST(SimulationTest, MissingBookSkipsResult) {
    OrderBookStore emptyStore;
    emptyStore.registerSymbol("ETHUSDT", "ETH", "USDT");
    // No books added

    SimulationEngine sim(emptyStore, 0.001);

    testing::internal::CaptureStdout();
    auto results = sim.simulate(triangleRoute(), {100.0}, 0.0);
    testing::internal::GetCapturedStdout();

    EXPECT_TRUE(results.empty());
}

TEST(SimulationTest, ViableFlagRespectsMinProfitThreshold) {
    OrderBookStore store;
    populateDeepStore(store);
    SimulationEngine sim(store, 0.0);

    testing::internal::CaptureStdout();
    auto results = sim.simulate(triangleRoute(), {100.0}, 999.0);
    testing::internal::GetCapturedStdout();

    ASSERT_FALSE(results.empty());
    EXPECT_FALSE(results[0].viable)
        << "No trade can be viable against a 999% profit threshold";
}
// ── Quote sanity ─────────────────────────────────────────────────────────────

TEST(QuoteSanityTest, AcceptsNormalSpread) {
    EXPECT_TRUE(isSaneTopOfBook(3199.9, 3200.1));
}

TEST(QuoteSanityTest, RejectsNonPositivePrices) {
    EXPECT_FALSE(isSaneTopOfBook(0.0, 3200.0));
    EXPECT_FALSE(isSaneTopOfBook(3199.0, -1.0));
}

TEST(QuoteSanityTest, RejectsCrossedOrLockedBook) {
    EXPECT_FALSE(isSaneTopOfBook(3200.5, 3200.0)); // crossed
    EXPECT_FALSE(isSaneTopOfBook(3200.0, 3200.0)); // locked
}
