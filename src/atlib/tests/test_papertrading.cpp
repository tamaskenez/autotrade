#include <atlib/marketdata/MarketData.h>
#include <atlib/papertrading/papertrading.h>

#include <gtest/gtest.h>

// What market_orders_from_portfolio_change() promises, and what these tests
// check, is a round trip: fill the orders it returns at the same prices it priced
// them with, using the same cost estimators it used, and the portfolio should
// come out fully invested -- no cash left over and, above all, no cash owed.
//
// Filling them here rather than comparing order quantities against hand-computed
// numbers is deliberate. The quantities are the answer to a fixed-point problem
// whose exact solution nobody wants to write out per test, while "did it spend
// exactly the money it had" is one number, is the property the function's own
// comment states as its goal, and does not have to be recomputed when the
// rebalancing threshold or the iteration count changes.

namespace
{

double trade_value(const MarketOrder& order, double price)
{
    return abs(order.unit == MarketOrder::Unit::shares ? order.quantity * price : order.quantity);
}

// For failure messages, where the orders themselves are the evidence.
string describe(span<const MarketOrder> orders)
{
    string out = "orders:";
    for (const auto& o : orders) {
        out += format("\n  {} {} {}", o.quantity < 0 ? "SELL" : "BUY", o.symbol, abs(o.quantity));
        out += o.unit == MarketOrder::Unit::shares ? " shares" : " of trade value";
    }
    return out;
}

class PortfolioChangeTest : public ::testing::Test
{
protected:
    expected<vector<MarketOrder>, string>
    orders(BrokerCostScheme scheme, const Portfolio& before, const vector<string>& desired) const
    {
        return market_orders_from_portfolio_change(scheme, prices, before, desired);
    }

    // Fills `os` at the prices the orders were priced with, paying what the
    // estimators say each fill costs. Anything left over is therefore the
    // function's own inconsistency and not a second opinion about trading costs.
    Portfolio fill(const Portfolio& before, span<const MarketOrder> os, BrokerCostScheme scheme) const
    {
        Portfolio after = before;
        for (const auto& o : os) {
            const double price = prices.at(o.symbol);
            const double shares = o.unit == MarketOrder::Unit::shares ? o.quantity : o.quantity / price;
            if (shares < 0) {
                after.cash += estimate_net_income_from_selling_equity_by_shares(scheme, -shares, price);
            } else {
                after.cash -= estimate_total_cash_needed_when_buying_equity_for_shares(scheme, shares, price);
            }
            after.equities[o.symbol] += shares;
        }
        std::erase_if(after.equities, [](const auto& kv) {
            return abs(kv.second) < 1e-9;
        });
        return after;
    }

    // Every price a test names: what the call is given, and what the fills above
    // settle at.
    std::flat_map<string, double> prices;
};

// ------------------------------------------------------- the cases that work

// The simple direction, and the control for everything below: with nothing held,
// the whole cash balance goes into the basket and the costs come out of it.
TEST_F(PortfolioChangeTest, CashIsFullyInvested)
{
    prices = {
      {"A", 100.0},
      {"B", 40.0 }
    };

    const auto before = Portfolio{.cash = 100'000.0};
    const auto os = orders(BrokerCostScheme::flat_10bp, before, {"A", "B"});
    ASSERT_TRUE(os.has_value()) << os.error();

    const auto after = fill(before, *os, BrokerCostScheme::flat_10bp);
    EXPECT_NEAR(after.cash, 0.0, 2 * k_min_cash_amount_to_trade);
    EXPECT_NEAR(after.equities.at("A") * 100.0, after.equities.at("B") * 40.0, 2 * k_min_cash_amount_to_trade);
}

// Every holding sold, an entirely new basket bought. No asset is on both sides,
// so the scaling loop has nothing to solve: the proceeds are known before the
// buys are priced.
TEST_F(PortfolioChangeTest, WholesaleSwitchSpendsExactlyTheProceeds)
{
    prices = {
      {"OLD", 100.0},
      {"A",   100.0},
      {"B",   25.0 }
    };

    const auto before = Portfolio{.cash = 0.0, .equities = {{"OLD", 1'000.0}}};
    const auto os = orders(BrokerCostScheme::flat_10bp, before, {"A", "B"});
    ASSERT_TRUE(os.has_value()) << os.error();

    const auto after = fill(before, *os, BrokerCostScheme::flat_10bp);
    EXPECT_FALSE(after.equities.contains("OLD"));
    EXPECT_NEAR(after.cash, 0.0, 2 * k_min_cash_amount_to_trade);
    EXPECT_NEAR(after.equities.at("A") * 100.0, after.equities.at("B") * 25.0, 2 * k_min_cash_amount_to_trade);
}

// A holding sold outright while the rest of the basket stays. The proceeds are
// known exactly before any buy is priced and the two remaining legs are
// symmetric, so the first guess is already the answer and the scaling loop has
// nothing to correct. That is what makes it a control for the tests below, which
// differ from it only in that the first guess is *not* the answer.
TEST_F(PortfolioChangeTest, ProceedsOfADroppedHoldingAreReinvested)
{
    prices = {
      {"A",   100.0},
      {"B",   100.0},
      {"OLD", 100.0}
    };

    const auto before = Portfolio{
      .cash = 0.0, .equities = {{"A", 400.0}, {"B", 400.0}, {"OLD", 400.0}}
    };
    const auto os = orders(BrokerCostScheme::flat_20bp, before, {"A", "B"});
    ASSERT_TRUE(os.has_value()) << os.error();

    const auto after = fill(before, *os, BrokerCostScheme::flat_20bp);
    EXPECT_FALSE(after.equities.contains("OLD"));
    EXPECT_NEAR(after.cash, 0.0, 3 * k_min_cash_amount_to_trade) << describe(*os);
}

// ------------------------------------------------- cash without a basket change

// Cash that arrives without the basket changing -- a dividend, most of the time.
// Nothing is bought or sold outright here, so this is the one call whose whole
// job is the leftover cash, and the round trip is the same one as everywhere
// above: it has to be spent, and not more of it than there is.
//
// What it does *not* assert is that the basket comes out equal. Each leg needs
// 4.99 shares, the rebalancing threshold declines a trade smaller than 0.5% of a
// 1000-share position, and so the first pass rejects all three; the retry drops
// one and hands its share to the other two. The cash lands, unevenly, in two legs
// out of three. That is the threshold working as intended on a portfolio too
// close to its target to be worth three orders, and pinning the split would pin
// the threshold's value rather than this function's contract.
TEST_F(PortfolioChangeTest, CashIsInvestedWhenTheBasketIsUnchanged)
{
    constexpr auto broker_scheme = BrokerCostScheme::flat_20bp;
    prices = {
      {"A", 100.0},
      {"B", 100.0},
      {"C", 100.0}
    };

    const auto before = Portfolio{
      .cash = 1'500.0, .equities = {{"A", 1'000.0}, {"B", 1'000.0}, {"C", 1'000.0}}
    };
    const auto os = orders(broker_scheme, before, {"A", "B", "C"});
    ASSERT_TRUE(os.has_value()) << os.error();
    ASSERT_FALSE(os->empty()) << "the cash is worth investing and nothing else has to change for it to be";

    const auto after = fill(before, *os, broker_scheme);
    EXPECT_GE(after.cash, -k_min_cash_amount_to_trade) << "the orders spend money the portfolio does not have\n"
                                                       << describe(*os);
    EXPECT_NEAR(after.cash, 0.0, 2 * k_min_cash_amount_to_trade) << describe(*os);
}

// The other side of that, and the reason the case above cannot simply always
// trade: a balance too small to place an order for is left where it is rather
// than turned into orders that cost more than they move.
//
// Both amounts below reach that answer by a different road. The first is under
// k_min_cash_amount_to_trade, so the call returns before pricing anything. The
// second is over it -- the basket is unchanged, so the early return does not
// apply and the whole scaling solve runs -- and comes back empty anyway, because
// $5 spread over three $100,000 legs is far below the rebalancing threshold and
// every order it proposes is rejected in turn.
TEST_F(PortfolioChangeTest, CashTooSmallToInvestIsLeftAlone)
{
    prices = {
      {"A", 100.0},
      {"B", 100.0},
      {"C", 100.0}
    };

    for (const double cash : {0.5, 5.0}) {
        const auto before = Portfolio{
          .cash = cash, .equities = {{"A", 1'000.0}, {"B", 1'000.0}, {"C", 1'000.0}}
        };
        const auto os = orders(BrokerCostScheme::flat_20bp, before, {"A", "B", "C"});
        ASSERT_TRUE(os.has_value()) << os.error();
        EXPECT_TRUE(os->empty()) << "$" << cash << " is not worth an order\n" << describe(*os);
    }
}

// -------------------------------------------- issue 1: the scaling fixed point

// One asset is added to a basket that is otherwise kept, so both legs have to be
// priced against each other: what the sells bring in depends on how much the buy
// needs, which is what the scaling loop is for.
//
// The correction it computes on the first pass is exactly right (1 - 66.67/100000
// here) and is then thrown away, because the loop breaks on
// equal_epsilon(scaling, new_scaling) *before* assigning it. So the orders are
// the unscaled first guess: they buy a third of the portfolio and sell a third of
// it, and nothing anywhere pays the two commissions.
TEST_F(PortfolioChangeTest, PartialRebalanceFundsTheTradingCost)
{
    constexpr auto broker_scheme = BrokerCostScheme::flat_100bp;
    prices = {
      {"A", 100.0},
      {"B", 100.0},
      {"C", 100.0}
    };

    const auto before = Portfolio{
      .cash = 0.0, .equities = {{"A", 500.0}, {"B", 500.0}}
    };
    const auto os = orders(broker_scheme, before, {"A", "B", "C"});
    ASSERT_TRUE(os.has_value()) << os.error();

    const auto after = fill(before, *os, broker_scheme);
    EXPECT_GE(after.cash, -k_min_cash_amount_to_trade) << "the orders spend money the portfolio does not have\n"
                                                       << describe(*os);
    EXPECT_NEAR(after.cash, 0.0, 3 * k_min_cash_amount_to_trade);
}

// The same shape, with one holding already at its target weight so the
// rebalancing threshold skips it.
//
// That skip is what separates this from the case above: the scaling loop divides
// the leftover cash by the value of *every* desired asset, including the ones it
// has just decided not to trade, so a step that should close the gap closes half
// of it. The iterations that follow halve it again, and the loop then stops
// because two successive scalings differ by less than the epsilon -- which they
// do precisely because the correction is converging on the wrong number.
TEST_F(PortfolioChangeTest, RebalanceAroundAnUntradedHoldingFundsTheTradingCost)
{
    prices = {
      {"A", 100.0},
      {"B", 100.0},
      {"C", 100.0}
    };

    // A is already worth a third of the final basket; B is worth two thirds.
    const auto before = Portfolio{
      .cash = 0.0, .equities = {{"A", 1'000.0}, {"B", 2'000.0}}
    };
    const auto os = orders(BrokerCostScheme::flat_20bp, before, {"A", "B", "C"});
    ASSERT_TRUE(os.has_value()) << os.error();

    const auto after = fill(before, *os, BrokerCostScheme::flat_20bp);
    EXPECT_GE(after.cash, -k_min_cash_amount_to_trade) << "the orders spend money the portfolio does not have\n"
                                                       << describe(*os);
    EXPECT_NEAR(after.cash, 0.0, 3 * k_min_cash_amount_to_trade);
}

// The rebalancing threshold is what makes the scaling loop's problem
// discontinuous, and small portfolios sit right on the discontinuity: at scaling
// 1 both legs of the basket fall below the minimum trade size and nothing is
// bought, which leaves the whole balance unspent, which asks for a scaling of
// about 2, at which both legs clear the minimum and cost twice what there is.
//
// The loop walks between those two states without either being a fixed point,
// and returns whichever one the last iteration happened to leave behind. Here it
// is the second: orders to spend $3.00 out of $1.50.
TEST_F(PortfolioChangeTest, TinyBasketDoesNotOrderMoreCashThanThereIs)
{
    prices = {
      {"OLD", 100.0},
      {"A",   100.0},
      {"B",   100.0}
    };

    const auto before = Portfolio{.cash = 0.0, .equities = {{"OLD", 0.015}}};
    const auto os = orders(BrokerCostScheme::flat_10bp, before, {"A", "B"});
    ASSERT_TRUE(os.has_value()) << os.error();

    const auto after = fill(before, *os, BrokerCostScheme::flat_10bp);
    EXPECT_GE(after.cash, -k_min_cash_amount_to_trade) << "the orders spend money the portfolio does not have\n"
                                                       << describe(*os);
}

// ------------------------------------------------ issue 4: the outright sells

// The sells of dropped holdings are emitted before any threshold is applied, so a
// position worth ten cents becomes an order -- one a broker charges a minimum
// commission for, and one the rest of the function has already decided is too
// small to be worth trading.
TEST_F(PortfolioChangeTest, DustHoldingIsNotWorthSelling)
{
    prices = {
      {"DUST", 100.0},
      {"A",    100.0}
    };

    const auto before = Portfolio{.cash = 1'000.0, .equities = {{"DUST", 0.001}}};
    const auto os = orders(BrokerCostScheme::flat_10bp, before, {"A"});
    ASSERT_TRUE(os.has_value()) << os.error();

    for (const auto& o : *os) {
        EXPECT_GE(trade_value(o, prices.at(o.symbol)), k_min_cash_amount_to_trade)
          << o.symbol << " is ordered below the minimum trade size";
    }
}

// The same path with nothing left in the position at all: an entry that has been
// sold down to zero but not erased still produces an order to sell zero shares.
TEST_F(PortfolioChangeTest, EmptyHoldingProducesNoOrder)
{
    prices = {
      {"GONE", 100.0},
      {"A",    100.0}
    };

    const auto before = Portfolio{.cash = 1'000.0, .equities = {{"GONE", 0.0}}};
    const auto os = orders(BrokerCostScheme::flat_10bp, before, {"A"});
    ASSERT_TRUE(os.has_value()) << os.error();

    for (const auto& o : *os) {
        EXPECT_NE(o.symbol, "GONE") << "an empty position is not something to sell";
        EXPECT_NE(o.quantity, 0.0);
    }
}

// The other direction of the same question, and the one that costs money to get
// wrong: an entry sold down to zero but not erased is a symbol the portfolio does
// not hold, so a basket naming it again has to buy it back. Nothing in the
// arithmetic below distinguishes this from a symbol never held, which is the
// point -- it is the same call as CashIsFullyInvested with one dead entry left
// lying in the map.
//
// Reading that entry as a holding put B in assets_to_keep, which left nothing to
// buy and nothing to sell, which is the one shape the function answers with no
// orders at all. The basket then silently stayed in A alone for as long as the
// entry lingered.
TEST_F(PortfolioChangeTest, ClosedPositionIsBoughtBackWhenTheBasketWantsItAgain)
{
    prices = {
      {"A", 100.0},
      {"B", 40.0 }
    };

    const auto before = Portfolio{
      .cash = 0.0, .equities = {{"A", 1'000.0}, {"B", 0.0}}
    };
    const auto os = orders(BrokerCostScheme::flat_10bp, before, {"A", "B"});
    ASSERT_TRUE(os.has_value()) << os.error();
    ASSERT_FALSE(os->empty()) << "B is wanted and not held, so it has to be bought";

    const auto after = fill(before, *os, BrokerCostScheme::flat_10bp);
    EXPECT_NEAR(after.cash, 0.0, 2 * k_min_cash_amount_to_trade) << describe(*os);
    EXPECT_NEAR(after.equities.at("A") * 100.0, after.equities.at("B") * 40.0, 2 * k_min_cash_amount_to_trade)
      << describe(*os);
}

// A short position reaches estimate_net_income_from_selling_equity_by_shares()
// with a negative share count and trips its CHECK.
//
// Pinned as a death test because that is what happens today, not because it is
// what should: the function returns expected<>, so a portfolio it cannot act on
// is an error it can report. When it does, this becomes an assertion that the
// call fails rather than that the process dies.
using PortfolioChangeDeathTest = PortfolioChangeTest;

TEST_F(PortfolioChangeDeathTest, ShortPositionTerminatesInsteadOfReportingAnError)
{
    prices = {
      {"SHORT", 100.0},
      {"A",     100.0}
    };

    const auto before = Portfolio{.cash = 1'000.0, .equities = {{"SHORT", -10.0}}};
    EXPECT_DEATH((void)orders(BrokerCostScheme::flat_10bp, before, {"A"}), "");
}

// ------------------------------------------------- issue 1: the scaling solve
//
// These two fail today. compute_scaling() fits a single straight line through
// cash_after_transaction_for_scaling() sampled at 0.995 and 1.005, and takes that
// line's root.
//
// The sampled function is only *piecewise* linear. Every asset contributes a
// corner at the scaling where its own trade changes direction, because a sell is
// credited (1 - cost) per unit of trade value while a buy is charged (1 + cost).
// An asset's corner sits at scaling = current_stock / the target the initial
// guess handed it, so a holding that starts within half a percent of that target
// puts its corner *between* the two sample points. The line is then fitted across
// the corner rather than along either side of it, and its root is not the root of
// the function it is standing in for.
//
// Nothing downstream notices. The loop re-solves only when an order was rejected,
// and always samples the same window around 1.0, so a bad root is never revisited;
// and cash_after_transaction_for_scaling(scaling) is never evaluated at the answer
// to see whether it balances. Evaluating that residual is both the cheapest way to
// detect this and, iterated, the fix: the function is piecewise linear, so
// re-solving around each new root reaches the exact answer in a few steps.

// Four equal legs of a $1,000,000 portfolio are 2500 shares at $100. B is held
// 0.05% above that, which puts its corner at 2501.25 / 2500 = 1.0005 -- inside the
// window. The orders that come back sell A and B and buy C and D in equal parts,
// and spend about $11 more than the portfolio holds.
TEST_F(PortfolioChangeTest, HoldingInsideTheSamplingWindowDoesNotOverdraw)
{
    constexpr auto broker_scheme = BrokerCostScheme::flat_100bp;
    prices = {
      {"A", 100.0},
      {"B", 100.0},
      {"C", 100.0},
      {"D", 100.0}
    };

    const auto before = Portfolio{
      .cash = 0.0, .equities = {{"A", 7'498.75}, {"B", 2'501.25}}
    };
    const auto os = orders(broker_scheme, before, {"A", "B", "C", "D"});
    ASSERT_TRUE(os.has_value()) << os.error();

    const auto after = fill(before, *os, broker_scheme);
    EXPECT_GE(after.cash, -k_min_cash_amount_to_trade) << "the orders spend money the portfolio does not have\n"
                                                       << describe(*os);
    EXPECT_NEAR(after.cash, 0.0, 3 * k_min_cash_amount_to_trade);
}

// The same defect over the whole window rather than at one chosen holding, which
// is what tells it apart from accumulated rounding: the shortfall is largest just
// above the target and falls linearly to nothing at 2512.50 -- exactly the 1.005
// sample point, where the corner leaves the window again. It is a fixed fraction
// of the portfolio, about 1.2e-5, so the same sweep over a $100,000,000 portfolio
// comes up roughly $1,234 short.
TEST_F(PortfolioChangeTest, ScalingSolveIsExactAcrossTheSamplingWindow)
{
    constexpr auto broker_scheme = BrokerCostScheme::flat_100bp;
    prices = {
      {"A", 100.0},
      {"B", 100.0},
      {"C", 100.0},
      {"D", 100.0}
    };

    double worst_cash = 0.0;
    double worst_b_shares = 0.0;
    for (int i = 0; i <= 20; ++i) {
        const double b_shares = 2'500.0 * (1.0 + i * 0.00025); // across the +0.5% half-window
        const auto before = Portfolio{
          .cash = 0.0, .equities = {{"A", 10'000.0 - b_shares}, {"B", b_shares}}
        };
        const auto os = orders(broker_scheme, before, {"A", "B", "C", "D"});
        ASSERT_TRUE(os.has_value()) << os.error();

        const auto after = fill(before, *os, broker_scheme);
        if (after.cash < worst_cash) {
            worst_cash = after.cash;
            worst_b_shares = b_shares;
        }
    }
    EXPECT_GE(worst_cash, -k_min_cash_amount_to_trade)
      << "worst at B = " << worst_b_shares << " shares, where the orders spend " << -worst_cash
      << " more than the portfolio has";
}

// ------------------------------------------------------- apply_market_orders()

// A holding absent from one portfolio and zero in the other is the same holding.
// fill() drops a position it has closed and apply_market_orders() leaves it at
// zero, and which of the two is the nicer representation is not what any test
// below is about.
double shares_of(const Portfolio& portfolio, const string& symbol)
{
    const auto it = portfolio.equities.find(symbol);
    return it == portfolio.equities.end() ? 0.0 : it->second;
}

// fill() above and apply_market_orders() are the same arithmetic written twice,
// and that is deliberate rather than an oversight to be tidied away: fill() is
// what every test in this file states its invariant in terms of, so if it were a
// call to the function under test instead, a mistake shared by the two would
// cancel and nothing here would report it.
//
// This is the one place they are held against each other, which is what lets the
// rest stay independent -- and what lets everything asserted above about fill()
// be read as a statement about the fill that actually runs.
TEST_F(PortfolioChangeTest, ApplyingGeneratedOrdersMatchesTheReferenceFill)
{
    constexpr auto broker_scheme = BrokerCostScheme::flat_20bp;
    prices = {
      {"OLD", 100.0},
      {"A",   100.0},
      {"B",   25.0 },
      {"C",   7.0  }
    };

    const auto before = Portfolio{
      .cash = 5'000.0, .equities = {{"OLD", 1'000.0}, {"A", 300.0}}
    };
    const auto os = orders(broker_scheme, before, {"A", "B", "C"});
    ASSERT_TRUE(os.has_value()) << os.error();

    const auto applied = apply_market_orders(broker_scheme, prices, before, *os);
    ASSERT_TRUE(applied.has_value()) << applied.error();

    const auto reference = fill(before, *os, broker_scheme);
    EXPECT_NEAR(applied->portfolio.cash, reference.cash, 1e-9) << describe(*os);
    for (const auto& symbol : prices.keys()) {
        EXPECT_NEAR(shares_of(applied->portfolio, symbol), shares_of(reference, symbol), 1e-9) << symbol;
    }

    // And so the round trip asserted of fill() above holds of the real fill too.
    EXPECT_NEAR(applied->portfolio.cash, 0.0, 2 * k_min_cash_amount_to_trade) << describe(*os);
    EXPECT_EQ(shares_of(applied->portfolio, "OLD"), 0.0);
}

class ApplyOrdersTest : public PortfolioChangeTest
{
protected:
    expected<ApplyMarketOrdersResult, string>
    apply(BrokerCostScheme scheme, const Portfolio& before, const vector<MarketOrder>& os) const
    {
        return apply_market_orders(scheme, prices, before, os);
    }
};

// The same minimum the rebalancer applies to the sells it declines to emit, so a
// round trip through the two agrees about which orders are too small to place.
// Skipped, not filled and not refused: an order this size is a no-op, not a bug.
TEST_F(ApplyOrdersTest, UndersizedOrderIsSkipped)
{
    prices = {
      {"A", 100.0}
    };

    const auto before = Portfolio{.cash = 1'000.0, .equities = {{"A", 10.0}}};
    const auto applied = apply(
      BrokerCostScheme::flat_20bp,
      before,
      {
        {.symbol = "A", .unit = MarketOrder::Unit::cash,   .quantity = 0.5   },
        {.symbol = "A", .unit = MarketOrder::Unit::shares, .quantity = 0.0001}
    }
    );
    ASSERT_TRUE(applied.has_value()) << applied.error();

    EXPECT_EQ(applied->portfolio.cash, before.cash);
    EXPECT_EQ(applied->portfolio.equities.at("A"), before.equities.at("A"));
}

// Selling more than is held is a short position arrived at by accident, and
// nothing downstream models one. The valid buy ahead of it in the list is what
// makes the point: a rejected order abandons the whole call rather than leaving
// the caller a half-filled portfolio.
TEST_F(ApplyOrdersTest, OversellIsRejected)
{
    prices = {
      {"A", 100.0},
      {"B", 100.0}
    };

    const auto before = Portfolio{.cash = 0.0, .equities = {{"A", 10.0}}};
    const auto applied = apply(
      BrokerCostScheme::flat_20bp,
      before,
      {
        {.symbol = "B", .unit = MarketOrder::Unit::shares, .quantity = 5.0  },
        {.symbol = "A", .unit = MarketOrder::Unit::shares, .quantity = -11.0}
    }
    );
    ASSERT_FALSE(applied.has_value());
    EXPECT_NE(applied.error().find("A"), string::npos) << applied.error();
}

TEST_F(ApplyOrdersTest, ShortOfAnUnheldSymbolIsRejected)
{
    prices = {
      {"A", 100.0}
    };

    const auto applied = apply(
      BrokerCostScheme::flat_20bp,
      Portfolio{
        .cash = 1'000.0
    },
      {{.symbol = "A", .unit = MarketOrder::Unit::shares, .quantity = -1.0}}
    );
    EXPECT_FALSE(applied.has_value());
}

// A sale of a whole position arrives as a trade value, and dividing it back by the
// price it was priced with need not land on the share count exactly. Both prices
// below are chosen because they do not, and because they miss in opposite
// directions: 2300.0 / 2.3 comes out a hair above 1000 shares and 1100.0 / 1.1 a
// hair below. A price like 3.0 would prove nothing here -- 3000.0 / 3.0 is exact.
//
// Two things have to hold. The overshoot must not read as an oversell, because the
// portfolio does have the shares and only the division says otherwise. And what is
// left must be exactly zero on both sides, rather than dust of either sign that the
// next rebalance would read as a position still open.
TEST_F(ApplyOrdersTest, WholePositionSoldAsCashLandsAtExactlyZero)
{
    for (const auto [price, trade_value] : {
           pair{2.3, 2'300.0},
           pair{1.1, 1'100.0}
    }) {
        prices = {
          {"A", price}
        };

        const auto applied = apply(
          BrokerCostScheme::flat_20bp,
          Portfolio{
            .cash = 0.0, .equities = {{"A", 1'000.0}}
        },
          {{.symbol = "A", .unit = MarketOrder::Unit::cash, .quantity = -trade_value}}
        );
        ASSERT_TRUE(applied.has_value()) << applied.error() << " at price " << price;
        EXPECT_EQ(applied->portfolio.equities.at("A"), 0.0) << "at price " << price;
    }
}

// The other half of the pair above: an overdraft is allowed through, because a
// backtest wants to see the balance it ended up with rather than an error where a
// number should be.
TEST_F(ApplyOrdersTest, CashMayGoNegative)
{
    prices = {
      {"A", 100.0}
    };

    const auto applied = apply(
      BrokerCostScheme::flat_20bp,
      Portfolio{
        .cash = 100.0
    },
      {{.symbol = "A", .unit = MarketOrder::Unit::shares, .quantity = 50.0}}
    );
    ASSERT_TRUE(applied.has_value()) << applied.error();
    EXPECT_LT(applied->portfolio.cash, 0.0);
    EXPECT_EQ(applied->portfolio.equities.at("A"), 50.0);
}

// A price map that cannot answer for an order is reported rather than asserted on.
// A zero or negative price matters as much as a missing one: it reaches the
// estimators, whose CHECKs would take the process down over what is really a bad
// argument.
TEST_F(ApplyOrdersTest, UnusablePriceIsReported)
{
    const vector<MarketOrder> os{
      {.symbol = "A", .unit = MarketOrder::Unit::cash, .quantity = 1'000.0}
    };

    for (const auto& unusable :
         {std::flat_map<string, double>{},
          std::flat_map<string, double>{{"A", 0.0}},
          std::flat_map<string, double>{{"A", -5.0}}}) {
        prices = unusable;
        EXPECT_FALSE(apply(BrokerCostScheme::flat_20bp, Portfolio{.cash = 10'000.0}, os).has_value());
    }
}

TEST_F(ApplyOrdersTest, NonFiniteQuantityIsReported)
{
    prices = {
      {"A", 10.0}
    };

    const auto applied = apply(
      BrokerCostScheme::flat_20bp,
      Portfolio{
        .cash = 10'000.0
    },
      {{.symbol = "A", .unit = MarketOrder::Unit::cash, .quantity = std::numeric_limits<double>::quiet_NaN()}}
    );
    EXPECT_FALSE(applied.has_value());
}

// Orders are applied in the order given rather than netted first, so several on
// one symbol compound -- and each leg pays its own commission, which is the whole
// reason the distinction is worth pinning.
TEST_F(ApplyOrdersTest, OrdersOnOneSymbolCompound)
{
    constexpr auto broker_scheme = BrokerCostScheme::flat_20bp;
    prices = {
      {"A", 10.0}
    };

    const auto before = Portfolio{.cash = 10'000.0};
    const auto applied = apply(
      broker_scheme,
      before,
      {
        {.symbol = "A", .unit = MarketOrder::Unit::shares, .quantity = 100.0},
        {.symbol = "A", .unit = MarketOrder::Unit::shares, .quantity = 50.0 },
        {.symbol = "A", .unit = MarketOrder::Unit::shares, .quantity = -30.0}
    }
    );
    ASSERT_TRUE(applied.has_value()) << applied.error();

    EXPECT_NEAR(applied->portfolio.equities.at("A"), 120.0, 1e-12);
    const double paid = estimate_total_cash_needed_when_buying_equity_for_shares(broker_scheme, 150.0, 10.0);
    const double received = estimate_net_income_from_selling_equity_by_shares(broker_scheme, 30.0, 10.0);
    EXPECT_NEAR(applied->portfolio.cash, before.cash - paid + received, 1e-9);
}

// ------------------------------------------- apply_market_orders() over MarketData
//
// Hermetic for the reasons test_MarketData.cpp gives at length: the test writes
// its own payload into a temporary workspace, because a miss against the real
// cache would not fail but *download*, and the suite would then depend on a
// credential and the network.
//
// The two bars are 01-02 and 01-04, so 01-03 is a gap inside the covered range.
// That matters: a date past the last bar would send MarketData to the network
// looking for one, while a hole in the middle is answered from what is already
// there.
class ApplyOverMarketDataTest : public ::testing::Test
{
protected:
    void SetUp() override
    {
        const auto* const test = ::testing::UnitTest::GetInstance()->current_test_info();
        config.workspace_dir =
          fs::temp_directory_path() / format("atlib_{}_{}_{}", test->test_suite_name(), test->name(), getpid());

        std::error_code ec;
        fs::remove_all(config.workspace_dir, ec);

        // Open and close deliberately far apart, so which leg was read is visible.
        ASSERT_TRUE(write_cache(
                      cache_path(config, Provider::tiingo, k_symbol),
                      R"([{"date":"2024-01-02T00:00:00.000Z","open":20.0,"high":60.0,"low":10.0,)"
                      R"("close":50.0,"volume":1000.0,"divCash":0.0,"splitFactor":1.0},)"
                      R"({"date":"2024-01-04T00:00:00.000Z","open":21.0,"high":61.0,"low":11.0,)"
                      R"("close":51.0,"volume":1000.0,"divCash":0.0,"splitFactor":1.0}])"
        )
                      .has_value());
    }

    void TearDown() override
    {
        std::error_code ec;
        fs::remove_all(config.workspace_dir, ec);
    }

    static chr::local_days day(int y, unsigned m, unsigned d)
    {
        return chr::local_days(chr::year_month_day(chr::year(y), chr::month(m), chr::day(d)));
    }

    static constexpr string_view k_symbol = "X";

    MarketDataConfig config;
};

TEST_F(ApplyOverMarketDataTest, PicksTheRequestedLegOfTheBar)
{
    constexpr auto broker_scheme = BrokerCostScheme::flat_20bp;
    const auto before = Portfolio{.cash = 10'000.0};
    const vector<MarketOrder> os{
      {.symbol = string{k_symbol}, .unit = MarketOrder::Unit::shares, .quantity = 100.0}
    };

    for (const auto [price_type, expected_price] : {
           pair{MarketOrderPriceType::open,  20.0},
           pair{MarketOrderPriceType::close, 50.0}
    }) {
        auto market_data = MarketData(config, Provider::tiingo);
        const auto applied = apply_market_orders(broker_scheme, market_data, day(2024, 1, 2), price_type, before, os);
        ASSERT_TRUE(applied.has_value()) << applied.error();

        // The price it reports having filled at, and the money it actually moved,
        // have to be the same story.
        EXPECT_EQ(applied->asset_prices.at(string{k_symbol}), expected_price);
        const double paid =
          estimate_total_cash_needed_when_buying_equity_for_shares(broker_scheme, 100.0, expected_price);
        EXPECT_NEAR(applied->portfolio.cash, before.cash - paid, 1e-9);
    }
}

// A day the symbol did not trade is MarketData's error to report, and this only
// has to not swallow it.
TEST_F(ApplyOverMarketDataTest, MissingBarIsReported)
{
    auto market_data = MarketData(config, Provider::tiingo);
    const auto applied = apply_market_orders(
      BrokerCostScheme::flat_20bp,
      market_data,
      day(2024, 1, 3),
      MarketOrderPriceType::close,
      Portfolio{
        .cash = 10'000.0
    },
      {{.symbol = string{k_symbol}, .unit = MarketOrder::Unit::shares, .quantity = 100.0}}
    );
    EXPECT_FALSE(applied.has_value());
}

} // namespace
