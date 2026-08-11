#include <atlib/marketdata/MarketData.h>

#include <atlib/marketdata/fred.h>
#include <atlib/marketdata/tiingo.h>

#include <meadow/file.h>

namespace
{

struct ProviderInfo {
    string_view name;
    string_view file_extension;
};

// Taken from the provider's own header rather than spelled again here, so the
// directory a payload is written to cannot drift from the module that writes it.
ProviderInfo info(Provider provider)
{
    switch (provider) {
    case Provider::tiingo:
        return {.name = tiingo::k_name, .file_extension = tiingo::k_file_extension};
    }
    std::unreachable();
}

expected<string, string> fetch(Provider provider, string_view symbol)
{
    switch (provider) {
    case Provider::tiingo:
        return tiingo::fetch(symbol);
    }
    std::unreachable();
}

expected<EquityHistory, string> parse(Provider provider, string_view symbol, string_view payload)
{
    switch (provider) {
    case Provider::tiingo:
        return tiingo::parse(symbol, payload);
    }
    std::unreachable();
}

// FRED is not in the Provider enum and so is spelled out here, once. See
// rate_history() in MarketData.h for why it is not a choice.
constexpr ProviderInfo k_fred{.name = fred::k_name, .file_extension = fred::k_file_extension};

// The layout, which is a property of the directory and not of the enum -- so
// FRED lands in it on the same terms as a vendor does, and the Python tools
// sharing the cache see one scheme rather than two.
fs::path payload_path(const MarketDataConfig& config, ProviderInfo provider, string_view symbol)
{
    return config.workspace_dir / "raw" / provider.name / format("{}.{}", symbol, provider.file_extension);
}

// The last date a history holds anything for, which is the whole of what the
// freshness rule and its error message need to know about the rows. Not public:
// it exists so the two kinds of history can go through one loader, and a caller
// wanting this has `bars` or `observations` right there.
optional<chr::local_days> last_date(const EquityHistory& history)
{
    if (history.bars.empty()) {
        return nullopt;
    }
    return history.bars.back().date;
}

optional<chr::local_days> last_date(const RateHistory& history)
{
    if (history.observations.empty()) {
        return nullopt;
    }
    return history.observations.back().date;
}

// The symbol becomes a path component, so it has to be one: a symbol containing a
// separator or spelling ".." would read and write outside the workspace.
//
// This is the filesystem's guard and only that. A provider that interpolates the
// symbol into a URL *path* needs its own check and has one -- see
// tiingo::fetch(). A provider that passes it as a query parameter, as fred does,
// needs none, because http_get percent-encodes those. Neither substitutes for
// this one, which runs before anything is opened.
bool is_plain_filename(string_view symbol)
{
    return !symbol.empty() && symbol != "." && symbol != ".." && symbol.find_first_of("/\\") == string_view::npos;
}

// Every row list is ascending by date, so what has to go is a suffix: find where
// it starts and drop the rest. Erasing a tail destroys elements without shifting
// any.
template<class Row, class DateOf>
void drop_after(vector<Row>& rows, chr::local_days as_of, DateOf date_of)
{
    rows.erase(ra::upper_bound(rows, as_of, {}, date_of), rows.end());
}

// The body of equity_history() and rate_history() both.
//
// What differs between them is only what a payload parses into; the rules -- read
// the cache, download at most once, refuse to return data that does not reach
// `as_of`, drop what is past it -- are the same rules, and they are the part
// worth getting right, so they are written once. covers() and truncate_to() are
// overloaded on the history type, which is what lets that happen.
//
// The documentation for all of it lives on the two public functions in
// MarketData.h. Comments here explain only the steps.
template<class History, class Fetch, class Parse>
expected<History, string> load_history(
  const MarketDataConfig& config,
  ProviderInfo provider,
  string_view symbol,
  chr::local_days as_of,
  Fetch fetch_payload,
  Parse parse_payload
)
{
    if (!is_plain_filename(symbol)) {
        return unexpected(format("not a usable symbol: \"{}\"", symbol));
    }

    const auto [name, file_extension] = provider;
    const fs::path path = payload_path(config, provider, symbol);

    optional<History> history;

    std::error_code ec;
    if (fs::exists(path, ec)) {
        const auto payload = read_file_to_string(path);
        if (!payload) {
            return unexpected(format("{}: {}", path.string(), payload.error()));
        }
        // A cached payload that will not parse is reported, not quietly replaced.
        // It arrived by a rename from a complete temporary, so it did not get that
        // way on its own, and downloading over it would erase the evidence of
        // whatever did.
        auto parsed = parse_payload(symbol, *payload);
        if (!parsed) {
            return unexpected(format("{}: {}", path.string(), parsed.error()));
        }
        history = std::move(*parsed);
    }

    if (!history || !covers(*history, as_of)) {
        const auto payload = fetch_payload(symbol);
        if (!payload) {
            return unexpected(payload.error());
        }
        if (const auto written = write_cache(path, *payload); !written) {
            return unexpected(written.error());
        }
        auto parsed = parse_payload(symbol, *payload);
        if (!parsed) {
            return unexpected(parsed.error());
        }
        history = std::move(*parsed);
    }

    if (!covers(*history, as_of)) {
        // One message for situations that are, from here, indistinguishable: the
        // caller asked before the session closed, or about a day the exchange was
        // shut, or about a symbol with no history at all. Naming the latest date
        // that does exist is what lets them tell which.
        const auto latest = last_date(*history);
        if (!latest) {
            return unexpected(format("{} {}: no data at all", name, symbol));
        }
        return unexpected(format("{} {}: no data for {}, latest available is {}", name, symbol, as_of, *latest));
    }

    truncate_to(*history, as_of);
    return std::move(*history);
}

} // namespace

fs::path cache_path(const MarketDataConfig& config, Provider provider, string_view symbol)
{
    return payload_path(config, info(provider), symbol);
}

bool covers(const EquityHistory& history, chr::local_days as_of)
{
    const auto latest = last_date(history);
    return latest && *latest >= as_of;
}

bool covers(const RateHistory& history, chr::local_days as_of)
{
    const auto latest = last_date(history);
    return latest && *latest >= as_of;
}

expected<void, string> write_cache(const fs::path& path, string_view payload)
{
    std::error_code ec;

    if (const fs::path parent = path.parent_path(); !parent.empty()) {
        fs::create_directories(parent, ec);
        if (ec) {
            return unexpected(format("{}: {}", parent.string(), ec.message()));
        }
    }

    // A sibling of the destination, so it lands on the same filesystem and the
    // rename below is atomic rather than a copy that can be interrupted.
    const fs::path partial = path.string() + ".part";

    if (const auto written = write_string_to_file(payload, partial); !written) {
        return unexpected(format("{}: {}", partial.string(), written.error()));
    }

    fs::rename(partial, path, ec);
    if (ec) {
        // A leftover .part would be read by nothing, but it would sit in the
        // workspace forever looking like a clue.
        std::error_code ignored;
        fs::remove(partial, ignored);
        return unexpected(format("{} -> {}: {}", partial.string(), path.string(), ec.message()));
    }

    return {};
}

void truncate_to(EquityHistory& history, chr::local_days as_of)
{
    drop_after(history.bars, as_of, &DailyBar::date);
    drop_after(history.distributions, as_of, &Distribution::ex_date);
    drop_after(history.splits, as_of, &Split::ex_date);
}

void truncate_to(RateHistory& history, chr::local_days as_of)
{
    drop_after(history.observations, as_of, &RateObservation::date);
}

expected<EquityHistory, string>
equity_history(const MarketDataConfig& config, Provider provider, string_view symbol, chr::local_days as_of)
{
    return load_history<EquityHistory>(
      config,
      info(provider),
      symbol,
      as_of,
      [provider](string_view s) {
          return fetch(provider, s);
      },
      [provider](string_view s, string_view payload) {
          return parse(provider, s, payload);
      }
    );
}

expected<RateHistory, string> rate_history(const MarketDataConfig& config, string_view symbol, chr::local_days as_of)
{
    return load_history<RateHistory>(config, k_fred, symbol, as_of, fred::fetch, fred::parse);
}
