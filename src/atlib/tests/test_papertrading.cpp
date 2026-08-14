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

// The same shape, with one holding already at its target weight so the 1%
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

} // namespace
