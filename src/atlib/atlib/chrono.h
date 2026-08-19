#pragma once

#include <meadow/cppext.h>

NODIS double years_between_days(chr::local_days d1, chr::local_days d2);
NODIS bool is_on_or_later_than_next_year_same_date(chr::local_days d1, chr::local_days d2);
