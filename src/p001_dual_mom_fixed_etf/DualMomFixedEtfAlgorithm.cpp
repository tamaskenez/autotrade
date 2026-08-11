#include "DualMomFixedEtfAlgorithm.h"

#include <magic_enum/magic_enum.hpp>

#include "atlib/marketdata/MarketData.h"

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
                  "Mixed data availability for past_trading_day ({}): assets before {}: {} vs. {}: {}",
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

optional<expected<Response, string>> nullopt_if_first_trading_day_of_period(
  MarketData& market_data,
  const vector<string>& all_assets,
  bool has_daily_bar,
  chr::local_days past_trading_day,
  string_view rebalance_rule,
  chr::local_days first_day_of_segment
)
{
    if (!has_daily_bar) {
        return Response{
          .info = format(
            "Nothing to do, past_trading_day ({}) is not a rebalance day ({}) because it's not a "
            "trading day.",
            past_trading_day,
            rebalance_rule
          )
        };
    }
    for (chr::local_days day = first_day_of_segment; day <= past_trading_day; ++day) {
        TRY_ASSIGN_OR_RETURN_UNEXPECTED_ERROR(day_avail, do_all_assets_have_daily_bars(market_data, all_assets, day));
        switch (day_avail) {
        case DailyBarAvailability::available:
            if (day == past_trading_day) {
                return nullopt;
            }
            return Response{
              .info = format(
                "Nothing to do, past_trading_day ({}) is not a rebalance day ({}), {} was.",
                past_trading_day,
                rebalance_rule,
                day
              )
            };
        case DailyBarAvailability::not_traded:
            // continue the for loop.
            break;
        case DailyBarAvailability::before_first_bar:
            return unexpected(format("The queried day ({}) is before some asset's first bar.", day));
        case DailyBarAvailability::after_last_bar:
            LOG(FATAL) << format(
              "day {} was reported as being after the last bar, while the later past_trading_day {} is a trading "
              "day, which is a contradiction.",
              day,
              past_trading_day
            );
        }
    }
    LOG(FATAL) << "Unreachable.";
}

variant<chr::local_days, expected<Response, string>> last_trading_day_of_period_or_return_value(
  MarketData& market_data,
  const vector<string>& all_assets,
  bool has_daily_bar,
  chr::local_days past_trading_day,
  chr::local_days first_day_of_period,
  chr::local_days last_day_of_period,
  string_view rebalance_rule
)
{
    CHECK(first_day_of_period < last_day_of_period);
    CHECK(first_day_of_period <= past_trading_day && past_trading_day <= last_day_of_period);
    // We can only figure this out on the last day of the segment which could be past the last trading day.
    if (past_trading_day != last_day_of_period) {
        return Response{
          .info = format(
            "past_trading_day {} is not a rebalance_day ({}) because it's not the last day of its period.",
            past_trading_day,
            rebalance_rule
          )
        };
    }
    if (has_daily_bar) {
        return past_trading_day;
    }

    // In this case we go back and find the last trading day of the period.
    for (auto day = past_trading_day;;) {
        if (day == first_day_of_period) {
            return unexpected(format(
              "Starting from past_trading_day {}, couldn't find any trading day in the period.", past_trading_day
            ));
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

expected<Response, string> execute_after_trading_day(
  const chr::local_days past_trading_day,
  MarketData& market_data,
  const Config& config,
  UNUSED const Portfolio& portfolio
)
{
    if (config.equities.empty() || config.defensive_asset.empty()) {
        return unexpected("No equities or defensive asset specified");
    }
    vector<string> all_assets = config.equities;
    all_assets.push_back(config.defensive_asset);

    TRY_ASSIGN_OR_RETURN_UNEXPECTED_ERROR(
      bar_avail, do_all_assets_have_daily_bars(market_data, all_assets, past_trading_day)
    );

    const auto compute_has_daily_bar = [&]() -> expected<bool, string> {
        switch (bar_avail) {
        case DailyBarAvailability::available:
            return true;
        case DailyBarAvailability::not_traded:
        case DailyBarAvailability::after_last_bar:
            return false;
        case DailyBarAvailability::before_first_bar:
            return unexpected(format("past_trading_day ({}) is before some asset's first bar.", past_trading_day));
        }
    };
    TRY_CONST_ASSIGN_OR_RETURN_UNEXPECTED_ERROR(has_daily_bar, compute_has_daily_bar());

    // Check if the past_trading_day was a rebalance day.
    optional<chr::local_days> rebalance_day;
    switch (config.rebalance_day) {
    case RebalanceDay::first_trading_day_of_month: {
        const chr::year_month_day ymd{past_trading_day};
        const auto first_day_of_month = ymd.year() / ymd.month() / chr::day{1};
        if (auto r = nullopt_if_first_trading_day_of_period(
              market_data,
              all_assets,
              has_daily_bar,
              past_trading_day,
              "first trading day of month",
              chr::local_days(first_day_of_month)
            )) {
            return MOVE(*r);
        }
        rebalance_day = past_trading_day;
        break;
    }
    case RebalanceDay::last_trading_day_of_month: {
        const chr::year_month_day ymd{past_trading_day};
        const auto first_day_of_month = ymd.year() / ymd.month() / chr::day{1};
        const auto last_day_of_month = ymd.year() / ymd.month() / chr::last;
        auto r = last_trading_day_of_period_or_return_value(
          market_data,
          all_assets,
          has_daily_bar,
          past_trading_day,
          chr::local_days(first_day_of_month),
          chr::local_days(last_day_of_month),
          "last trading day of month"
        );
        if (auto* r1 = std::get_if<chr::local_days>(&r)) {
            rebalance_day = *r1;
        } else if (auto* r2 = std::get_if<expected<Response, string>>(&r)) {
            return MOVE(*r2);
        } else {
            LOG(FATAL) << format("Unexpected index {}", r.index());
        }
    } break;
    case RebalanceDay::first_trading_day_of_week: {
        // The Monday at or before `past_trading_day`.
        //
        // Monday and not Sunday, which is the other common convention: the trading
        // week is bounded by the two days nobody trades, so a Sunday-based week cuts
        // between Saturday and Sunday and puts a weekend in the middle. Every week
        // would then begin with a day that is never a trading day, which the scan
        // would step over -- harmless but pointless -- and, worse, a `past_trading_day`
        // falling on a Sunday would be attributed to the week that has not started yet.
        //
        // `wd - Monday` is modular and lands in [0, 6] days, so this needs no branch
        // and no ok() check: subtracting days from a date cannot leave the calendar.
        const chr::weekday wd{past_trading_day};
        const chr::local_days first_day_of_week = past_trading_day - (wd - chr::Monday);
        if (auto r = nullopt_if_first_trading_day_of_period(
              market_data, all_assets, has_daily_bar, past_trading_day, "first trading day of week", first_day_of_week
            )) {
            return MOVE(*r);
        }
        rebalance_day = past_trading_day;
        break;
    }
    case RebalanceDay::last_trading_day_of_week: {
        const chr::weekday wd{past_trading_day};
        const auto first_day_of_week = chr::local_days(past_trading_day - (wd - chr::Monday));
        auto r = last_trading_day_of_period_or_return_value(
          market_data,
          all_assets,
          has_daily_bar,
          past_trading_day,
          first_day_of_week,
          first_day_of_week + chr::days(6),
          "last trading day of week"
        );
        if (auto* r1 = std::get_if<chr::local_days>(&r)) {
            rebalance_day = *r1;
        } else if (auto* r2 = std::get_if<expected<Response, string>>(&r)) {
            return MOVE(*r2);
        } else {
            LOG(FATAL) << format("Unexpected index {}", r.index());
        }
    } break;
    }
    CHECK(rebalance_day);
    return unexpected("to be implemented");
}
} // namespace dual_mom_fixed_etf_algorithm
