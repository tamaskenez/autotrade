#include <atlib/marketdata/MarketData.h>

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

// The symbol becomes a path component, so it has to be one: a symbol containing a
// separator or spelling ".." would read and write outside the workspace.
//
// The provider validates the symbol too, for the URL, and neither check
// substitutes for the other -- this one guards the filesystem and runs before
// anything is opened, that one guards the request path. They happen to agree today
// because real tickers are boring.
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

} // namespace

fs::path cache_path(const MarketDataConfig& config, Provider provider, string_view symbol)
{
    const auto [name, file_extension] = info(provider);
    return config.workspace_dir / "raw" / name / format("{}.{}", symbol, file_extension);
}

bool covers(const EquityHistory& history, chr::local_days as_of)
{
    return !history.bars.empty() && history.bars.back().date >= as_of;
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

expected<EquityHistory, string>
equity_history(const MarketDataConfig& config, Provider provider, string_view symbol, chr::local_days as_of)
{
    if (!is_plain_filename(symbol)) {
        return unexpected(format("not a usable symbol: \"{}\"", symbol));
    }

    const auto [name, file_extension] = info(provider);
    const fs::path path = cache_path(config, provider, symbol);

    optional<EquityHistory> history;

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
        auto parsed = parse(provider, symbol, *payload);
        if (!parsed) {
            return unexpected(format("{}: {}", path.string(), parsed.error()));
        }
        history = std::move(*parsed);
    }

    if (!history || !covers(*history, as_of)) {
        const auto payload = fetch(provider, symbol);
        if (!payload) {
            return unexpected(payload.error());
        }
        if (const auto written = write_cache(path, *payload); !written) {
            return unexpected(written.error());
        }
        auto parsed = parse(provider, symbol, *payload);
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
        if (history->bars.empty()) {
            return unexpected(format("{} {}: no data at all", name, symbol));
        }
        return unexpected(
          format("{} {}: no data for {}, latest available is {}", name, symbol, as_of, history->bars.back().date)
        );
    }

    truncate_to(*history, as_of);
    return std::move(*history);
}
