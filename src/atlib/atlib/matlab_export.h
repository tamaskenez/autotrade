#pragma once

#include <meadow/cppext.h>

namespace matlab_export
{
// Print 3 variable assignments into the result string.
// Values for the date axis (datenums):
//     $var = [ ... ]';
// Datenums of ticks (subset of $var):
//     $var_ticks = [ ... ]';
// Corresponding labels:
//     $var_labels = [ '...'; '...', ... ];
NODIS string assign_date_axis(string_view var, span<const chr::local_days> local_days);

// Print "$var = [ ... ]';"
NODIS string assign_row_vector(string_view var, span<const double> values);

NODIS string assign_matrix(string_view var, const vector<vector<double>>& values);

// Print setting the datenum related tick variables of a datenum axis:
//     set(gca, 'xtick', ${var}_ticks);
//     set(gca, 'xticklabel', ${var}_labels);
NODIS string set_ticks_labels(string_view var);
} // namespace matlab_export
