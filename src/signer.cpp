#include <sc/s3/signer.h>

#include <sc/s3/checksum.h>

#include <algorithm>
#include <cctype>
#include <ctime>
#include <map>
#include <stdexcept>

namespace sc::s3 {
    bool credentials::expired(const std::chrono::seconds margin) const {
        return expiration && std::chrono::system_clock::now() + margin >= *expiration;
    }

    namespace sigv4 {
        namespace {
            std::string lower(std::string_view text) {
                std::string out{text};
                std::ranges::transform(out, out.begin(), [](const unsigned char c) { return static_cast<char>(std::tolower(c)); });
                return out;
            }

            // Trimmed, with runs of spaces inside as one, as SigV4 wants header values.
            std::string trimmed(const std::string_view value) {
                std::string out;
                bool space = false;
                for (const char c: value) {
                    if (c == ' ' || c == '\t') {
                        space = !out.empty();
                        continue;
                    }
                    if (space) out += ' ';
                    space = false;
                    out += c;
                }
                return out;
            }

            // The headers by lowercase name, sorted; a header given twice gets its values joined by ','.
            std::map<std::string, std::string> canonical_headers(const signed_request &request) {
                std::map<std::string, std::string> headers;
                for (const auto &[name, value]: request.headers) {
                    if (lower(name) == "authorization") continue; // never signed: it's the signature
                    auto &slot = headers[lower(name)];
                    slot += (slot.empty() ? "" : ",") + trimmed(value);
                }
                return headers;
            }

            std::string format(const std::chrono::system_clock::time_point when, const char *pattern) {
                const std::time_t seconds = std::chrono::system_clock::to_time_t(when);
                std::tm utc{};
                gmtime_r(&seconds, &utc);
                char text[32];
                std::strftime(text, sizeof text, pattern, &utc);
                return text;
            }

            bool has(const signed_request &request, const std::string_view name) {
                return std::ranges::any_of(request.headers, [&](const auto &header) { return lower(header.first) == name; });
            }
        }

        std::string uri_encode(const std::string_view text, const bool keep_slash) {
            static constexpr char digits[] = "0123456789ABCDEF";
            std::string out;
            out.reserve(text.size() * 3);
            for (const char c: text) {
                const auto u = static_cast<unsigned char>(c);
                if (std::isalnum(u) || c == '-' || c == '_' || c == '.' || c == '~' || (keep_slash && c == '/')) {
                    out += c;
                } else {
                    out += '%';
                    out += digits[u >> 4];
                    out += digits[u & 0xF];
                }
            }
            return out;
        }

        std::string amz_date(const std::chrono::system_clock::time_point when) { return format(when, "%Y%m%dT%H%M%SZ"); }
        std::string short_date(const std::chrono::system_clock::time_point when) { return format(when, "%Y%m%d"); }

        std::string signed_headers(const signed_request &request) {
            std::string names;
            for (const auto &[name, value]: canonical_headers(request)) names += (names.empty() ? "" : ";") + name;
            return names;
        }

        std::string canonical_request(const signed_request &request) {
            std::vector<std::pair<std::string, std::string>> query;
            query.reserve(request.query.size());
            for (const auto &[name, value]: request.query) query.emplace_back(uri_encode(name), uri_encode(value));
            std::ranges::sort(query);

            std::string text = request.method + '\n' + (request.path.empty() ? "/" : request.path) + '\n';
            for (std::size_t i = 0; i < query.size(); ++i) text += (i ? "&" : "") + query[i].first + '=' + query[i].second;
            text += '\n';
            for (const auto &[name, value]: canonical_headers(request)) text += name + ':' + value + '\n';
            text += '\n' + signed_headers(request) + '\n';
            text += request.payload_hash.empty() ? hex(sha256("")) : request.payload_hash;
            return text;
        }

        std::string scope(const std::string_view short_date, const std::string_view region, const std::string_view service) {
            return std::string{short_date} + '/' + std::string{region} + '/' + std::string{service} + "/aws4_request";
        }

        std::string string_to_sign(const std::string_view amz_date, const std::string_view scope, const std::string_view canonical_request) {
            return "AWS4-HMAC-SHA256\n" + std::string{amz_date} + '\n' + std::string{scope} + '\n' + hex(sha256(canonical_request));
        }

        std::string signing_key(const std::string_view secret, const std::string_view short_date, const std::string_view region,
                                const std::string_view service) {
            auto key = hmac_sha256("AWS4" + std::string{secret}, short_date);
            key = hmac_sha256(key, region);
            key = hmac_sha256(key, service);
            return hmac_sha256(key, "aws4_request");
        }

        std::string sign(signed_request &request, const credentials &key, const std::string_view region, const std::string_view service,
                         const std::chrono::system_clock::time_point when) {
            if (key.empty()) throw std::invalid_argument("no credentials to sign with (an access key id and its secret)");
            if (!has(request, "host")) throw std::invalid_argument("a request to sign needs its host header");
            if (request.payload_hash.empty()) request.payload_hash = hex(sha256(""));
            std::erase_if(request.headers, [](const auto &header) {
                const auto name = lower(header.first);
                return name == "authorization" || name == "x-amz-date" || name == "x-amz-security-token" || name == "x-amz-content-sha256";
            });
            const auto date = amz_date(when);
            request.headers.emplace_back("x-amz-date", date);
            if (!key.session_token.empty()) request.headers.emplace_back("x-amz-security-token", key.session_token);
            if (service == "s3") request.headers.emplace_back("x-amz-content-sha256", request.payload_hash);

            const auto day = short_date(when);
            const auto request_scope = scope(day, region, service);
            const auto to_sign = string_to_sign(date, request_scope, canonical_request(request));
            const auto signature = hex(hmac_sha256(signing_key(key.secret_access_key, day, region, service), to_sign));
            auto authorization = "AWS4-HMAC-SHA256 Credential=" + key.access_key_id + '/' + request_scope + ", SignedHeaders=" +
                                 signed_headers(request) + ", Signature=" + signature;
            request.headers.emplace_back("Authorization", authorization);
            return authorization;
        }
    }
}
