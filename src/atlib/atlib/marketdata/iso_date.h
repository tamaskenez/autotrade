#pragma once

#include <meadow/cppext.h>

// "1993-01-29" -> 1993-01-29, for the vendor parsers.
//
// Anything past the tenth character is ignored, which is what Tiingo's
// "1993-01-29T00:00:00.000Z" needs: the time is always midnight UTC, which is not
// when anything traded, so it is discarded rather than interpreted.
//
// Rejects rather than repairs, and rejects here rather than downstream. A date is
// the one field where a wrong-but-plausible value cannot be spotted later -- it
// would just quietly place a row on the wrong day -- so a 13th month or a 30th of
// February fails at the boundary, which is the only place it is recognisable as
// bad. Returning chr::local_days rather than chr::year_month_day is what makes
// that unavoidable; see equity.h.
//
// Shared by the providers rather than written once per parser: every feed spells
// dates this way, and a second copy of this validation is a second chance to get
// it subtly wrong.
expected<chr::local_days, string> parse_iso_date(string_view text);
