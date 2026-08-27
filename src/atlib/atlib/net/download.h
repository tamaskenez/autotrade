#pragma once

#include <meadow/cppext.h>

// HTTP GET into memory.
//
// Nothing here writes to disk. Payloads are a couple of megabytes, the caller
// wants the bytes in order to parse them anyway, and whether a copy is worth
// caching -- and under what name, with what atomicity -- is a question only the
// caller can answer. A download-to-file variant would have to invent answers.

struct HttpRequest {
    string url;

    // Appended to `url` as a query string, each key and value percent-encoded.
    // Passing them here rather than pre-formatting the URL keeps escaping out of
    // every call site.
    vector<pair<string, string>> query;

    // Sent verbatim as "Name: value". Credentials belong here rather than in
    // `query`: a token in the URL ends up in redirect targets, proxy logs and
    // any error message that quotes the request.
    vector<pair<string, string>> headers;

    chr::seconds timeout{5};
};

struct HttpResponse {
    long status = 0;
    string body;

    bool ok() const
    {
        return 200 <= status && status < 300;
    }
};

// Performs the request. The error case is a failure to complete the exchange --
// DNS, TLS, timeout -- not an unhappy status code: a 404 with a body explaining
// itself is a successful round trip and comes back as a value. Callers that
// treat any non-2xx as fatal check `ok()` themselves, and can quote `body`,
// which is where APIs put the reason.
expected<HttpResponse, string> http_get(const HttpRequest& request);
