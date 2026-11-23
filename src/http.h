#pragma once
// Internal: one HTTP exchange on libcurl, signed by the caller, and S3's error answers.
#include <sc/s3/client.h>
#include <sc/s3/settings.h>
#include <sc/s3/signer.h>

#include <cstdint>
#include <functional>
#include <map>
#include <string>
#include <string_view>

namespace sc::s3::detail {
    struct http_response {
        long status = 0; // 0: no answer (see failure)
        std::map<std::string, std::string> headers; // lowercase names
        std::string body;                           // all of it, but a 2xx body that went to the sink
        std::uint64_t delivered = 0;                // bytes handed to the sink
        std::string failure;                        // why there was no (whole) answer
        bool timed_out = false;
    };

    struct http_request {
        std::string method = "GET";
        std::string url;
        signed_request::fields headers; // as signed, Authorization among them
        std::string_view body;
        bool send_body = false; // a body (perhaps empty, Content-Length: 0) goes with it
        // A 2xx answer's body goes here, piece by piece; returning false stops the transfer.
        const std::function<bool(std::string_view)> *sink = nullptr;
    };

    // One curl handle, kept between requests so its connection stays open.
    class http {
    public:
        http();
        ~http();
        http(const http &) = delete;
        http &operator=(const http &) = delete;

        // Never throws for the network: a failure comes back as status 0 with failure set.
        http_response perform(const settings &options, const http_request &request);

    private:
        void *handle_;
    };

    // An error answer (S3's or STS's XML, or only the status for HEAD) as sc::s3::error.
    error error_from(const http_response &response);
    // No answer at all as sc::s3::error (status 0, code Network or Timeout).
    error network_error(const http_response &response, std::string_view what);

    // %XX and (in a query) '+' decoded.
    std::string url_decode(std::string_view text);

    // "https", "s3.af-south-1.amazonaws.com" or "127.0.0.1:9000" (without the scheme's default port),
    // and a base path ("" or "/minio") from a base URL.
    struct base_url {
        std::string scheme;
        std::string host;
        std::string path;
    };
    base_url split_url(std::string_view url);

    // name=value&... with both encoded as SigV4 wants.
    std::string query_string(const signed_request::fields &query);
}
