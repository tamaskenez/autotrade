#include <atlib/net/download.h>

#include <curl/curl.h>

namespace
{

// libcurl needs a process-wide initialisation before any handle exists, and a
// matching teardown. A function-local static gets both, in the right order,
// without a mutex of our own -- and the failure code has to be kept, because
// curl_global_init is the one call whose result nothing else will report.
struct CurlGlobal {
    CurlGlobal()
        : code(curl_global_init(CURL_GLOBAL_DEFAULT))
    {
    }

    ~CurlGlobal()
    {
        if (code == CURLE_OK) {
            curl_global_cleanup();
        }
    }

    CurlGlobal(const CurlGlobal&) = delete;
    CurlGlobal& operator=(const CurlGlobal&) = delete;

    CURLcode code;
};

CURLcode init_curl_globally_once()
{
    static const CurlGlobal instance;
    return instance.code;
}

size_t append_to_string(char* data, size_t size, size_t nmemb, void* userdata)
{
    const size_t count = size * nmemb;
    // This is called from C. An exception escaping into libcurl's stack frames
    // is undefined behaviour, so a failed allocation has to be turned into the
    // short count that means "abort the transfer" -- which surfaces as
    // CURLE_WRITE_ERROR from curl_easy_perform.
    try {
        static_cast<string*>(userdata)->append(data, count);
    } catch (...) {
        return 0;
    }
    return count;
}

expected<string, string> percent_encode(CURL* curl, string_view text)
{
    // The length is passed explicitly: a string_view need not be terminated.
    char* encoded = curl_easy_escape(curl, text.data(), static_cast<int>(text.size()));
    if (!encoded) {
        return unexpected(format("cannot percent-encode \"{}\"", text));
    }
    const absl::Cleanup free_encoded = [encoded] {
        curl_free(encoded);
    };
    return string(encoded);
}

expected<string, string> build_url(CURL* curl, const HttpRequest& request)
{
    string url = request.url;

    // A caller that already put a query in the URL gets the pairs appended to
    // it rather than a second '?'.
    bool has_query = url.find('?') != string::npos;

    for (const auto& [key, value] : request.query) {
        const auto encoded_key = percent_encode(curl, key);
        if (!encoded_key) {
            return unexpected(encoded_key.error());
        }
        const auto encoded_value = percent_encode(curl, value);
        if (!encoded_value) {
            return unexpected(encoded_value.error());
        }
        url += std::exchange(has_query, true) ? '&' : '?';
        url += format("{}={}", *encoded_key, *encoded_value);
    }

    return url;
}

} // namespace

expected<HttpResponse, string> http_get(const HttpRequest& request)
{
    if (const CURLcode code = init_curl_globally_once(); code != CURLE_OK) {
        return unexpected(format("curl_global_init failed: {}", curl_easy_strerror(code)));
    }

    CURL* curl = curl_easy_init();
    if (!curl) {
        return unexpected(string("curl_easy_init failed"));
    }
    const absl::Cleanup cleanup_curl = [curl] {
        curl_easy_cleanup(curl);
    };

    const auto url = build_url(curl, request);
    if (!url) {
        return unexpected(url.error());
    }

    curl_slist* header_list = nullptr;
    const absl::Cleanup cleanup_header_list = [&header_list] {
        curl_slist_free_all(header_list);
    };
    for (const auto& [name, value] : request.headers) {
        curl_slist* appended = curl_slist_append(header_list, format("{}: {}", name, value).c_str());
        if (!appended) {
            return unexpected(format("cannot add header \"{}\"", name));
        }
        header_list = appended;
    }

    HttpResponse response;
    array<char, CURL_ERROR_SIZE> error_buffer{};

    curl_easy_setopt(curl, CURLOPT_URL, url->c_str());
    curl_easy_setopt(curl, CURLOPT_HTTPHEADER, header_list);
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, append_to_string);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, &response.body);
    curl_easy_setopt(curl, CURLOPT_TIMEOUT, static_cast<long>(request.timeout.count()));

    // curl_easy_strerror only names the category of failure; the buffer is where
    // libcurl writes which host, which certificate, which timeout.
    curl_easy_setopt(curl, CURLOPT_ERRORBUFFER, error_buffer.data());

    // libcurl decompresses transparently, so this costs a header and saves most
    // of the bytes on JSON payloads.
    curl_easy_setopt(curl, CURLOPT_ACCEPT_ENCODING, "");

    // Without this libcurl installs a SIGALRM handler for DNS timeouts, which is
    // not something a library should do to the process around it.
    curl_easy_setopt(curl, CURLOPT_NOSIGNAL, 1L);

    // Redirects are deliberately not followed. libcurl replays custom headers to
    // the redirect target, including to another host, and custom headers are
    // exactly where callers were told to put their credentials. A 3xx comes back
    // as a response for the caller to look at instead.

    if (const CURLcode code = curl_easy_perform(curl); code != CURLE_OK) {
        const string_view detail = error_buffer.data();
        // The bare URL, not the one with the query appended: this string ends up
        // in logs, and query values are the caller's, not ours to publish.
        return unexpected(
          format("GET {}: {}", request.url, detail.empty() ? string_view(curl_easy_strerror(code)) : detail)
        );
    }

    curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &response.status);

    return response;
}
