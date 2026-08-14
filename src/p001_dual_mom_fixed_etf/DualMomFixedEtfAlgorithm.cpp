#include "DualMomFixedEtfAlgorithm.h"

#include <magic_enum/magic_enum.hpp>

#include "atlib/marketdata/MarketData.h"
#include "meadow/math.h"

namespace dual_mom_fixed_etf_algorithm
{
namespace
{
// `day` minus one `period`, on the calendar and not in days: a month back from a
// month-end is the previous month-end, whatever the two months are worth in days.
//
// Months need the special case because `chr::months` is not a whole number of
// days -- it is a fixed 30.436875-day average, so subtracting it from a date
// gives a time point part-way through a day and answers a question nobody asked.
// Weeks and days are exact multiples and need no help.
//
// With `jump_to`, the result is moved to one end of the period it lands in: the
// month's first or last calendar day, or the week's Monday or Sunday. Without it,
// the result keeps `day`'s position within the period.
//
// Told to the function rather than inferred from `day`, and that is the point. The
// inferable version -- "if `day` is the last day of its month, land on a last day
// too" -- looks equivalent and is not: the caller's signal is the last *trading*
// day, which coincides with the last *calendar* day only when the month happens
// not to end on a weekend or holiday. Inferring would therefore apply one
// convention in about seven months out of ten and the other in the rest,
// switching on something with no economic meaning. The caller knows which rule
// produced the day; the date does not.
//
// Either boundary lands strictly before `day` for any period of one or more, so
// the result is always a date the caller is entitled to look at.
//
// Meaningless for a `chr::days` period, which is a single day and has no ends to
// choose between, so asking is a caller bug rather than a request to be honoured
// somehow.
//
// The same function, minus the boundary, exists in playground/main.cpp. Whichever
// of the two outlives the other belongs in atlib next to the rest of the calendar
// arithmetic.
enum class JumpToPeriodBoundary {
    first_day,
    last_day
};

chr::local_days minus(
  chr::local_days day, const variant<chr::months, chr::weeks, chr::days>& period, optional<JumpToPeriodBoundary> jump_to
)
{
    return switch_variant(
      period,
      [day, jump_to](chr::months m) -> chr::local_days {
          const chr::year_month_day ymd{day};
          const chr::year_month back = chr::year_month(ymd.year(), ymd.month()) - m;

          if (jump_to) {
              switch (*jump_to) {
              case JumpToPeriodBoundary::first_day:
                  return chr::local_days(back / chr::day{1});
              case JumpToPeriodBoundary::last_day:
                  return chr::local_days(chr::year_month_day(back / chr::last));
              }
              std::unreachable();
          }

          // A day-of-month the shorter month does not have -- 31 March back one
          // month -- lands on its last day. Anything else would have to skip a
          // month or spill into the next one.
          const chr::year_month_day candidate = back / ymd.day();
          return chr::local_days(candidate.ok() ? candidate : chr::year_month_day(back / chr::last));
      },
      [day, jump_to](chr::weeks w) -> chr::local_days {
          // Subtracting whole weeks preserves the weekday, so without a boundary
          // there is nothing to decide.
          const chr::local_days back = day - w;
          if (!jump_to) {
              return back;
          }
          // Monday to Sunday, the same convention
          // get_past_trading_day_to_rebalance_after() uses when it bounds a week as
          // Monday plus six days. Both differences are modular and land in [0, 6],
          // so each stays inside `back`'s own week.
          const chr::weekday wd{back};
          switch (*jump_to) {
          case JumpToPeriodBoundary::first_day:
              return back - (wd - chr::Monday);
          case JumpToPeriodBoundary::last_day:
              return back + (chr::Sunday - wd);
          }
          std::unreachable();
      },
      [day, jump_to](chr::days d) -> chr::local_days {
          CHECK(!jump_to);
          return day - d;
      }
    );
}

// Which end of its period the lookback anchor should sit at, if either.
//
// `last_day` for the "last trading day of ..." rules, where the signal itself sits
// at the end of its period: anchoring mid-period there would measure from a
// partial period to a whole one.
//
// `first_day` for the "first trading day of ..." rules, the mirror image, which
// resolve_anchor() then walks forward from so the anchor stays inside the period
// whose first day it names.
//
// Nothing at all for a `chr::days` lookback, whatever the rule: a single day has no
// ends to choose between, and minus() treats being asked for one as a caller bug.
// Without this the two are reachable together -- a `days` lookback under a
// last-trading-day rule -- and that CHECK fires.
optional<JumpToPeriodBoundary>
anchor_boundary(RebalanceDay rebalance_day, const variant<chr::months, chr::weeks, chr::days>& lookback_period)
{
    if (HOLDS(lookback_period, chr::days)) {
        return nullopt;
    }
    switch (rebalance_day) {
    case RebalanceDay::first_trading_day_of_month:
    case RebalanceDay::first_trading_day_of_week:
        return JumpToPeriodBoundary::first_day;
    case RebalanceDay::last_trading_day_of_month:
    case RebalanceDay::last_trading_day_of_week:
        return JumpToPeriodBoundary::last_day;
    }
    std::unreachable();
}

expected<DailyBarAvailability, string>
do_all_assets_have_daily_bars(MarketData& market_data, const vector<string>& all_assets, chr::local_days day)
{
    CHECK(!all_assets.empty());
    TRY_ASSIGN_OR_RETURN_UNEXPECTED(auto bar_avail, market_data.daily_bar_availability(all_assets.front(), day));
    for (unsigned i = 1; i < all_assets.size(); ++i) {
        const auto& s = all_assets[i];
        TRY_ASSIGN_OR_RETURN_UNEXPECTED(const auto& b, market_data.daily_bar_availability(s, day));
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

// Everything both entry points need to be true of a Config before either touches
// any data. Checked in both, because either can be called first and a caller that
// only ever hits one of them would otherwise never learn the config is unusable.
expected<void, string> validate(const Config& config)
{
    if (config.equities.empty()) {
        return unexpected("no equities specified");
    }
    if (config.defensive_asset.empty()) {
        return unexpected("no defensive asset specified");
    }
    if (config.cash_proxy.empty()) {
        return unexpected("no cash proxy specified");
    }
    // Zero would reach the resize below and return an empty portfolio, which is
    // neither the equities nor the defensive asset and which a caller cannot tell
    // from "hold nothing on purpose". Negative would trip sucast()'s assert.
    if (config.max_portfolio_size < 1) {
        return unexpected(format("max_portfolio_size is {}, which would select nothing", config.max_portfolio_size));
    }
    return {};
}

// The latest day at or before `day` on which every asset has a bar.
//
// Walks back rather than forward so the window it opens covers the whole
// requested period rather than falling short of it, and so the choice does not
// depend on which side happens to be nearer.
//
// Unbounded, and it terminates: every history is finite, so a scan that finds no
// trading day eventually walks past some asset's first bar and reports that. A
// lookback reaching before an asset listed is a real failure and not a shorter
// window -- silently shortening it would return a number of a different quantity
// that nothing downstream could recognise as wrong.
expected<chr::local_days, string>
last_trading_day_on_or_before(MarketData& market_data, const vector<string>& all_assets, chr::local_days day)
{
    for (auto d = day;; --d) {
        TRY_ASSIGN_OR_RETURN_UNEXPECTED(const auto& avail, do_all_assets_have_daily_bars(market_data, all_assets, d));
        switch (avail) {
        case DailyBarAvailability::available:
            return d;
        case DailyBarAvailability::not_traded:
            // Leaves the switch, not the loop; `--d` in the for header runs next.
            break;
        case DailyBarAvailability::before_first_bar:
            return unexpected(
              format("scanning back from {}, reached {}, which is before some asset's first bar", day, d)
            );
        case DailyBarAvailability::after_last_bar:
            // `day` is at or before a day already established as a trading day, so
            // it cannot sit past the last visible bar, and nor can anything before
            // it. Same argument as the forward scan below.
            LOG(FATAL) << format(
              "day {} was reported as being after the last bar while scanning back from {}, which is a contradiction.",
              d,
              day
            );
        }
    }
}

// The earliest day at or after `day` on which every asset has a bar.
//
// The mirror of last_trading_day_on_or_before(), and needed for the same reason in
// the other direction: a `first_day` anchor names the start of a period, so the day
// it resolves to has to stay inside that period. Walking backwards from it would
// land in the period before whenever the first calendar day is not a trading day.
//
// Both ends of the history are errors, and unlike the backward scan neither is
// provably impossible: running out of data means the history does not reach the
// anchor, and starting before an asset's first bar means the lookback reaches back
// further than that asset exists. Either would otherwise be answered by silently
// returning a window of a different length.
//
// Walking forward is the direction that could trip the as_of guard, and does not:
// the caller has already established that `past_trading_day` is a trading day, and
// the anchor precedes it, so the scan finds a bar at or before `past_trading_day`
// and never asks about a later date.
expected<chr::local_days, string>
first_trading_day_on_or_after(MarketData& market_data, const vector<string>& all_assets, chr::local_days day)
{
    for (auto d = day;; ++d) {
        TRY_ASSIGN_OR_RETURN_UNEXPECTED(const auto& avail, do_all_assets_have_daily_bars(market_data, all_assets, d));
        switch (avail) {
        case DailyBarAvailability::available:
            return d;
        case DailyBarAvailability::not_traded:
            // Leaves the switch, not the loop; `++d` in the for header runs next.
            break;
        case DailyBarAvailability::before_first_bar:
            return unexpected(
              format("scanning forward from {}, reached {}, which is before some asset's first bar", day, d)
            );
        case DailyBarAvailability::after_last_bar:
            return unexpected(
              format("scanning forward from {}, reached {}, where the history runs out before any trading day", day, d)
            );
        }
    }
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
        TRY_ASSIGN_OR_RETURN_UNEXPECTED(
          const auto& day_avail, do_all_assets_have_daily_bars(market_data, all_assets, day)
        );
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
        TRY_ASSIGN_OR_RETURN_UNEXPECTED(
          const auto& day_avail, do_all_assets_have_daily_bars(market_data, all_assets, day)
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
    if (const auto valid = validate(config); !valid) {
        return unexpected(valid.error());
    }
    vector<string> all_assets = config.equities;
    all_assets.push_back(config.defensive_asset);
    TRY_ASSIGN_OR_RETURN_UNEXPECTED(
      const auto& bar_avail, do_all_assets_have_daily_bars(market_data, all_assets, past_day)
    );

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
    TRY_ASSIGN_OR_RETURN_UNEXPECTED(const auto& has_daily_bar, compute_has_daily_bar());

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

expected<Response, string> rebalance(MarketData& market_data, const Config& config, chr::local_days past_trading_day)
{
    if (const auto valid = validate(config); !valid) {
        return unexpected(valid.error());
    }
    // All assets must have data on past_trading_day.
    vector<string> all_assets = config.equities;
    all_assets.push_back(config.defensive_asset);
    TRY_ASSIGN_OR_RETURN_UNEXPECTED(
      const auto& bar_avail, do_all_assets_have_daily_bars(market_data, all_assets, past_trading_day)
    );
    if (bar_avail != DailyBarAvailability::available) {
        return unexpected(format("past_trading_day ({}) is not a trading day.", past_trading_day));
    }

    // The anchor: one `lookback_period` back on the calendar, then the last day at
    // or before that on which every asset traded.
    //
    // Resolved once, across all assets together, rather than per symbol. Snapping
    // per symbol would let two assets open their windows on different dates
    // whenever one has a holiday the other does not, and relative momentum would
    // then compare returns over unequal windows -- a small, plausible, invisible
    // bias in exactly the comparison that picks the holding.
    const optional<JumpToPeriodBoundary> boundary = anchor_boundary(config.rebalance_day, config.lookback_period);
    const auto anchor_day = minus(past_trading_day, config.lookback_period, boundary);
    // anchor day might or might not be a trading day. Scan in the proper direction to find the nearest trading day.
    TRY_ASSIGN_OR_RETURN_UNEXPECTED(
      const auto& anchor_trading_day,
      boundary == JumpToPeriodBoundary::first_day ? first_trading_day_on_or_after(market_data, all_assets, anchor_day)
                                                  : last_trading_day_on_or_before(market_data, all_assets, anchor_day)
    );

    // Compute equity return factors.
    vector<pair<double, string>> equities_and_return_factors;
    equities_and_return_factors.reserve(config.equities.size());
    for (const auto& s : config.equities) {
        TRY_ASSIGN_OR_RETURN_UNEXPECTED(
          const auto& total_return_factor,
          market_data.total_equity_return_factor_close_to_close(s, anchor_trading_day, past_trading_day)
        );
        equities_and_return_factors.emplace_back(total_return_factor, s);
    }

    // Compute the cash proxy return factor over the same window.
    TRY_ASSIGN_OR_RETURN_UNEXPECTED(
      const auto& cash_factor,
      market_data.total_cash_return_factor(config.cash_proxy, anchor_trading_day, past_trading_day)
    );

    // Absolute momentum, applied per asset rather than to the winner alone. At
    // max_portfolio_size 1 the two are the same -- if the winner fails the hurdle
    // nothing else can pass it -- and above 1 this is the generalisation that
    // keeps every held asset individually above cash. `<=` because "exceeds" is
    // strict: an equity exactly level with the hurdle does not qualify.
    const auto removed = ra::remove_if(equities_and_return_factors, [cash_factor](const auto& p) {
        return p.first <= cash_factor;
    });
    equities_and_return_factors.erase(removed.begin(), removed.end());

    if (equities_and_return_factors.empty()) {
        return Response{.desired_portfolio = {{config.defensive_asset}}};
    }

    // Descending by return factor, and *stable*, so equal factors keep the order
    // they were listed in `config.equities`.
    //
    // Not sort-then-reverse, which is what this was: sorting a pair orders by
    // symbol after factor, and reversing then flips that too, leaving a tie broken
    // in reverse alphabetical order. That happened to agree with the documented
    // "SPY over EFA" only because "SPY" > "EFA" -- rename an asset and the rule
    // silently inverts. Config order is a tie-break someone chose.
    ra::stable_sort(equities_and_return_factors, ra::greater{}, &pair<double, string>::first);

    if (cmp_greater(equities_and_return_factors.size(), config.max_portfolio_size)) {
        equities_and_return_factors.resize(sucast(config.max_portfolio_size));
    }

    vector<string> desired_portfolio;
    desired_portfolio.reserve(equities_and_return_factors.size());
    for (const auto& p : equities_and_return_factors) {
        desired_portfolio.push_back(p.second);
    }
    return Response{.desired_portfolio = MOVE(desired_portfolio)};
}
} // namespace dual_mom_fixed_etf_algorithm
