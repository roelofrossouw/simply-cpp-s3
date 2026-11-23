#include <sc/s3/sts.h>

#include <sc/s3/checksum.h>
#include <sc/s3/client.h>
#include <sc/s3/xml.h>

#include "http.h"

#include <stdexcept>
#include <thread>

namespace sc::s3 {
    credentials parse_assume_role(const std::string_view document) {
        const auto root = xml::parse(document);
        const auto *found = root.find("AssumeRoleResult/Credentials");
        if (!found) throw std::runtime_error("STS's answer holds no credentials");
        credentials key;
        key.access_key_id = found->value("AccessKeyId");
        key.secret_access_key = found->value("SecretAccessKey");
        key.session_token = found->value("SessionToken");
        if (const auto expiration = found->value("Expiration"); !expiration.empty()) key.expiration = parse_time(expiration);
        if (key.empty() || key.session_token.empty()) throw std::runtime_error("STS's answer holds incomplete credentials");
        return key;
    }

    credentials assume_role(const settings &options, const assume_role_options &request) {
        if (request.duration < std::chrono::minutes{15}) throw std::invalid_argument("assume_role: at least 15 minutes");
        signed_request::fields form{{"Action", "AssumeRole"},
                                    {"Version", "2011-06-15"},
                                    {"RoleArn", request.role_arn},
                                    {"RoleSessionName", request.session_name},
                                    {"DurationSeconds", std::to_string(request.duration.count())}};
        if (!request.policy.empty()) form.emplace_back("Policy", request.policy);
        if (!request.external_id.empty()) form.emplace_back("ExternalId", request.external_id);
        const std::string body = detail::query_string(form);

        const auto base = detail::split_url(options.sts_url());
        detail::http connection;
        const int attempts = std::max(1, options.retries + 1);
        for (int attempt = 1;; ++attempt) {
            signed_request to_sign;
            to_sign.method = "POST";
            to_sign.path = base.path.empty() ? "/" : base.path + '/';
            to_sign.headers = {{"host", base.host}, {"Content-Type", "application/x-www-form-urlencoded; charset=utf-8"}};
            to_sign.payload_hash = hex(sha256(body));
            sigv4::sign(to_sign, options.credentials, options.region, "sts");

            detail::http_request http;
            http.method = "POST";
            http.url = base.scheme + "://" + base.host + to_sign.path;
            for (const auto &header: to_sign.headers) {
                if (header.first != "host") http.headers.push_back(header);
            }
            http.body = body;
            http.send_body = true;
            const auto response = connection.perform(options, http);
            if (response.status >= 200 && response.status < 300) return parse_assume_role(response.body);
            const auto refused = response.status == 0 ? detail::network_error(response, "STS AssumeRole") : detail::error_from(response);
            if (!refused.retriable() || attempt >= attempts) throw refused;
            std::this_thread::sleep_for(std::chrono::seconds{std::min(30, 1 << std::min(attempt - 1, 5))});
        }
    }
}
