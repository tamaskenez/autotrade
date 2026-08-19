#include "matlab_export.h"

#include <meadow/matlab.h>

namespace matlab_export
{
string assign_date_axis(string_view var, span<const chr::local_days> local_days)
{
    CHECK(!local_days.empty());

    vector<double> tick_datenums;
    vector<string> tick_labels;

    auto last_ymd = chr::year_month_day(local_days.front());
    string s = format("{}=[", var);

    for (auto d : local_days) {
        s += format(" {}", matlab::datenum(d));
        const auto ymd = chr::year_month_day(d);
        if (ymd.year() != last_ymd.year() && ymd.month() == chr::month(1)) {
            tick_datenums.push_back(matlab::datenum(d));
            tick_labels.push_back(format("{}", ymd.year()));
            last_ymd = ymd;
        } else if (ymd.month() != last_ymd.month()) {
            tick_datenums.push_back(matlab::datenum(d));
            if ((static_cast<unsigned>(ymd.month()) - 1) % 3 == 0) {
                tick_labels.push_back(format("{:%b}", ymd.month()));
            } else {
                tick_labels.emplace_back("|");
            }
            last_ymd = ymd;
        }
    }
    s += format("]';");
    s += format("{}_ticks={}';\n", var, tick_datenums);
    s += format("{}_labels=[", var);
    bool first = true;
    for (const auto& l : tick_labels) {
        if (first) {
            first = false;
        } else {
            s += "; ";
        }
        s += format("'{}'", l);
    }
    s += format("];\n");
    return s;
}

string assign_row_vector(string_view var, span<const double> values)
{
    return format("{}={}';\n", var, values);
}

string assign_matrix(string_view var, const vector<vector<double>>& values)
{
    string s = format("{}=[\n", var);
    for (const auto& row : values) {
        for (auto& x : row) {
            s += format(" {}", x);
        }
        s += '\n';
    }
    s += "];\n";
    return s;
}

string set_ticks_labels(string_view var)
{
    return format("set(gca, 'xtick', {}_ticks);\n", var) + format("set(gca, 'xticklabel', {}_labels);\n", var);
}
} // namespace matlab_export
