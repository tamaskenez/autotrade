#pragma once

#include <meadow/cppext.h>

inline double years_between_days(chr::local_days d1, chr::local_days d2)
{
    return chr::duration<double>(d2 - d1) / chr::duration<double>(chr::years(1));
}
