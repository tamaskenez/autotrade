#include <atlib/marketdata/iso_date.h>

#include <charconv>

expected<chr::local_days, string> parse_iso_date(string_view text)
{
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
