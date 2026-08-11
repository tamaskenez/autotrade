#include <atlib/marketdata/fred.h>

#include <atlib/marketdata/iso_date.h>
#include <atlib/net/download.h>

#include <charconv>

namespace fred
{
namespace
{

constexpr string_view k_series_url = "https://fred.stlouisfed.org/graph/fredgraph.csv";

constexpr chr::seconds k_timeout{60};

// Enough of a failing response to identify the failure, not so much that a stray
// HTML error page fills the log.
constexpr size_t k_error_body_limit = 500;

// What quoting basis a series is published on, which is the one thing about it
// that is not in the payload. FRED serves the numbers and documents the basis on
// the series page, so the basis lives here.
//
// An unrecognised series is rejected rather than given a default. Every default
// is correct for some series and wrong for others -- DTB3 is a discount rate,
// DGS3MO over the same maturity is a bond-equivalent yield, and they differ by
// more than the spread this data is being used to resolve -- and a wrong basis
// does not look wrong. Same rule as the instrument registry in
// src/marketdata/instruments.toml: a typo does not become data.
optional<RateConvention> series_convention(string_view series_id)
{
    if (series_id == "DTB3") {
        return RateConvention::discount_360;
    }
    return nullopt;
}

// FRED writes an unavailable observation as an empty field, and used to write it
// as a lone ".". Both spellings mean the same thing and neither is a number.
bool is_no_observation(string_view value)
{
    return value.empty() || value == ".";
}

expected<double, string> parse_number(string_view text)
{
    double value = 0;
    const auto [end, ec] = std::from_chars(text.data(), text.data() + text.size(), value);
    if (ec != std::errc{} || end != text.data() + text.size()) {
        return unexpected(format("not a number: \"{}\"", text));
    }
    return value;
}

} // namespace

expected<string, string> fetch(string_view series_id)
{
    // No symbol validation here, unlike tiingo::fetch(). The id goes into a query
    // parameter, which http_get percent-encodes, so there is no path to climb out
    // of and nothing to truncate the URL. The filesystem is guarded where it is
    // used, in cache_path()'s caller.
    const auto response = http_get({
      .url = string(k_series_url),
      .query = {{"id", string(series_id)}},
      .headers = {},
      .timeout = k_timeout,
    });
    if (!response) {
        return unexpected(response.error());
    }

    if (!response->ok()) {
        return unexpected(format(
          "fred {}: HTTP {}\n{}", series_id, response->status, string_view(response->body).substr(0, k_error_body_limit)
        ));
    }

    return response->body;
}

expected<RateHistory, string> parse(string_view series_id, string_view payload)
{
    const auto convention = series_convention(series_id);
    if (!convention) {
        return unexpected(format("fred {}: no quoting convention recorded for this series", series_id));
    }

    RateHistory history;
    history.symbol = string(series_id);
    history.convention = *convention;
    history.observations.reserve(sucast(ra::count(payload, '\n')));

    size_t line_number = 0;
    for (size_t begin = 0; begin < payload.size();) {
        const size_t newline = payload.find('\n', begin);
        const size_t end = newline == string_view::npos ? payload.size() : newline;

        string_view line = payload.substr(begin, end - begin);
        begin = end + 1;
        ++line_number;

        // Tolerated rather than expected: the endpoint serves LF today, and a
        // payload that arrives or is cached with CRLF should not become 18939
        // unparseable numbers.
        if (line.ends_with('\r')) {
            line.remove_suffix(1);
        }
        if (line.empty()) {
            continue;
        }

        const size_t comma = line.find(',');
        if (comma == string_view::npos) {
            return unexpected(format("fred {}: line {}: not two fields: \"{}\"", series_id, line_number, line));
        }
        const string_view first = line.substr(0, comma);
        const string_view second = line.substr(comma + 1);

        if (line_number == 1) {
            // The value column is named after the series, and checking it is the
            // only thing standing between us and quietly reading one series as
            // another -- a cache file saved under the wrong name, or an id that
            // FRED resolved to something else. The date column is not checked:
            // FRED has renamed it once already and the name says nothing.
            if (second != series_id) {
                return unexpected(format("fred {}: payload is for \"{}\"", series_id, second));
            }
            continue;
        }

        if (is_no_observation(second)) {
            continue;
        }

        const auto date = parse_iso_date(first);
        if (!date) {
            return unexpected(format("fred {}: line {}: {}", series_id, line_number, date.error()));
        }

        const auto value = parse_number(second);
        if (!value) {
            return unexpected(format("fred {}: line {}: {}", series_id, line_number, value.error()));
        }

        history.observations.push_back({.date = *date, .value = *value});
    }

    // FRED returns rows oldest-first, and everything downstream assumes it.
    // Checking rather than sorting, for the same reason tiingo::parse() does: an
    // unsorted or duplicated response means the endpoint is not behaving as
    // understood, and repairing it here would hide whatever else changed with it.
    const auto out_of_order =
      ra::adjacent_find(history.observations, [](const RateObservation& a, const RateObservation& b) {
          return a.date >= b.date;
      });
    if (out_of_order != history.observations.end()) {
        return unexpected(format("fred {}: rows are not in ascending date order at {}", series_id, out_of_order->date));
    }

    return history;
}

} // namespace fred
