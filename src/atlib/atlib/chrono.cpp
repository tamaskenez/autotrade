#include "chrono.h"

double years_between_days(chr::local_days d1, chr::local_days d2)
{
    return chr::duration<double>(d2 - d1) / chr::duration<double>(chr::years(1));
}

bool is_on_or_later_than_next_year_same_date(chr::local_days d1, chr::local_days d2)
{
    if (d2 - d1 < chr::days(365)) {
        return false;
    }
    auto ymd1 = chr::year_month_day(d1);
    if (ymd1.month() == chr::month(2) && ymd1.day() == chr::day(29)) {
        ymd1 = chr::year_month_day(d1 + chr::days(1));
    }
    const auto ymd2 = chr::year_month_day(d2);
    return ymd1.year() < ymd2.year() && (ymd1.year() + chr::years(1)) / ymd1.month() / ymd1.day() <= ymd2;
}
