#include <atlib/marketdata/total_return.h>

namespace
{

// The bar dated exactly `date`, or bars.end(). Ascending order is a parse
// postcondition -- tiingo.cpp rejects a payload that violates it -- so this is a
// binary search rather than a scan.
auto find_bar(const vector<DailyBar>& bars, chr::local_days date)
{
    const auto bar = ra::lower_bound(bars, date, {}, &DailyBar::date);
    return bar != bars.end() && bar->date == date ? bar : bars.end();
}

// Naming the range that does exist is what makes the failure diagnosable: it
// separates "too early", "not a trading day" and "before this symbol listed",
// which are otherwise the same message. Same reasoning as equity_history().
string no_bar(const EquityHistory& history, chr::local_days date)
{
    if (history.bars.empty()) {
        return format("{}: no bar for {}, history is empty", history.symbol, date);
    }
    return format(
      "{}: no bar for {}, history covers {} to {}",
      history.symbol,
      date,
      history.bars.front().date,
      history.bars.back().date
    );
}

} // namespace

expected<double, string>
total_return_factor_close_to_close(const EquityHistory& history, chr::local_days from, chr::local_days to)
{
    if (from >= to) {
        return unexpected(format("{}: window does not run forwards: {} to {}", history.symbol, from, to));
    }

    const auto first = find_bar(history.bars, from);
    if (first == history.bars.end()) {
        return unexpected(no_bar(history, from));
    }
    const auto last = find_bar(history.bars, to);
    if (last == history.bars.end()) {
        return unexpected(no_bar(history, to));
    }

    // Both cursors open past `from` and only ever advance, so every event in the
    // window is consumed exactly once and an event dated `from` is never reached.
    // That is the half-open boundary, enforced by where the walk starts rather
    // than by a condition inside the loop that could be read the other way.
    auto distribution = ra::upper_bound(history.distributions, from, {}, &Distribution::ex_date);
    auto split = ra::upper_bound(history.splits, from, {}, &Split::ex_date);

    double factor = 1.0;

    for (auto bar = first + 1; bar <= last; ++bar) {
        const double previous_close = (bar - 1)->close;
        if (!(previous_close > 0.0)) {
            // Left alone this divides to an infinity or a NaN, and a NaN loses
            // every comparison it takes part in -- so a broken bar would not
            // fail, it would quietly decide which asset to hold for a month.
            return unexpected(format("{}: close on {} is {}", history.symbol, (bar - 1)->date, previous_close));
        }

        // Everything since the previous bar, not everything dated today. An
        // ex-date that is not a trading day would otherwise be stepped over and
        // lost; attributing it to the next session is the lesser wrong, and it
        // guarantees the window accounts for every event inside it.
        double amount = 0.0;
        for (; distribution != history.distributions.end() && distribution->ex_date <= bar->date; ++distribution) {
            amount += distribution->amount;
        }
        double split_factor = 1.0;
        for (; split != history.splits.end() && split->ex_date <= bar->date; ++split) {
            split_factor *= split->factor;
        }

        // The dividend is added before the split is undone, because the two are
        // quoted in the same units: the vendor reports both on the ex-date row,
        // so a cash amount there is per post-split share, as `close` is.
        factor *= (bar->close + amount) * split_factor / previous_close;
    }

    return factor;
}
