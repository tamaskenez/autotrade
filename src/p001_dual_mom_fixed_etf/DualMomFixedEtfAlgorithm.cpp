#if 0
  #include "DualMomFixedEtfAlgorithm.h"


namespace dual_mom_fixed_etf_algorithm {
    expected<Response, string> execute_after_trading_day(chr::local_days past_trading_day, MarketData &market_data,
                                                         const Config &config,
                                                         const Portfolio &portfolio) {
        if (config.equities.empty() || config.defensive_asset.empty()) {
            return unexpected("No equities or defensive asset specified");
        }
        vector<string> all_assets = config.equities;
        all_assets.push_back(config.defensive_asset);

        const auto do_all_assets_have_daily_bars = [&all_assets,&market_data
                ](chr::local_days day) -> expected<bool, string> {
            CHECK(!all_assets.empty());
            const auto r = market_data.has_daily_bar(day, all_assets.front());
            if (!r) {
                return unexpected(r.error());
            }
            const auto has_daily_bar = *r;
            for (unsigned i = 1; i < all_assets.size(); ++i) {
                const auto &s = all_assets[i];
                const auto r2 = market_data.has_daily_bar(day, s);
                if (!r2) {
                    return unexpected(r2.error());
                }
                const bool b = *r2;
                if (b != has_daily_bar) {
                    return unexpected("Mixed data availability for past_trading_day ({}): {}: {} vs. {}: {}",
                                      day, s, b, all_assets.front(), has_daily_bar);
                }
            }
            return has_daily_bar;
        };

        const auto r3 = do_all_assets_have_daily_bars(past_trading_day);
        if (!r3) {
            return unexpected(r3.error());
        }
        const bool has_daily_bar = *r3;

        if (!has_daily_bar) {
            return Response{
                .info = format("Nothing to do, there's no data available for past_trading_day ({})",
                               past_trading_day)
                //                .info = format("Nothing to do, past_trading_day ({}) is not a rebalance day per rule \"{}\"",
                //                             past_trading_day, magic_enum::enum_name(config.rebalance_day))
            };
        }
        // Check if the past_trading_day was a rebalance day.
        switch (config.rebalance_day) {
            case RebalanceDay::first_trading_day_of_month:
                // TODO: start check the availability of daily bars for the days of this month, 1, 2, 3... past_trading_day.
                // If past_trading_day is the first day with daily bars, then this is a rebalance day, otherwise
                // return an empty Response and explain in the info field.
                break;
            case RebalanceDay::last_trading_day_of_month:
                // TODO: if this isn't the last day of the month, return an empty Response with an explanation.
                // Otherwise, start going back from the last day and check the availability of the daily bars and find
                // the actual last trading day of the month.
                break;
            case RebalanceDay::first_trading_day_of_week:
                //TODO: same idea as above, but for the week.
                break;
            case RebalanceDay::last_trading_day_of_week:
                //TODO: same idea as above, but for the week.
                break;
        }
    }
}
#endif
