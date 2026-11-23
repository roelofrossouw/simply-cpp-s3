#pragma once

#include <chrono>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace sc::s3 {
    /// An access key: its id and secret, and for temporary credentials (STS) the session token
    /// and when they expire.
    struct credentials {
        std::string access_key_id;
        std::string secret_access_key;
        std::string session_token;
        std::optional<std::chrono::system_clock::time_point> expiration;

        [[nodiscard]] bool empty() const { return access_key_id.empty() || secret_access_key.empty(); }
        /// Whether they have expired by now (plus margin), when they expire at all.
        [[nodiscard]] bool expired(std::chrono::seconds margin = std::chrono::seconds{60}) const;
    };

    /// A request to sign with AWS Signature Version 4, for S3, STS or any AWS service.
    struct signed_request {
        using fields = std::vector<std::pair<std::string, std::string>>;

        std::string method = "GET";
        /// The path as sent, already URI-encoded ("/", "/my-bucket/a%20b.txt").
        std::string path = "/";
        /// Query parameters as they are (not encoded); a parameter without a value has "".
        fields query;
        /// The headers to sign, by any case; host must be among them. sign() adds its own.
        fields headers;
        /// The body's SHA-256 in hex, or "UNSIGNED-PAYLOAD".
        std::string payload_hash;
    };

    /// AWS Signature Version 4 (https://docs.aws.amazon.com/IAM/latest/UserGuide/reference_sigv-create-signed-request.html),
    /// step by step so each can be checked against AWS's examples, and sign() doing all of them.
    namespace sigv4 {
        /// The text RFC 3986 leaves alone (A-Z a-z 0-9 - _ . ~), everything else as %XX in capitals;
        /// '/' too unless keep_slash (an object key's path).
        [[nodiscard]] std::string uri_encode(std::string_view text, bool keep_slash = false);

        /// "20150830T123600Z" and "20150830" for a time.
        [[nodiscard]] std::string amz_date(std::chrono::system_clock::time_point when);
        [[nodiscard]] std::string short_date(std::chrono::system_clock::time_point when);

        /// The canonical request: method, path, sorted query, lowercase sorted headers with
        /// trimmed values, the signed header names and the payload hash, each on its own line.
        [[nodiscard]] std::string canonical_request(const signed_request &request);
        /// "host;x-amz-date": the lowercase names of the request's headers, sorted.
        [[nodiscard]] std::string signed_headers(const signed_request &request);
        /// "AWS4-HMAC-SHA256", the time, the scope and the canonical request's SHA-256.
        [[nodiscard]] std::string string_to_sign(std::string_view amz_date, std::string_view scope, std::string_view canonical_request);
        /// "20150830/us-east-1/s3/aws4_request".
        [[nodiscard]] std::string scope(std::string_view short_date, std::string_view region, std::string_view service);
        /// The signing key derived from the secret for that day, region and service (32 bytes).
        [[nodiscard]] std::string signing_key(std::string_view secret, std::string_view short_date, std::string_view region, std::string_view service);

        /// Signs request in place: adds x-amz-date, x-amz-security-token (when the credentials
        /// have one) and, for S3, x-amz-content-sha256, then Authorization over all its headers.
        /// Returns the Authorization header's value.
        std::string sign(signed_request &request, const credentials &key, std::string_view region, std::string_view service,
                         std::chrono::system_clock::time_point when = std::chrono::system_clock::now());
    }
}
