#include "DualMomFixedEtfAlgorithm.h"

#include <magic_enum/magic_enum.hpp>

#include "atlib/marketdata/MarketData.h"
#include "meadow/math.h"

namespace dual_mom_fixed_etf_algorithm
{
namespace
{
expected<DailyBarAvailability, string>
do_all_assets_have_daily_bars(MarketData& market_data, const vector<string>& all_assets, chr::local_days day)
{
    CHECK(!all_assets.empty());
    TRY_ASSIGN_OR_RETURN_UNEXPECTED_ERROR(bar_avail, market_data.daily_bar_availability(all_assets.front(), day));
    for (unsigned i = 1; i < all_assets.size(); ++i) {
        const auto& s = all_assets[i];
        TRY_ASSIGN_OR_RETURN_UNEXPECTED_ERROR(b, market_data.daily_bar_availability(s, day));
        if (b != bar_avail) {
            if (b != DailyBarAvailability::after_last_bar && bar_avail != DailyBarAvailability::after_last_bar
                && (b == DailyBarAvailability::before_first_bar || bar_avail == DailyBarAvailability::before_first_bar)) {
                bar_avail = DailyBarAvailability::before_first_bar;
            } else {
                return unexpected(format(
                  "Mixed data availability for day ({}): assets before {}: {} vs. {}: {}",
                  day,
                  s,
                  magic_enum::enum_name(bar_avail),
                  s,
                  magic_enum::enum_name(b)
                ));
            }
        }
    }
    return bar_avail;
}

expected<variant<chr::local_days, NotARebalanceDay>, string> get_past_day_if_first_trading_day_of_period(
  MarketData& market_data,
  const vector<string>& all_assets,
  bool has_daily_bar,
  chr::local_days past_day,
  string_view rebalance_rule,
  chr::local_days first_day_of_segment
)
{
    if (!has_daily_bar) {
        return NotARebalanceDay{format("{} is not a trading day.", past_day)};
    }
    for (chr::local_days day = first_day_of_segment; day < past_day; ++day) {
        TRY_ASSIGN_OR_RETURN_UNEXPECTED_ERROR(day_avail, do_all_assets_have_daily_bars(market_data, all_assets, day));
        switch (day_avail) {
        case DailyBarAvailability::available:
            return NotARebalanceDay{format("{} is not a rebalance day, {} was the {}", past_day, day, rebalance_rule)};
        case DailyBarAvailability::not_traded:
            // continue the for loop.
            break;
        case DailyBarAvailability::before_first_bar:
            return unexpected(format("The queried day ({}) is before some asset's first bar.", day));
        case DailyBarAvailability::after_last_bar:
            LOG(FATAL) << format(
              "day {} was reported as being after the last bar, while the later past_day {} is a trading "
              "day, which is a contradiction.",
              day,
              past_day
            );
        }
    }
    return past_day;
}

expected<variant<chr::local_days, NotARebalanceDay>, string> get_last_trading_day_if_period_complete(
  MarketData& market_data,
  const vector<string>& all_assets,
  bool has_daily_bar,
  chr::local_days past_day,
  chr::local_days first_day_of_period,
  chr::local_days last_day_of_period,
  string_view rebalance_rule
)
{
    CHECK(first_day_of_period < last_day_of_period);
    CHECK(in_cc_range(past_day, first_day_of_period, last_day_of_period));
    // We can only figure this out on the last day of the segment which could be past the last trading day.
    if (past_day != last_day_of_period) {
        return NotARebalanceDay{
          format("not scanning for {} because past_day {} is not the last day of its period.", rebalance_rule, past_day)
        };
    }
    if (has_daily_bar) {
        return past_day;
    }

    // In this case we go back and find the last trading day of the period.
    for (auto day = past_day;;) {
        if (day == first_day_of_period) {
            return unexpected(
              format("Starting from past_day {}, couldn't find any trading day in the period.", past_day)
            );
        }
        CHECK(first_day_of_period < day);
        --day;
        TRY_CONST_ASSIGN_OR_RETURN_UNEXPECTED_ERROR(
          day_avail, do_all_assets_have_daily_bars(market_data, all_assets, day)
        );
        switch (day_avail) {
        case DailyBarAvailability::available:
            return day;
        case DailyBarAvailability::not_traded:
        case DailyBarAvailability::after_last_bar:
            // continue the for loop.
            break;
        case DailyBarAvailability::before_first_bar:
            return unexpected(format("The queried day ({}) is before some asset's first bar.", day));
        }
    }
}
} // namespace

expected<variant<chr::local_days, NotARebalanceDay>, string>
get_past_trading_day_to_rebalance_after(MarketData& market_data, const Config& config, chr::local_days past_day)
{
    if (config.equities.empty() || config.defensive_asset.empty()) {
        return unexpected("No equities or defensive asset specified");
    }
    vector<string> all_assets = config.equities;
    all_assets.push_back(config.defensive_asset);
    TRY_ASSIGN_OR_RETURN_UNEXPECTED_ERROR(bar_avail, do_all_assets_have_daily_bars(market_data, all_assets, past_day));

    const auto compute_has_daily_bar = [&]() -> expected<bool, string> {
        switch (bar_avail) {
        case DailyBarAvailability::available:
            return true;
        case DailyBarAvailability::not_traded:
        case DailyBarAvailability::after_last_bar:
            return false;
        case DailyBarAvailability::before_first_bar:
            return unexpected(format("past_day ({}) is before some asset's first bar.", past_day));
        }
    };
    TRY_CONST_ASSIGN_OR_RETURN_UNEXPECTED_ERROR(has_daily_bar, compute_has_daily_bar());

    // Check if the past_day was a rebalance day.
    switch (config.rebalance_day) {
    case RebalanceDay::first_trading_day_of_month: {
        const chr::year_month_day ymd{past_day};
        const auto first_day_of_month = ymd.year() / ymd.month() / chr::day{1};
        return get_past_day_if_first_trading_day_of_period(
          market_data,
          all_assets,
          has_daily_bar,
          past_day,
          "first trading day of month",
          chr::local_days(first_day_of_month)
        );
    }
    case RebalanceDay::last_trading_day_of_month: {
        const chr::year_month_day ymd{past_day};
        const auto first_day_of_month = ymd.year() / ymd.month() / chr::day{1};
        const auto last_day_of_month = ymd.year() / ymd.month() / chr::last;
        return get_last_trading_day_if_period_complete(
          market_data,
          all_assets,
          has_daily_bar,
          past_day,
          chr::local_days(first_day_of_month),
          chr::local_days(last_day_of_month),
          "last trading day of month"
        );
    }
    case RebalanceDay::first_trading_day_of_week: {
        // The Monday at or before `past_day`.
        //
        // Monday and not Sunday, which is the other common convention: the trading
        // week is bounded by the two days nobody trades, so a Sunday-based week cuts
        // between Saturday and Sunday and puts a weekend in the middle. Every week
        // would then begin with a day that is never a trading day, which the scan
        // would step over -- harmless but pointless -- and, worse, a `past_day`
        // falling on a Sunday would be attributed to the week that has not started yet.
        //
        // `wd - Monday` is modular and lands in [0, 6] days, so this needs no branch
        // and no ok() check: subtracting days from a date cannot leave the calendar.
        const chr::weekday wd{past_day};
        const chr::local_days first_day_of_week = past_day - (wd - chr::Monday);
        return get_past_day_if_first_trading_day_of_period(
          market_data, all_assets, has_daily_bar, past_day, "first trading day of week", first_day_of_week
        );
    }
    case RebalanceDay::last_trading_day_of_week: {
        const chr::weekday wd{past_day};
        const auto first_day_of_week = chr::local_days(past_day - (wd - chr::Monday));
        return get_last_trading_day_if_period_complete(
          market_data,
          all_assets,
          has_daily_bar,
          past_day,
          first_day_of_week,
          first_day_of_week + chr::days(6),
          "last trading day of week"
        );
    }
    }
}

expected<Response, string> rebalance(
  UNUSED MarketData& market_data,
  const Config& config,
  UNUSED chr::local_days past_trading_day,
  UNUSED const Portfolio& portfolio
)
{
    if (config.equities.empty() || config.defensive_asset.empty()) {
        return unexpected("No equities or defensive asset specified");
    }
    // All assets must have data on past_trading_day.
    vector<string> all_assets = config.equities;
    all_assets.push_back(config.defensive_asset);
    TRY_ASSIGN_OR_RETURN_UNEXPECTED_ERROR(
      bar_avail, do_all_assets_have_daily_bars(market_data, all_assets, past_trading_day)
    );
    if (bar_avail != DailyBarAvailability::available) {
        return unexpected(format("past_trading_day ({}) is not a trading day.", past_trading_day));
    }

    // Calculate the trading day in the past to calculate returns from that day to past_trading_day.
#if 0
        config.lookback_months
        // Get the past returns of the assets.

        chr::year_month_day ymd{past_trading_day};
        auto r = market_data.total_return_factor_close_to_close(
            config.defensive_asset, *rebalance_day - config.lookback_months, *rebalance_day
        );

        market_data.

#endif

    return unexpected("to be implemented");
}
} // namespace dual_mom_fixed_etf_algorithm
