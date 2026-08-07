#pragma once

#include <meadow/cppext.h>

// API keys, looked up in the environment first and then in a TOML file outside
// the repository:
//
//     ~/.config/autotrade/credentials.toml
//
//         [tiingo]
//         api_key = "..."
//
// Same two sources, same file, same precedence as src/marketdata/config.py, so
// the C++ and Python tools cannot end up authenticating as different accounts.
// The file lives outside the working tree so secrets cannot be committed by
// accident -- true by construction rather than by remembering to gitignore
// something. The environment override keeps scripted use easy.

// Returns the key for `provider`, from $<PROVIDER>_API_KEY or the file above.
//
// A missing key is a configuration mistake, not a bug, so the error text is the
// fix: which variable to set, which file to write, what to put in it.
expected<string, string> api_key(string_view provider);
