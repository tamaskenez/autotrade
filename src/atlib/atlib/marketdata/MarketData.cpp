#include <atlib/marketdata/MarketData.h>

#include <atlib/marketdata/fred.h>
#include <atlib/marketdata/tiingo.h>
#include <atlib/marketdata/total_return.h>

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

// The mirror image: drop the prefix dated before `first_kept`.
template<class Row, class DateOf>
void drop_before(vector<Row>& rows, chr::local_days first_kept, DateOf date_of)
{
    rows.erase(rows.begin(), ra::lower_bound(rows, first_kept, {}, date_of));
}

// The end of the prefix of `rows` dated before `day`.
template<class Row, class DateOf>
auto end_before(const vector<Row>& rows, chr::local_days day, DateOf date_of)
{
    return ra::lower_bound(rows, day, {}, date_of);
}

// Whether anything is dated exactly `day` in either event list.
bool has_corporate_action(const EquityHistory& history, chr::local_days day)
{
    return ra::binary_search(history.distributions, day, {}, &Distribution::ex_date)
        || ra::binary_search(history.splits, day, {}, &Split::ex_date);
}

// The bar dated exactly `day`, or nullptr.
const DailyBar* bar_on(const EquityHistory& history, chr::local_days day)
{
    const auto bar = ra::lower_bound(history.bars, day, {}, &DailyBar::date);
    return bar != history.bars.end() && bar->date == day ? &*bar : nullptr;
}

// The cached payload at `path`, parsed. nullopt when there is nothing cached --
// which is not a failure, it is the state before the first download.
//
// A cached payload that will not parse is reported, not quietly replaced. It
// arrived by a rename from a complete temporary, so it did not get that way on
// its own, and downloading over it would erase the evidence of whatever did.
template<class History, class Parse>
expected<optional<History>, string> read_cached(const fs::path& path, string_view symbol, Parse parse_payload)
{
    std::error_code ec;
    if (!fs::exists(path, ec)) {
        return optional<History>{};
    }

    const auto payload = read_file_to_string(path);
    if (!payload) {
        return unexpected(format("{}: {}", path.string(), payload.error()));
    }
    auto parsed = parse_payload(symbol, *payload);
    if (!parsed) {
        return unexpected(format("{}: {}", path.string(), parsed.error()));
    }
    return optional<History>(std::move(*parsed));
}

// Downloads the full history, replaces the cached payload with it, and parses it.
template<class History, class Fetch, class Parse>
expected<History, string>
download_to_cache(const fs::path& path, string_view symbol, Fetch fetch_payload, Parse parse_payload)
{
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
    return std::move(*parsed);
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

    auto cached = read_cached<History>(path, symbol, parse_payload);
    if (!cached) {
        return unexpected(cached.error());
    }
    optional<History> history = std::move(*cached);

    if (!history || !covers(*history, as_of)) {
        auto downloaded = download_to_cache<History>(path, symbol, fetch_payload, parse_payload);
        if (!downloaded) {
            return unexpected(downloaded.error());
        }
        history = std::move(*downloaded);
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

// The body of MarketData::equity() and MarketData::rate() both.
//
// Same rules as load_history() above -- read the cache, download at most once --
// and one more that only an in-memory cache needs: remember that the download
// happened. The documentation is on the members in MarketData.h; comments here
// explain only the steps.
template<class History, class Map, class Fetch, class Parse>
expected<const History*, string> load_into_cache(
  Map& cache,
  const MarketDataConfig& config,
  ProviderInfo provider,
  string_view symbol,
  chr::local_days day,
  Fetch fetch_payload,
  Parse parse_payload
)
{
    auto entry = cache.find(symbol);
    if (entry == cache.end()) {
        if (!is_plain_filename(symbol)) {
            return unexpected(format("not a usable symbol: \"{}\"", symbol));
        }
        auto cached = read_cached<History>(payload_path(config, provider, symbol), symbol, parse_payload);
        if (!cached) {
            return unexpected(cached.error());
        }
        // Inserted even when there was nothing on disk, so that the download below
        // is attempted once for this symbol and not once per query.
        entry = cache.try_emplace(string(symbol)).first;
        entry->second.history = std::move(*cached);
    }

    auto& cached = entry->second;

    if (!cached.history || !covers(*cached.history, day)) {
        if (cached.download_error) {
            return unexpected(*cached.download_error);
        }
        if (!cached.downloaded) {
            // Marked before the attempt rather than after it, so that a download
            // that throws or fails cannot be retried by the next query either.
            cached.downloaded = true;
            auto downloaded =
              download_to_cache<History>(payload_path(config, provider, symbol), symbol, fetch_payload, parse_payload);
            if (!downloaded) {
                cached.download_error = downloaded.error();
                return unexpected(*cached.download_error);
            }
            cached.history = std::move(*downloaded);
        }
    }

    if (!cached.history) {
        return unexpected(format("{} {}: no data at all", provider.name, symbol));
    }
    return &*cached.history;
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

expected<const EquityHistory*, string> MarketData::equity(string_view symbol, chr::local_days day)
{
    return load_into_cache<EquityHistory>(
      equities,
      config,
      info(provider),
      symbol,
      day,
      [this](string_view s) {
          return fetch(provider, s);
      },
      [this](string_view s, string_view payload) {
          return parse(provider, s, payload);
      }
    );
}

expected<const RateHistory*, string> MarketData::rate(string_view symbol, chr::local_days day)
{
    return load_into_cache<RateHistory>(rates, config, k_fred, symbol, day, fred::fetch, fred::parse);
}

span<const DailyBar> MarketData::visible(const vector<DailyBar>& bars) const
{
    if (!as_of) {
        return bars;
    }
    return {bars.begin(), ra::upper_bound(bars, *as_of, {}, &DailyBar::date)};
}

expected<double, string>
MarketData::total_equity_return_factor_close_to_close(string_view symbol, chr::local_days from, chr::local_days to)
{
    // Both ends, not just the later one: `from` past the guard with `to` before it
    // is not a window that runs backwards, it is a caller that has lost track of
    // which way time goes, and the free function would report it as the former.
    CHECK(!as_of || from <= *as_of);
    CHECK(!as_of || to <= *as_of);

    // `to` is the further of the two, so covering it covers the window.
    const auto history = equity(symbol, to);
    if (!history) {
        return unexpected(history.error());
    }

    return total_return_factor_close_to_close(**history, from, to);
}

expected<double, string>
MarketData::total_cash_return_factor(string_view symbol, chr::local_days from, chr::local_days to)
{
    CHECK(!as_of || from <= *as_of);
    CHECK(!as_of || to <= *as_of);

    const auto history = rate(symbol, to);
    if (!history) {
        return unexpected(history.error());
    }

    return cash_return_factor(**history, from, to);
}

expected<DailyBarAvailability, string> MarketData::daily_bar_availability(string_view symbol, chr::local_days day)
{
    // The guard, and the reason it terminates rather than reporting: see MarketData.h.
    CHECK(!as_of || day <= *as_of);

    const auto history = equity(symbol, day);
    if (!history) {
        return unexpected(history.error());
    }

    // Everything below is decided against the visible range and never against the
    // rows behind it, which is what makes both ends of the answer point-in-time:
    // `as_of` moves where the history ends, not just what can be read out of it.
    const span<const DailyBar> bars = visible((*history)->bars);

    if (bars.empty()) {
        // Nothing visible: either the guard precedes this instrument's inception,
        // which is what this reports, or the payload has no bars at all -- a
        // degenerate case in which neither end is informative and no bar is going
        // to be found either way.
        return DailyBarAvailability::before_first_bar;
    }
    if (day < bars.front().date) {
        return DailyBarAvailability::before_first_bar;
    }
    if (day > bars.back().date) {
        return DailyBarAvailability::after_last_bar;
    }

    // Inside the range, so absence here is a closure and not a shortage of data.
    const auto bar = ra::lower_bound(bars, day, {}, &DailyBar::date);
    return bar != bars.end() && bar->date == day ? DailyBarAvailability::available : DailyBarAvailability::not_traded;
}

expected<DailyBar, string> MarketData::daily_bar(string_view symbol, chr::local_days day)
{
    // The guard, and the reason it terminates rather than reporting: see MarketData.h.
    CHECK(!as_of || day <= *as_of);

    const auto history = equity(symbol, day);
    if (!history) {
        return unexpected(history.error());
    }

    const span<const DailyBar> bars = visible((*history)->bars);
    const auto bar = ra::lower_bound(bars, day, {}, &DailyBar::date);
    if (bar == bars.end() || bar->date != day) {
        if (bars.empty()) {
            return unexpected(format("{}: no bars at or before {}", symbol, day));
        }
        // The range rather than a verdict, because it answers all three absences
        // at once: earlier than the first bar, later than the last, or a day the
        // exchange was shut between them.
        return unexpected(format("{}: no bar for {}, have {} to {}", symbol, day, bars.front().date, bars.back().date));
    }
    return *bar;
}

expected<CorporateActions, string> MarketData::corporate_actions(string_view symbol, chr::local_days day)
{
    // The guard, and the reason it terminates rather than reporting: see MarketData.h.
    // It is also what makes reading the rows directly safe here -- an event dated
    // exactly `day` is at or before `as_of` by construction.
    CHECK(!as_of || day <= *as_of);

    const auto history = equity(symbol, day);
    if (!history) {
        return unexpected(history.error());
    }

    // Ranges rather than a single match, though tiingo.cpp guarantees at most one
    // of each per date -- it rejects a payload whose rows are not strictly
    // ascending. Composing costs nothing and is the answer that stays right for a
    // provider that reports a date twice, where picking one would lose half an
    // event without saying so.
    CorporateActions actions;
    for (const auto& split : ra::equal_range((*history)->splits, day, {}, &Split::ex_date)) {
        actions.split_factor *= split.factor;
    }
    for (const auto& distribution : ra::equal_range((*history)->distributions, day, {}, &Distribution::ex_date)) {
        actions.distribution_amount += distribution.amount;
    }
    return actions;
}

expected<void, string> MarketData::prepend_equity_with_proxy(
  string_view prepended_symbol,
  chr::local_days prepended_symbol_as_of,
  string_view proxy_symbol,
  chr::days ignore_first_days
)
{
    // The guard, and the reason it terminates rather than reporting: see MarketData.h.
    CHECK(!as_of || prepended_symbol_as_of <= *as_of);

    // Rejected here rather than falling out of the anchor search below, because the
    // two histories would be the same cache entry and the splice would read the rows
    // it is rewriting.
    if (prepended_symbol == proxy_symbol) {
        return unexpected(format("{}: cannot be its own proxy", prepended_symbol));
    }

    const auto prepended_loaded = equity(prepended_symbol, prepended_symbol_as_of);
    if (!prepended_loaded) {
        return unexpected(prepended_loaded.error());
    }
    const auto proxy_loaded = equity(proxy_symbol, prepended_symbol_as_of);
    if (!proxy_loaded) {
        return unexpected(proxy_loaded.error());
    }
    const EquityHistory& proxy = **proxy_loaded;

    // Mutable access to what equity() just put in the cache. The map is node-based,
    // so loading the proxy did not move the entry found here.
    const auto entry = equities.find(prepended_symbol);
    CHECK(entry != equities.end() && entry->second.history);
    EquityHistory& prepended = *entry->second.history;

    if (prepended.bars.empty()) {
        return unexpected(format("{}: no bars at all", prepended_symbol));
    }

    // Everything below decides before anything is written, so a call that fails
    // leaves the cache exactly as it found it.

    const chr::local_days after_ramp_up = prepended.bars.front().date + ignore_first_days;
    const auto ramped_up = ra::lower_bound(prepended.bars, after_ramp_up, {}, &DailyBar::date);
    if (ramped_up == prepended.bars.end()) {
        return unexpected(format(
          "{}: no bars left after ignoring the first {} days from {}",
          prepended_symbol,
          ignore_first_days.count(),
          prepended.bars.front().date
        ));
    }

    // A day both traded and neither went ex on. An event on either side would put a
    // price step into the splice that one day's opens cannot represent.
    const DailyBar* anchor_bar = nullptr;
    const DailyBar* anchor_proxy_bar = nullptr;
    for (auto bar = ramped_up; bar != prepended.bars.end(); ++bar) {
        const DailyBar* const proxy_bar = bar_on(proxy, bar->date);
        if (proxy_bar != nullptr && !has_corporate_action(prepended, bar->date)
            && !has_corporate_action(proxy, bar->date)) {
            anchor_bar = &*bar;
            anchor_proxy_bar = proxy_bar;
            break;
        }
    }
    if (anchor_bar == nullptr) {
        return unexpected(format(
          "{} and {}: no day between {} and {} on which both traded and neither went ex",
          prepended_symbol,
          proxy_symbol,
          ramped_up->date,
          prepended.bars.back().date
        ));
    }

    const chr::local_days anchor = anchor_bar->date;
    if (!(anchor_bar->open > 0.0) || !(anchor_proxy_bar->open > 0.0)) {
        return unexpected(format(
          "{} and {}: cannot scale from the opens on {}, {} and {}",
          prepended_symbol,
          proxy_symbol,
          anchor,
          anchor_bar->open,
          anchor_proxy_bar->open
        ));
    }
    const double price_factor = anchor_bar->open / anchor_proxy_bar->open;

    const auto proxy_bars_end = end_before(proxy.bars, anchor, &DailyBar::date);
    if (proxy_bars_end == proxy.bars.begin()) {
        return unexpected(format("{}: no history before {} to prepend to {}", proxy_symbol, anchor, prepended_symbol));
    }
    const auto proxy_distributions_end = end_before(proxy.distributions, anchor, &Distribution::ex_date);
    const auto proxy_splits_end = end_before(proxy.splits, anchor, &Split::ex_date);

    vector<DailyBar> bars;
    bars.reserve(static_cast<size_t>(proxy_bars_end - proxy.bars.begin()) + prepended.bars.size());
    for (auto bar = proxy.bars.begin(); bar != proxy_bars_end; ++bar) {
        bars.push_back(
          DailyBar{
            .date = bar->date,
            .open = bar->open * price_factor,
            .high = bar->high * price_factor,
            .low = bar->low * price_factor,
            .close = bar->close * price_factor,
            // Not scaled: a volume is a share count, and these are the proxy's shares.
            .volume = bar->volume,
          }
        );
    }

    vector<Distribution> distributions;
    distributions.reserve(
      static_cast<size_t>(proxy_distributions_end - proxy.distributions.begin()) + prepended.distributions.size()
    );
    for (auto distribution = proxy.distributions.begin(); distribution != proxy_distributions_end; ++distribution) {
        // Quoted in the same units as the row it lands on, so it scales with them.
        distributions.push_back(
          Distribution{.ex_date = distribution->ex_date, .amount = distribution->amount * price_factor}
        );
    }

    // Splits carry over untouched: a ratio has no units to scale, and the proxy's
    // pre-anchor prices are on its own pre-split basis, which the copies above
    // preserve. Uniform scaling leaves that internally consistent.
    vector<Split> splits(proxy.splits.begin(), proxy_splits_end);
    splits.reserve(splits.size() + prepended.splits.size());

    drop_before(prepended.bars, anchor, &DailyBar::date);
    drop_before(prepended.distributions, anchor, &Distribution::ex_date);
    drop_before(prepended.splits, anchor, &Split::ex_date);

    bars.insert(bars.end(), prepended.bars.begin(), prepended.bars.end());
    distributions.insert(distributions.end(), prepended.distributions.begin(), prepended.distributions.end());
    splits.insert(splits.end(), prepended.splits.begin(), prepended.splits.end());

    prepended.bars = std::move(bars);
    prepended.distributions = std::move(distributions);
    prepended.splits = std::move(splits);

    return {};
}
