#include "http.h"

#include <sc/s3/xml.h>

#include <curl/curl.h>

#include <algorithm>
#include <cctype>
#include <cstring>
#include <mutex>
#include <stdexcept>

namespace sc::s3::detail {
    namespace {
        struct exchange {
            CURL *handle = nullptr;
            const http_request *request = nullptr;
            http_response *response = nullptr;
            std::size_t sent = 0;
        };

        std::size_t on_read(char *buffer, const std::size_t size, const std::size_t count, void *data) {
            auto &x = *static_cast<exchange *>(data);
            const auto n = std::min(x.request->body.size() - x.sent, size * count);
            std::memcpy(buffer, x.request->body.data() + x.sent, n);
            x.sent += n;
            return n;
        }

        std::size_t on_write(char *buffer, const std::size_t size, const std::size_t count, void *data) {
            auto &x = *static_cast<exchange *>(data);
            const auto n = size * count;
            long status = 0;
            curl_easy_getinfo(x.handle, CURLINFO_RESPONSE_CODE, &status);
            if (x.request->sink && status >= 200 && status < 300) {
                if (!(*x.request->sink)(std::string_view{buffer, n})) return 0; // CURLE_WRITE_ERROR
                x.response->delivered += n;
            } else {
                x.response->body.append(buffer, n);
            }
            return n;
        }

        std::size_t on_header(char *buffer, const std::size_t size, const std::size_t count, void *data) {
            auto &x = *static_cast<exchange *>(data);
            const std::string_view line{buffer, size * count};
            if (line.starts_with("HTTP/")) {
                x.response->headers.clear(); // a new answer (after a 100 Continue)
                return line.size();
            }
            const auto colon = line.find(':');
            if (colon == std::string_view::npos) return line.size();
            std::string name{line.substr(0, colon)};
            std::ranges::transform(name, name.begin(), [](const unsigned char c) { return static_cast<char>(std::tolower(c)); });
            auto value = line.substr(colon + 1);
            while (!value.empty() && (value.front() == ' ' || value.front() == '\t')) value.remove_prefix(1);
            while (!value.empty() && (value.back() == '\r' || value.back() == '\n' || value.back() == ' ')) value.remove_suffix(1);
            x.response->headers[name] = std::string{value};
            return line.size();
        }
    }

    http::http() {
        static std::once_flag once;
        std::call_once(once, [] { curl_global_init(CURL_GLOBAL_DEFAULT); });
        handle_ = curl_easy_init();
        if (!handle_) throw std::runtime_error("curl_easy_init failed");
    }

    http::~http() { curl_easy_cleanup(static_cast<CURL *>(handle_)); }

    http_response http::perform(const settings &options, const http_request &request) {
        auto *handle = static_cast<CURL *>(handle_);
        curl_easy_reset(handle); // the options go; the connection stays
        http_response response;
        exchange x{handle, &request, &response};

        curl_slist *headers = nullptr;
        for (const auto &[name, value]: request.headers) headers = curl_slist_append(headers, (name + ": " + value).c_str());
        headers = curl_slist_append(headers, "Expect:"); // no 100-continue round trip

        curl_easy_setopt(handle, CURLOPT_URL, request.url.c_str());
        curl_easy_setopt(handle, CURLOPT_HTTPHEADER, headers);
        curl_easy_setopt(handle, CURLOPT_NOSIGNAL, 1L);
        curl_easy_setopt(handle, CURLOPT_CONNECTTIMEOUT, static_cast<long>(options.connect_timeout.count()));
        curl_easy_setopt(handle, CURLOPT_LOW_SPEED_LIMIT, 1L);
        curl_easy_setopt(handle, CURLOPT_LOW_SPEED_TIME, static_cast<long>(options.idle_timeout.count()));
        curl_easy_setopt(handle, CURLOPT_HEADERFUNCTION, on_header);
        curl_easy_setopt(handle, CURLOPT_HEADERDATA, &x);
        curl_easy_setopt(handle, CURLOPT_WRITEFUNCTION, on_write);
        curl_easy_setopt(handle, CURLOPT_WRITEDATA, &x);
        if (!options.ca_file.empty()) curl_easy_setopt(handle, CURLOPT_CAINFO, options.ca_file.c_str());
        if (!options.verify_tls) {
            curl_easy_setopt(handle, CURLOPT_SSL_VERIFYPEER, 0L);
            curl_easy_setopt(handle, CURLOPT_SSL_VERIFYHOST, 0L);
        }
        if (options.rate_limit.count() > 0) {
            curl_easy_setopt(handle, CURLOPT_MAX_SEND_SPEED_LARGE, static_cast<curl_off_t>(options.rate_limit.count()));
            curl_easy_setopt(handle, CURLOPT_MAX_RECV_SPEED_LARGE, static_cast<curl_off_t>(options.rate_limit.count()));
        }

        if (request.method == "HEAD") {
            curl_easy_setopt(handle, CURLOPT_NOBODY, 1L);
        } else if (request.send_body || !request.body.empty()) {
            curl_easy_setopt(handle, CURLOPT_UPLOAD, 1L);
            curl_easy_setopt(handle, CURLOPT_READFUNCTION, on_read);
            curl_easy_setopt(handle, CURLOPT_READDATA, &x);
            curl_easy_setopt(handle, CURLOPT_INFILESIZE_LARGE, static_cast<curl_off_t>(request.body.size()));
            if (request.method != "PUT") curl_easy_setopt(handle, CURLOPT_CUSTOMREQUEST, request.method.c_str());
        } else if (request.method != "GET") {
            curl_easy_setopt(handle, CURLOPT_CUSTOMREQUEST, request.method.c_str());
        }

        const CURLcode result = curl_easy_perform(handle);
        curl_easy_getinfo(handle, CURLINFO_RESPONSE_CODE, &response.status);
        curl_slist_free_all(headers);
        if (result != CURLE_OK) {
            response.failure = curl_easy_strerror(result);
            response.timed_out = result == CURLE_OPERATION_TIMEDOUT;
            if (result == CURLE_WRITE_ERROR && request.sink) response.failure = "couldn't write what came";
            response.status = 0; // a half answer counts as none
        }
        return response;
    }

    error error_from(const http_response &response) {
        std::string code, message, request_id;
        if (!response.body.empty()) {
            try {
                const auto root = xml::parse(response.body);
                code = root.value("Code");
                message = root.value("Message");
                request_id = root.value("RequestId");
                if (const auto *inner = root.child("Error")) { // STS: <ErrorResponse><Error>...
                    code = inner->value("Code", code);
                    message = inner->value("Message", message);
                }
            } catch (const std::exception &) {
                message = response.body.substr(0, 300);
            }
        }
        if (request_id.empty()) {
            if (const auto it = response.headers.find("x-amz-request-id"); it != response.headers.end()) request_id = it->second;
        }
        if (code.empty()) {
            switch (response.status) {
                case 301: code = "PermanentRedirect"; break;
                case 400: code = "BadRequest"; break;
                case 403: code = "AccessDenied"; break;
                case 404: code = "NotFound"; break;
                case 409: code = "Conflict"; break;
                case 503: code = "ServiceUnavailable"; break;
                default: code = "HTTP" + std::to_string(response.status);
            }
        }
        std::string text = code;
        if (!message.empty()) text += ": " + message;
        text += " (HTTP " + std::to_string(response.status) + ")";
        if (response.status == 301) {
            if (const auto it = response.headers.find("x-amz-bucket-region"); it != response.headers.end()) {
                text += "; the bucket is in " + it->second;
            }
        }
        return {response.status, code, text, request_id};
    }

    error network_error(const http_response &response, const std::string_view what) {
        return {0, response.timed_out ? "Timeout" : "Network", std::string{what} + ": " + response.failure};
    }

    std::string url_decode(const std::string_view text) {
        std::string out;
        out.reserve(text.size());
        for (std::size_t i = 0; i < text.size(); ++i) {
            if (text[i] == '%' && i + 2 < text.size() && std::isxdigit(static_cast<unsigned char>(text[i + 1])) &&
                std::isxdigit(static_cast<unsigned char>(text[i + 2]))) {
                out += static_cast<char>(std::stoi(std::string{text.substr(i + 1, 2)}, nullptr, 16));
                i += 2;
            } else if (text[i] == '+') {
                out += ' ';
            } else {
                out += text[i];
            }
        }
        return out;
    }

    base_url split_url(const std::string_view url) {
        base_url out;
        const auto scheme_end = url.find("://");
        if (scheme_end == std::string_view::npos) throw std::invalid_argument("not a URL: " + std::string{url});
        out.scheme = std::string{url.substr(0, scheme_end)};
        std::ranges::transform(out.scheme, out.scheme.begin(), [](const unsigned char c) { return static_cast<char>(std::tolower(c)); });
        if (out.scheme != "http" && out.scheme != "https") throw std::invalid_argument("not an http(s) URL: " + std::string{url});
        auto rest = url.substr(scheme_end + 3);
        const auto slash = rest.find('/');
        out.host = std::string{rest.substr(0, slash)};
        if (slash != std::string_view::npos) {
            out.path = std::string{rest.substr(slash)};
            while (!out.path.empty() && out.path.back() == '/') out.path.pop_back();
        }
        if (out.host.empty()) throw std::invalid_argument("no host in " + std::string{url});
        const std::string default_port = out.scheme == "https" ? ":443" : ":80";
        if (out.host.ends_with(default_port)) out.host.resize(out.host.size() - default_port.size());
        return out;
    }

    std::string query_string(const signed_request::fields &query) {
        std::string out;
        for (const auto &[name, value]: query) {
            out += (out.empty() ? "" : "&") + sigv4::uri_encode(name) + '=' + sigv4::uri_encode(value);
        }
        return out;
    }
}
