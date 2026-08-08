#include <atlib/marketdata/credentials.h>

#include <meadow/file.h>

#include <rfl/toml.hpp>

#include <cctype>
#include <cstdlib>

namespace
{

// Only the key is read. A provider table may carry anything else it likes --
// reflect-cpp ignores fields nobody asked for -- and a table with no key at all
// is not an error here, just an absent credential.
struct ProviderCredentials {
    optional<string> api_key;
};

fs::path credentials_path()
{
    const char* home = std::getenv("HOME");
    return fs::path(home ? home : "") / ".config" / "autotrade" / "credentials.toml";
}

string env_var_name(string_view provider)
{
    string name;
    name.reserve(provider.size() + 8);
    for (const char c : provider) {
        name += static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
    }
    name += "_API_KEY";
    return name;
}

} // namespace

expected<string, string> api_key(string_view provider)
{
    const string variable = env_var_name(provider);

    // An empty value counts as unset. Exporting FOO= to "clear" it is common
    // enough that treating the empty string as a credential would only ever
    // produce a puzzling 401.
    if (const char* from_environment = std::getenv(variable.c_str()); from_environment && *from_environment != '\0') {
        return string(from_environment);
    }

    const fs::path path = credentials_path();

    std::error_code ec;
    if (fs::exists(path, ec)) {
        const auto text = read_file_to_string(path);
        if (!text) {
            return unexpected(format("{}: {}", path.string(), text.error()));
        }

        // A map rather than a struct per provider: the file is a registry, and a
        // provider this build has never heard of is the file's business, not an
        // error.
        const auto parsed = rfl::toml::read<std::map<string, ProviderCredentials>>(*text);
        if (!parsed) {
            return unexpected(format("{}: {}", path.string(), parsed.error().what()));
        }

        if (const auto it = parsed->find(string(provider)); it != parsed->end()) {
            if (const auto& key = it->second.api_key; key && !key->empty()) {
                return *key;
            }
        }
    }

    // Not a bug -- a setup step nobody has done yet -- so the message is the
    // instructions rather than a diagnosis.
    return unexpected(format(
      "no API key for \"{}\".\n"
      "Set ${}, or add to {}:\n"
      "\n"
      "    [{}]\n"
      "    api_key = \"...\"\n",
      provider,
      variable,
      path.string(),
      provider
    ));
}
