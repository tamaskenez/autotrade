#include <atlib/marketdata/tiingo.h>

#include <atlib/marketdata/credentials.h>
#include <atlib/net/download.h>

#include <rfl/json.hpp>

#include <cctype>
#include <charconv>

namespace tiingo
{
namespace
{

constexpr string_view k_prices_url = "https://api.tiingo.com/tiingo/daily/{}/prices";

// Tiingo returns only the most recent row unless a start date is given. This one
// predates every listing, so "the full history" needs no per-symbol knowledge.
constexpr string_view k_epoch = "1900-01-01";

constexpr chr::seconds k_timeout{60};

// Enough of a failing response to identify the failure, not so much that a stray
// HTML error page fills the log.
constexpr size_t k_error_body_limit = 500;

// The symbol is interpolated into the URL *path*, where a stray character does
// not merely fail: "../.." climbs out of /daily/ and addresses some other
// endpoint entirely, and a '?' or '#' truncates the path. Real tickers are
// alphanumeric with dots and dashes, so anything else is rejected rather than
// escaped -- no legitimate symbol needs escaping, and refusing is easier to
// reason about than encoding.
bool is_usable_symbol(string_view symbol)
{
    return !symbol.empty() && ra::all_of(symbol, [](const char c) {
        return std::isalnum(static_cast<unsigned char>(c)) != 0 || c == '.' || c == '-';
    });
}

// One row of the response, as it arrives.
//
// The adjOpen/adjHigh/adjLow/adjClose/adjVolume fields are deliberately absent:
// reflect-cpp ignores what nobody declares, and equity.h explains why the
// adjusted series is not wanted. Member names are snake_case and the reader is
// given SnakeCaseToCamelCase, so div_cash reads divCash off the wire without
// the struct having to adopt the vendor's spelling.
struct Row {
    string date;
    double open;
    double high;
    double low;
    double close;
    double volume;
    double div_cash;
    double split_factor;
};

// "1993-01-29T00:00:00.000Z" -> 1993-01-29.
//
// Only the date is meaningful. The time is always midnight UTC, which is not when
// anything traded, and is discarded rather than interpreted.
expected<chr::local_days, string> parse_date(string_view text)
{
    // Reject rather than repair. A date is the one field where a wrong-but-
    // plausible value cannot be spotted downstream: it would just quietly place
    // a bar on the wrong day.
    const auto malformed = [text] {
        return unexpected(format("not a date: \"{}\"", text));
    };

    if (text.size() < 10 || text[4] != '-' || text[7] != '-') {
        return malformed();
    }

    const auto number = [text](size_t offset, size_t length) -> optional<int> {
        int value = 0;
        const char* begin = text.data() + offset;
        const auto [end, ec] = std::from_chars(begin, begin + length, value);
        if (ec != std::errc{} || end != begin + length) {
            return nullopt;
        }
        return value;
    };

    const auto year = number(0, 4);
    const auto month = number(5, 2);
    const auto day = number(8, 2);
    if (!year || !month || !day) {
        return malformed();
    }

    const chr::year_month_day date{
      chr::year{*year}, chr::month{static_cast<unsigned>(*month)}, chr::day{static_cast<unsigned>(*day)}
    };
    if (!date.ok()) {
        return malformed();
    }

    return chr::local_days{date};
}

} // namespace

expected<string, string> fetch(string_view symbol)
{
    if (!is_usable_symbol(symbol)) {
        return unexpected(format("not a usable symbol: \"{}\"", symbol));
    }

    const auto key = api_key(k_name);
    if (!key) {
        return unexpected(key.error());
    }

    const auto response = http_get({
      .url = format(k_prices_url, symbol),
      .query = {{"startDate", string(k_epoch)}, {"format", "json"}},
      // A header rather than a token= query parameter: a token in the query
      // would end up in the cached payload's path, in logs, and in every error
      // message that quotes the request.
      .headers = {{"Authorization", format("Token {}", *key)}},
      .timeout = k_timeout,
    });
    if (!response) {
        return unexpected(response.error());
    }

    if (!response->ok()) {
        // The body carries the reason -- a bad key, an unknown ticker, a
        // quota -- and the status alone never distinguishes them.
        return unexpected(format(
          "tiingo {}: HTTP {}\n{}", symbol, response->status, string_view(response->body).substr(0, k_error_body_limit)
        ));
    }

    return response->body;
}

expected<EquityHistory, string> parse(string_view symbol, string_view payload)
{
    const auto rows = rfl::json::read<vector<Row>, rfl::SnakeCaseToCamelCase>(payload);
    if (!rows) {
        return unexpected(format("tiingo {}: {}", symbol, rows.error().what()));
    }

    EquityHistory history;
    history.symbol = string(symbol);
    history.bars.reserve(rows->size());

    for (const auto& row : *rows) {
        const auto date = parse_date(row.date);
        if (!date) {
            return unexpected(format("tiingo {}: {}", symbol, date.error()));
        }

        history.bars.push_back(
          {.date = *date, .open = row.open, .high = row.high, .low = row.low, .close = row.close, .volume = row.volume}
        );

        // Tiingo reports these on every row, zero and one on the overwhelming
        // majority of them. Keeping the non-events would bury the events.
        if (row.div_cash != 0.0) {
            history.distributions.push_back({.ex_date = *date, .amount = row.div_cash});
        }
        if (row.split_factor != 1.0) {
            history.splits.push_back({.ex_date = *date, .factor = row.split_factor});
        }
    }

    // Tiingo returns rows oldest-first, and everything downstream assumes it.
    // Checking rather than sorting: an unsorted or duplicated response means the
    // endpoint is not behaving as understood, and quietly repairing the order
    // would hide whatever else changed with it.
    const auto out_of_order = ra::adjacent_find(history.bars, [](const DailyBar& a, const DailyBar& b) {
        return a.date >= b.date;
    });
    if (out_of_order != history.bars.end()) {
        return unexpected(format("tiingo {}: rows are not in ascending date order at {}", symbol, out_of_order->date));
    }

    return history;
}

} // namespace tiingo
