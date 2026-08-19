#include <atlib/marketdata/equity.h>

void EquityHistory::adjust()
{
    if (!bars.empty()) {
        // Both cursors open just past the last bar and only ever retreat, so every
        // event is consumed exactly once, by the first bar on or after its ex-date --
        // the attribution total_return_factor_close_to_close() makes. An event dated
        // after the last bar is never reached: there is no bar it could restate.
        auto distribution = ra::upper_bound(distributions, bars.back().date, {}, &Distribution::ex_date);
        auto split = ra::upper_bound(splits, bars.back().date, {}, &Split::ex_date);

        // What the bar being visited is multiplied by. 1.0 at the last bar -- that is
        // the normalization -- and one event's worth heavier per step backwards, so
        // the walk has to run from the end and cannot be turned around.
        double price_factor = 1.0;
        double volume_factor = 1.0;

        for (auto bar = bars.end(); bar-- != bars.begin();) {
            // Read before the bar is restated: the dividend term below is a ratio
            // against the raw close and is not scale-invariant, unlike the split.
            const double raw_close = bar->close;

            bar->open *= price_factor;
            bar->high *= price_factor;
            bar->low *= price_factor;
            bar->close *= price_factor;
            bar->volume *= volume_factor;

            if (bar == bars.begin()) {
                break; // Nothing earlier for this bar's events to restate.
            }

            // Everything since the previous bar, not everything dated today. An
            // ex-date that is not a trading day would otherwise be stepped over and
            // lost; attributing it to the next session is the lesser wrong, and it
            // guarantees every event inside the history is accounted for.
            double amount = 0.0;
            while (distribution != distributions.begin() && (distribution - 1)->ex_date > (bar - 1)->date) {
                --distribution;
                amount += distribution->amount;
            }
            double split_factor = 1.0;
            while (split != splits.begin() && (split - 1)->ex_date > (bar - 1)->date) {
                --split;
                split_factor *= split->factor;
            }

            // Left alone these divide to an infinity or a NaN, and the factor is
            // cumulative -- so one broken row would not fail, it would quietly turn
            // every earlier bar in the series into a NaN.
            CHECK(split_factor > 0.0) << format("{}: split factor on {} is {}", symbol, bar->date, split_factor);
            if (amount != 0.0) {
                CHECK(raw_close > 0.0 && raw_close + amount > 0.0)
                  << format("{}: close on {} is {}, distribution {}", symbol, bar->date, raw_close, amount);
            }

            // Inverse of the one-bar total return: this bar's close over what a
            // holder of the previous one earned into it. The dividend is added before
            // the split is undone, because the vendor quotes both on the ex-date row
            // and a cash amount there is per post-split share, as `close` is.
            price_factor *= raw_close / ((raw_close + amount) * split_factor);
            // Volume moves the other way and on splits only: a dividend does not
            // change how many shares changed hands.
            volume_factor *= split_factor;
        }
    }

    distributions.clear();
    splits.clear();
}
