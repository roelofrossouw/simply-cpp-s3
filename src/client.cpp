#include <sc/s3/client.h>

#include <sc/s3/xml.h>

#include <sc/core/base64.h>

#include "http.h"

#include <algorithm>
#include <array>
#include <cctype>
#include <ctime>
#include <fstream>
#include <future>
#include <thread>

namespace sc::s3 {
    // ---- names ----

    namespace {
        std::string upper(std::string_view text) {
            std::string out{text};
            std::ranges::transform(out, out.begin(), [](const unsigned char c) { return static_cast<char>(std::toupper(c)); });
            return out;
        }

        std::string lower(std::string_view text) {
            std::string out{text};
            std::ranges::transform(out, out.begin(), [](const unsigned char c) { return static_cast<char>(std::tolower(c)); });
            return out;
        }

        constexpr std::array<std::pair<storage_class, std::string_view>, 8> storage_names{{
            {storage_class::standard, "STANDARD"},
            {storage_class::reduced_redundancy, "REDUCED_REDUNDANCY"},
            {storage_class::standard_ia, "STANDARD_IA"},
            {storage_class::onezone_ia, "ONEZONE_IA"},
            {storage_class::intelligent_tiering, "INTELLIGENT_TIERING"},
            {storage_class::glacier_ir, "GLACIER_IR"},
            {storage_class::glacier, "GLACIER"},
            {storage_class::deep_archive, "DEEP_ARCHIVE"},
        }};
    }

    std::string_view to_string(const storage_class kind) {
        for (const auto &[value, name]: storage_names) {
            if (value == kind) return name;
        }
        return "STANDARD";
    }

    storage_class storage_class_from(const std::string_view name) {
        const auto wanted = upper(name);
        for (const auto &[value, known]: storage_names) {
            if (known == wanted) return value;
        }
        throw std::invalid_argument("not a storage class: " + std::string{name} +
                                    " (STANDARD, STANDARD_IA, ONEZONE_IA, INTELLIGENT_TIERING, GLACIER_IR, GLACIER, DEEP_ARCHIVE)");
    }

    bool archived(const storage_class kind) { return kind == storage_class::glacier || kind == storage_class::deep_archive; }

    std::string_view to_string(const restore_tier tier) {
        switch (tier) {
            case restore_tier::bulk: return "Bulk";
            case restore_tier::standard: return "Standard";
            case restore_tier::expedited: return "Expedited";
        }
        return "Bulk";
    }

    restore_tier restore_tier_from(const std::string_view name) {
        const auto wanted = lower(name);
        if (wanted == "bulk") return restore_tier::bulk;
        if (wanted == "standard") return restore_tier::standard;
        if (wanted == "expedited") return restore_tier::expedited;
        throw std::invalid_argument("not a restore tier: " + std::string{name} + " (bulk, standard or expedited)");
    }

    // ---- error ----

    error::error(const long status, std::string code, const std::string &message, std::string request_id)
        : std::runtime_error(message), status_(status), code_(std::move(code)), request_id_(std::move(request_id)) {}

    bool error::retriable() const {
        if (status_ == 0 || status_ >= 500 || status_ == 429) return true;
        return code_ == "SlowDown" || code_ == "RequestTimeout" || code_ == "InternalError" || code_ == "RequestTimeTooSkewed";
    }

    // ---- times, restore status, URLs ----

    std::chrono::system_clock::time_point parse_time(const std::string_view text) {
        std::tm parts{};
        const std::string value{text};
        static constexpr std::array<std::string_view, 12> months{"Jan", "Feb", "Mar", "Apr", "May", "Jun", "Jul", "Aug", "Sep", "Oct", "Nov", "Dec"};
        if (value.size() >= 19 && value[4] == '-' && value[10] == 'T') { // 2012-12-21T00:00:00.000Z
            if (std::sscanf(value.c_str(), "%4d-%2d-%2dT%2d:%2d:%2d", &parts.tm_year, &parts.tm_mon, &parts.tm_mday, &parts.tm_hour, &parts.tm_min,
                            &parts.tm_sec) != 6) {
                throw std::invalid_argument("not a time: " + value);
            }
            parts.tm_year -= 1900;
            parts.tm_mon -= 1;
        } else { // Fri, 21 Dec 2012 00:00:00 GMT
            char month[4] = {};
            const auto comma = value.find(", ");
            if (comma == std::string::npos ||
                std::sscanf(value.c_str() + comma + 2, "%2d %3s %4d %2d:%2d:%2d", &parts.tm_mday, month, &parts.tm_year, &parts.tm_hour, &parts.tm_min,
                            &parts.tm_sec) != 6) {
                throw std::invalid_argument("not a time: " + value);
            }
            const auto found = std::ranges::find(months, std::string_view{month});
            if (found == months.end()) throw std::invalid_argument("not a time: " + value);
            parts.tm_mon = static_cast<int>(found - months.begin());
            parts.tm_year -= 1900;
        }
        return std::chrono::system_clock::from_time_t(timegm(&parts));
    }

    restore_status restore_status::parse(const std::string_view header) {
        restore_status status;
        if (header.empty()) return status;
        status.requested = true;
        const auto field = [&](const std::string_view name) -> std::string {
            const auto at = header.find(name);
            if (at == std::string_view::npos) return {};
            const auto open = header.find('"', at + name.size());
            const auto close = open == std::string_view::npos ? open : header.find('"', open + 1);
            if (close == std::string_view::npos) return {};
            return std::string{header.substr(open + 1, close - open - 1)};
        };
        status.ongoing = field("ongoing-request") == "true";
        if (const auto expiry = field("expiry-date"); !expiry.empty()) {
            try {
                status.expiry = parse_time(expiry);
            } catch (const std::invalid_argument &) {
            }
        }
        return status;
    }

    std::pair<std::string, std::string> parse_url(const std::string_view url) {
        if (!url.starts_with("s3://")) throw std::invalid_argument("not an s3:// URL: " + std::string{url});
        const auto rest = url.substr(5);
        const auto slash = rest.find('/');
        std::string bucket{rest.substr(0, slash)};
        if (bucket.empty()) throw std::invalid_argument("no bucket in " + std::string{url});
        return {bucket, slash == std::string_view::npos ? std::string{} : std::string{rest.substr(slash + 1)}};
    }

    // ---- what S3 describes ----

    object_info object_info::from_headers(std::string key, const std::map<std::string, std::string> &headers) {
        object_info info;
        info.key = std::move(key);
        const auto get = [&](const std::string &name) -> std::string {
            const auto it = headers.find(name);
            return it == headers.end() ? std::string{} : it->second;
        };
        if (const auto range = get("content-range"); !range.empty() && range.find('/') != std::string::npos) {
            const auto total = range.substr(range.find('/') + 1);
            if (total != "*") info.size = bytes{std::stoull(total)};
        } else if (const auto length = get("content-length"); !length.empty()) {
            info.size = bytes{std::stoull(length)};
        }
        info.etag = get("etag");
        std::erase(info.etag, '"');
        if (const auto modified = get("last-modified"); !modified.empty()) {
            try {
                info.last_modified = parse_time(modified);
            } catch (const std::invalid_argument &) {
            }
        }
        if (const auto kind = get("x-amz-storage-class"); !kind.empty()) {
            try {
                info.kind = storage_class_from(kind);
            } catch (const std::invalid_argument &) {
            }
        }
        info.content_type = get("content-type");
        info.version_id = get("x-amz-version-id");
        for (const auto &[name, value]: headers) {
            if (name.starts_with("x-amz-meta-")) info.metadata[name.substr(11)] = value;
        }
        if (const auto crc = get("x-amz-checksum-crc32"); !crc.empty()) info.checksums[checksum::crc32] = crc;
        if (const auto crc = get("x-amz-checksum-crc32c"); !crc.empty()) info.checksums[checksum::crc32c] = crc;
        if (const auto sha = get("x-amz-checksum-sha256"); !sha.empty()) info.checksums[checksum::sha256] = sha;
        info.restore = restore_status::parse(get("x-amz-restore"));
        return info;
    }

    list_page list_page::parse(const std::string_view document) {
        const auto root = xml::parse(document);
        const bool encoded = lower(root.value("EncodingType")) == "url";
        const auto decoded = [&](std::string text) { return encoded ? detail::url_decode(text) : text; };
        list_page page;
        for (const auto *contents: root.all("Contents")) {
            object_entry entry;
            entry.key = decoded(contents->value("Key"));
            const auto size = contents->value("Size", "0");
            entry.size = bytes{std::stoull(size.empty() ? "0" : size)};
            entry.etag = contents->value("ETag");
            std::erase(entry.etag, '"');
            if (const auto modified = contents->value("LastModified"); !modified.empty()) {
                try {
                    entry.last_modified = parse_time(modified);
                } catch (const std::invalid_argument &) {
                }
            }
            if (const auto kind = contents->value("StorageClass"); !kind.empty()) {
                try {
                    entry.kind = storage_class_from(kind);
                } catch (const std::invalid_argument &) {
                }
            }
            page.objects.push_back(std::move(entry));
        }
        for (const auto *prefix: root.all("CommonPrefixes")) page.prefixes.push_back(decoded(prefix->value("Prefix")));
        page.truncated = lower(root.value("IsTruncated")) == "true";
        page.next_token = root.value("NextContinuationToken");
        return page;
    }

    std::vector<pending_upload> pending_upload::parse(const std::string_view document, bool *truncated, std::string *next_key,
                                                      std::string *next_upload_id) {
        const auto root = xml::parse(document);
        const bool encoded = lower(root.value("EncodingType")) == "url";
        std::vector<pending_upload> uploads;
        for (const auto *upload: root.all("Upload")) {
            pending_upload pending;
            pending.key = encoded ? detail::url_decode(upload->value("Key")) : upload->value("Key");
            pending.upload_id = upload->value("UploadId");
            if (const auto initiated = upload->value("Initiated"); !initiated.empty()) {
                try {
                    pending.initiated = parse_time(initiated);
                } catch (const std::invalid_argument &) {
                }
            }
            if (const auto kind = upload->value("StorageClass"); !kind.empty()) {
                try {
                    pending.kind = storage_class_from(kind);
                } catch (const std::invalid_argument &) {
                }
            }
            uploads.push_back(std::move(pending));
        }
        if (truncated) *truncated = lower(root.value("IsTruncated")) == "true";
        if (next_key) *next_key = root.value("NextKeyMarker");
        if (next_upload_id) *next_upload_id = root.value("NextUploadIdMarker");
        return uploads;
    }

    // ---- part sizes ----

    namespace {
        constexpr std::uint64_t smallest_part = 5ULL << 20;
        constexpr std::uint64_t biggest_part = 5ULL << 30;
        constexpr int most_parts = 10'000;
    }

    bytes part_size_for(const int part_number, const bytes base) {
        std::uint64_t size = std::max(base.count(), smallest_part);
        for (int doubled = (part_number - 1) / 1000; doubled > 0 && size < biggest_part; --doubled) size *= 2;
        return bytes{std::min(size, biggest_part)};
    }

    bytes first_part_size(const bytes requested, const bytes expected) {
        std::uint64_t size = std::max(requested.count(), smallest_part);
        if (expected.count() > 0) size = std::max(size, (expected.count() + 8999) / 9000);
        constexpr std::uint64_t mib = 1ULL << 20;
        size = (size + mib - 1) / mib * mib;
        return bytes{std::min(size, biggest_part)};
    }

    bytes largest_upload(const bytes base) {
        std::uint64_t total = 0;
        for (int tier = 0; tier < most_parts / 1000; ++tier) total += part_size_for(tier * 1000 + 1, base).count() * 1000;
        return bytes{total};
    }

    // ---- the client ----

    namespace {
        // A request to S3 before it's signed: which object, what to add.
        struct call {
            std::string method = "GET";
            std::string bucket;
            std::string key;
            signed_request::fields query;
            signed_request::fields headers;
            std::string_view body;
            bool send_body = false;
            std::string payload_hash; // empty: the body's SHA-256
            const std::function<bool(std::string_view)> *sink = nullptr;
        };

        std::string checksum_header(const checksum algorithm) { return "x-amz-checksum-" + lower(to_string(algorithm)); }

        std::string checksum_element(const checksum algorithm) {
            switch (algorithm) {
                case checksum::crc32: return "ChecksumCRC32";
                case checksum::crc32c: return "ChecksumCRC32C";
                case checksum::sha256: return "ChecksumSHA256";
                case checksum::none: break;
            }
            return {};
        }

        std::string header_of(const detail::http_response &response, const std::string &name) {
            const auto it = response.headers.find(name);
            return it == response.headers.end() ? std::string{} : it->second;
        }

        std::string unquoted(std::string etag) {
            std::erase(etag, '"');
            return etag;
        }

        // Reads up to size bytes; fewer only at the stream's end.
        std::string read_part(std::istream &in, const bytes size) {
            std::string part(size.count(), '\0');
            std::size_t got = 0;
            while (got < part.size() && in) {
                in.read(part.data() + got, static_cast<std::streamsize>(part.size() - got));
                got += static_cast<std::size_t>(in.gcount());
            }
            if (in.bad()) throw std::runtime_error("reading the data to upload failed");
            part.resize(got);
            return part;
        }

        void add_put_headers(signed_request::fields &headers, const put_options &options) {
            if (!options.content_type.empty()) headers.emplace_back("Content-Type", options.content_type);
            if (options.kind != storage_class::standard) headers.emplace_back("x-amz-storage-class", std::string{to_string(options.kind)});
            for (const auto &[name, value]: options.metadata) headers.emplace_back("x-amz-meta-" + lower(name), value);
        }
    }

    struct client::state {
        sc::s3::settings options;
        detail::base_url base;
        detail::http connection;

        explicit state(sc::s3::settings settings) : options(std::move(settings)), base(detail::split_url(options.s3_url())) {}

        // Path style for every bucket when asked for, and for buckets with a dot (TLS) or none.
        [[nodiscard]] bool in_path(const std::string &bucket) const {
            return options.path_style || bucket.empty() || bucket.find('.') != std::string::npos;
        }

        detail::http_response once(const call &request) {
            const bool path_style = in_path(request.bucket);
            const std::string host = path_style ? base.host : request.bucket + '.' + base.host;
            std::string path = base.path + '/';
            if (path_style && !request.bucket.empty()) path += request.bucket + (request.key.empty() ? "" : "/");
            path += request.key;

            signed_request to_sign;
            to_sign.method = request.method;
            to_sign.path = sigv4::uri_encode(path, true);
            to_sign.query = request.query;
            to_sign.headers = request.headers;
            to_sign.headers.emplace_back("host", host);
            to_sign.payload_hash = request.payload_hash.empty() ? hex(sha256(request.body)) : request.payload_hash;
            sigv4::sign(to_sign, options.credentials, options.region, "s3");

            detail::http_request http;
            http.method = request.method;
            http.url = base.scheme + "://" + host + to_sign.path;
            if (const auto query = detail::query_string(request.query); !query.empty()) http.url += '?' + query;
            for (auto &header: to_sign.headers) {
                if (lower(header.first) != "host") http.headers.push_back(header);
            }
            http.body = request.body;
            http.send_body = request.send_body;
            http.sink = request.sink;
            return connection.perform(options, http);
        }

        // Tries a request until it gets an answer that isn't worth trying again, or runs out of
        // tries; throws error for no answer. The answer may be an error: see check().
        detail::http_response send(const call &request, const std::string_view what) {
            const int attempts = std::max(1, options.retries + 1);
            for (int attempt = 1;; ++attempt) {
                auto response = once(request);
                const bool last = attempt >= attempts || response.delivered > 0;
                if (response.status == 0) {
                    if (last) throw detail::network_error(response, what);
                } else if (response.status >= 300) {
                    const auto refused = detail::error_from(response);
                    if (!refused.retriable() || last) return response;
                } else {
                    return response;
                }
                std::this_thread::sleep_for(std::chrono::seconds{std::min(30, 1 << std::min(attempt - 1, 5))});
            }
        }

        static void check(const detail::http_response &response) {
            if (response.status < 200 || response.status >= 300) throw detail::error_from(response);
        }
    };

    client::client(sc::s3::settings settings) : state_(std::make_unique<state>(std::move(settings))) {}
    client::~client() = default;
    client::client(client &&) noexcept = default;
    client &client::operator=(client &&) noexcept = default;

    const sc::s3::settings &client::settings() const { return state_->options; }
    void client::use(const sc::s3::credentials &key) { state_->options.credentials = key; }

    upload_result client::put(const std::string &bucket, const std::string &key, const std::string_view body, const put_options &options) {
        if (body.size() > biggest_part) throw std::invalid_argument("a single PUT holds at most 5 GiB; use upload()");
        call request{"PUT", bucket, key};
        request.body = body;
        request.send_body = true;
        add_put_headers(request.headers, options);
        if (options.checksum != checksum::none) request.headers.emplace_back(checksum_header(options.checksum), checksum_of(options.checksum, body));
        if (options.content_md5) request.headers.emplace_back("Content-MD5", sc::base64::encode(md5(body)));
        const auto response = state_->send(request, "PUT " + bucket + '/' + key);
        state::check(response);
        upload_result result;
        result.key = key;
        result.size = bytes{body.size()};
        result.etag = unquoted(header_of(response, "etag"));
        result.version_id = header_of(response, "x-amz-version-id");
        result.parts = 1;
        if (options.checksum != checksum::none) result.checksum = header_of(response, checksum_header(options.checksum));
        return result;
    }

    upload_result client::upload(const std::string &bucket, const std::string &key, std::istream &in, const upload_options &options) {
        const auto started = std::chrono::steady_clock::now();
        const bytes base = first_part_size(options.part_size, options.expected_size);
        std::string current = read_part(in, part_size_for(1, base));
        const auto confirmed = [&] {
            if (options.confirm && !options.confirm())
                throw error(0, "NotConfirmed", "the data's source said it failed: nothing was stored (a truncated stream isn't kept)");
        };
        // It fits in one part: a plain PUT.
        if (current.size() < part_size_for(1, base).count() || in.peek() == std::char_traits<char>::eof()) {
            confirmed();
            auto result = put(bucket, key, current, options);
            if (options.on_progress) options.on_progress({1, result.size, std::chrono::steady_clock::now() - started});
            return result;
        }

        call create{"POST", bucket, key, {{"uploads", ""}}};
        create.send_body = true;
        add_put_headers(create.headers, options);
        if (options.checksum != checksum::none) create.headers.emplace_back("x-amz-checksum-algorithm", std::string{to_string(options.checksum)});
        auto created = state_->send(create, "CreateMultipartUpload " + bucket + '/' + key);
        state::check(created);
        const auto upload_id = xml::parse(created.body).value("UploadId");
        if (upload_id.empty()) throw error(created.status, "NoUploadId", "S3 started a multipart upload but gave no UploadId");

        struct stored_part {
            int number;
            std::string etag, checksum;
        };
        std::vector<stored_part> parts;
        upload_result result;
        result.key = key;
        try {
            const auto stopping = [&] {
                if (options.stop && options.stop()) throw error(0, "Stopped", "the upload was stopped; its parts were deleted");
            };
            for (int number = 1;; ++number) {
                if (number > most_parts) throw std::runtime_error("more than 10,000 parts: start with a bigger part_size");
                stopping();
                // The next part is read while this one goes.
                auto next = std::async(std::launch::async, [&in, size = part_size_for(number + 1, base)] { return read_part(in, size); });

                call part{"PUT", bucket, key, {{"partNumber", std::to_string(number)}, {"uploadId", upload_id}}};
                part.body = current;
                part.send_body = true;
                const auto digest = sha256(current);
                part.payload_hash = hex(digest);
                std::string part_checksum;
                if (options.checksum == checksum::sha256) part_checksum = sc::base64::encode(digest);
                else if (options.checksum != checksum::none) part_checksum = checksum_of(options.checksum, current);
                if (!part_checksum.empty()) part.headers.emplace_back(checksum_header(options.checksum), part_checksum);
                if (options.content_md5) part.headers.emplace_back("Content-MD5", sc::base64::encode(md5(current)));
                const auto stored = state_->send(part, "UploadPart " + std::to_string(number) + " of " + bucket + '/' + key);
                if (stored.status < 200 || stored.status >= 300) {
                    next.wait();
                    throw detail::error_from(stored);
                }
                parts.push_back({number, header_of(stored, "etag"), part_checksum});
                result.size += bytes{current.size()};
                result.parts = number;
                if (options.on_progress) options.on_progress({number, result.size, std::chrono::steady_clock::now() - started});

                current = next.get();
                if (current.empty()) break;
            }

            stopping();
            confirmed();
            std::string body = "<CompleteMultipartUpload xmlns=\"http://s3.amazonaws.com/doc/2006-03-01/\">";
            for (const auto &stored: parts) {
                body += "<Part><PartNumber>" + std::to_string(stored.number) + "</PartNumber><ETag>" + xml::escape(stored.etag) + "</ETag>";
                if (!stored.checksum.empty()) {
                    const auto element = checksum_element(options.checksum);
                    body += '<' + element + '>' + stored.checksum + "</" + element + '>';
                }
                body += "</Part>";
            }
            body += "</CompleteMultipartUpload>";
            call complete{"POST", bucket, key, {{"uploadId", upload_id}}};
            complete.body = body;
            complete.send_body = true;
            complete.headers.emplace_back("Content-Type", "application/xml");
            const auto completed = state_->send(complete, "CompleteMultipartUpload " + bucket + '/' + key);
            state::check(completed);
            // S3 can answer 200 and still fail, with an Error document.
            const auto answer = xml::parse(completed.body);
            if (answer.name == "Error") throw detail::error_from({500, completed.headers, completed.body});
            result.etag = unquoted(answer.value("ETag"));
            result.version_id = header_of(completed, "x-amz-version-id");
            if (options.checksum != checksum::none) result.checksum = answer.value(checksum_element(options.checksum));
            return result;
        } catch (...) {
            try {
                abort_upload(bucket, key, upload_id);
            } catch (...) {
                // The first failure is the one to report; a lifecycle rule cleans up what's left.
            }
            throw;
        }
    }

    upload_result client::upload_file(const std::string &bucket, const std::string &key, const std::filesystem::path &file,
                                      const upload_options &options) {
        std::ifstream in{file, std::ios::binary};
        if (!in) throw std::runtime_error("can't read " + file.string());
        auto settings = options;
        if (settings.expected_size.count() == 0 && std::filesystem::is_regular_file(file)) settings.expected_size = bytes{std::filesystem::file_size(file)};
        return upload(bucket, key, in, settings);
    }

    list_page client::list(const std::string &bucket, const list_options &options) {
        call request{"GET", bucket, {}, {{"list-type", "2"}, {"encoding-type", "url"}}};
        if (!options.prefix.empty()) request.query.emplace_back("prefix", options.prefix);
        if (!options.delimiter.empty()) request.query.emplace_back("delimiter", options.delimiter);
        if (options.max_keys != 1000) request.query.emplace_back("max-keys", std::to_string(std::clamp(options.max_keys, 1, 1000)));
        if (!options.continuation_token.empty()) request.query.emplace_back("continuation-token", options.continuation_token);
        if (!options.start_after.empty() && options.continuation_token.empty()) request.query.emplace_back("start-after", options.start_after);
        const auto response = state_->send(request, "ListObjectsV2 " + bucket);
        state::check(response);
        return list_page::parse(response.body);
    }

    std::uint64_t client::list_all(const std::string &bucket, list_options options, const std::function<bool(const object_entry &)> &each,
                                   const std::function<void(const std::string &)> &prefix) {
        std::uint64_t seen = 0;
        for (;;) {
            const auto page = list(bucket, options);
            if (prefix) {
                for (const auto &name: page.prefixes) prefix(name);
            }
            for (const auto &object: page.objects) {
                ++seen;
                if (!each(object)) return seen;
            }
            if (!page.truncated || page.next_token.empty()) return seen;
            options.continuation_token = page.next_token;
        }
    }

    std::optional<object_info> client::head(const std::string &bucket, const std::string &key, const bool checksums) {
        call request{"HEAD", bucket, key};
        if (checksums) request.headers.emplace_back("x-amz-checksum-mode", "ENABLED");
        const auto response = state_->send(request, "HEAD " + bucket + '/' + key);
        if (response.status == 404) return std::nullopt;
        state::check(response);
        return object_info::from_headers(key, response.headers);
    }

    object_info client::get(const std::string &bucket, const std::string &key, std::ostream &out, const get_options &options) {
        return get(bucket, key, [&out](const std::string_view piece) {
            out.write(piece.data(), static_cast<std::streamsize>(piece.size()));
            if (!out) throw std::runtime_error("couldn't write the object out");
        }, options);
    }

    object_info client::get(const std::string &bucket, const std::string &key, const std::function<void(std::string_view)> &sink,
                            const get_options &options) {
        std::uint64_t from = options.range ? options.range->first : 0;
        const std::optional<std::uint64_t> to = options.range ? std::optional{options.range->second} : std::nullopt;
        std::optional<object_info> first;
        std::string write_failure;
        const std::function<bool(std::string_view)> write = [&](const std::string_view piece) {
            try {
                sink(piece);
                return true;
            } catch (const std::exception &failure) {
                write_failure = failure.what();
                return false;
            }
        };
        // Tries in a row that got nothing; a try that got some of it starts the count again.
        int failures = 0;
        const int allowed = std::max(0, state_->options.retries);
        for (;;) {
            call request{"GET", bucket, key};
            if (options.checksums) request.headers.emplace_back("x-amz-checksum-mode", "ENABLED");
            if (from > 0 || to) request.headers.emplace_back("Range", "bytes=" + std::to_string(from) + '-' + (to ? std::to_string(*to) : ""));
            // Going on after a break: only from the same object.
            if (first && !first->etag.empty()) request.headers.emplace_back("If-Match", '"' + first->etag + '"');
            request.sink = &write;
            const auto response = state_->once(request);
            if (!write_failure.empty()) throw std::runtime_error(write_failure);
            if (!first && response.delivered > 0) first = object_info::from_headers(key, response.headers);

            if (response.status >= 200 && response.status < 300) {
                if (!first) first = object_info::from_headers(key, response.headers);
                return *first;
            }
            if (response.status == 416 && first && from >= first->size.count()) return *first; // broke right at the end

            if (response.status == 0) {
                from += response.delivered;
                failures = response.delivered > 0 ? 0 : failures + 1;
                if (failures > allowed) throw detail::network_error(response, "GET " + bucket + '/' + key);
            } else {
                const auto refused = detail::error_from(response);
                if (!refused.retriable() || ++failures > allowed) throw refused; // 412: it changed under us
            }
            if (failures > 0) std::this_thread::sleep_for(std::chrono::seconds{std::min(30, 1 << std::min(failures - 1, 5))});
        }
    }

    object_attributes object_attributes::parse(const std::string_view document, bool *truncated, int *next_part_marker) {
        const auto root = xml::parse(document);
        object_attributes out;
        out.etag = root.value("ETag");
        std::erase(out.etag, '"');
        if (const auto size = root.value("ObjectSize"); !size.empty()) out.size = bytes{std::stoull(size)};
        if (const auto kind = root.value("StorageClass"); !kind.empty()) {
            try {
                out.kind = storage_class_from(kind);
            } catch (const std::invalid_argument &) {
            }
        }
        const auto read_checksums = [](const xml::element &from, std::map<checksum, std::string> &into) {
            for (const auto algorithm: {checksum::crc32, checksum::crc32c, checksum::sha256}) {
                auto value = from.value(checksum_element(algorithm));
                if (const auto dash = value.find('-'); dash != std::string::npos) value.resize(dash); // "...==-3"
                if (!value.empty()) into[algorithm] = value;
            }
        };
        if (const auto *sums = root.child("Checksum")) read_checksums(*sums, out.checksums);
        if (const auto *parts = root.child("ObjectParts")) {
            if (const auto count = parts->value("PartsCount"); !count.empty()) out.parts_count = std::stoi(count);
            for (const auto *each: parts->all("Part")) {
                object_attributes::part part;
                part.number = std::stoi(each->value("PartNumber", "0"));
                part.size = bytes{std::stoull(each->value("Size", "0"))};
                read_checksums(*each, part.checksums);
                out.parts.push_back(std::move(part));
            }
            if (truncated) *truncated = lower(parts->value("IsTruncated")) == "true";
            if (next_part_marker) *next_part_marker = std::stoi(parts->value("NextPartNumberMarker", "0"));
        } else if (truncated) {
            *truncated = false;
        }
        return out;
    }

    std::pair<std::uint64_t, std::uint64_t> object_attributes::part_range(const int number) const {
        std::uint64_t first = 0;
        for (const auto &each: parts) {
            if (each.number == number) {
                if (each.size.count() == 0) throw std::invalid_argument("part " + std::to_string(number) + " is empty");
                return {first, first + each.size.count() - 1};
            }
            first += each.size.count();
        }
        throw std::invalid_argument("no part " + std::to_string(number));
    }

    std::optional<object_attributes> client::attributes(const std::string &bucket, const std::string &key) {
        std::optional<object_attributes> all;
        int marker = 0;
        for (;;) {
            call request{"GET", bucket, key, {{"attributes", ""}}};
            request.headers.emplace_back("x-amz-object-attributes", "ETag,Checksum,ObjectParts,StorageClass,ObjectSize");
            request.headers.emplace_back("x-amz-max-parts", "1000");
            if (marker > 0) request.headers.emplace_back("x-amz-part-number-marker", std::to_string(marker));
            const auto response = state_->send(request, "GetObjectAttributes " + bucket + '/' + key);
            if (response.status == 404) return std::nullopt;
            state::check(response);
            bool truncated = false;
            int next = 0;
            auto page = object_attributes::parse(response.body, &truncated, &next);
            if (!all) all = std::move(page);
            else all->parts.insert(all->parts.end(), page.parts.begin(), page.parts.end());
            if (!truncated || next <= marker) return all;
            marker = next;
        }
    }

    void client::remove(const std::string &bucket, const std::string &key) {
        const auto response = state_->send({"DELETE", bucket, key}, "DELETE " + bucket + '/' + key);
        if (response.status == 404) return;
        state::check(response);
    }

    restore_result client::restore(const std::string &bucket, const std::string &key, const std::chrono::days days, const restore_tier tier) {
        if (days.count() < 1) throw std::invalid_argument("a restore lasts at least a day");
        const std::string body = "<RestoreRequest xmlns=\"http://s3.amazonaws.com/doc/2006-03-01/\"><Days>" + std::to_string(days.count()) +
                                 "</Days><GlacierJobParameters><Tier>" + std::string{to_string(tier)} + "</Tier></GlacierJobParameters></RestoreRequest>";
        call request{"POST", bucket, key, {{"restore", ""}}};
        request.body = body;
        request.send_body = true;
        request.headers.emplace_back("Content-Type", "application/xml");
        request.headers.emplace_back("Content-MD5", sc::base64::encode(md5(body)));
        const auto response = state_->send(request, "RestoreObject " + bucket + '/' + key);
        if (response.status == 202) return restore_result::started;
        if (response.status == 200) return restore_result::extended;
        if (response.status == 409) {
            const auto refused = detail::error_from(response);
            if (refused.code() == "RestoreAlreadyInProgress") return restore_result::in_progress;
            throw refused;
        }
        state::check(response);
        return restore_result::started;
    }

    std::vector<pending_upload> client::pending_uploads(const std::string &bucket, const std::string &prefix) {
        std::vector<pending_upload> all;
        std::string key_marker, upload_marker;
        for (;;) {
            call request{"GET", bucket, {}, {{"uploads", ""}, {"encoding-type", "url"}}};
            if (!prefix.empty()) request.query.emplace_back("prefix", prefix);
            if (!key_marker.empty()) request.query.emplace_back("key-marker", key_marker);
            if (!upload_marker.empty()) request.query.emplace_back("upload-id-marker", upload_marker);
            const auto response = state_->send(request, "ListMultipartUploads " + bucket);
            state::check(response);
            bool truncated = false;
            auto page = pending_upload::parse(response.body, &truncated, &key_marker, &upload_marker);
            all.insert(all.end(), std::make_move_iterator(page.begin()), std::make_move_iterator(page.end()));
            if (!truncated || (key_marker.empty() && upload_marker.empty())) return all;
        }
    }

    void client::abort_upload(const std::string &bucket, const std::string &key, const std::string &upload_id) {
        const auto response = state_->send({"DELETE", bucket, key, {{"uploadId", upload_id}}}, "AbortMultipartUpload " + bucket + '/' + key);
        if (response.status == 404) return; // gone already
        state::check(response);
    }

    void client::create_bucket(const std::string &bucket) {
        call request{"PUT", bucket};
        std::string body;
        if (state_->options.region != "us-east-1") {
            body = "<CreateBucketConfiguration xmlns=\"http://s3.amazonaws.com/doc/2006-03-01/\"><LocationConstraint>" + state_->options.region +
                   "</LocationConstraint></CreateBucketConfiguration>";
            request.headers.emplace_back("Content-Type", "application/xml");
        }
        request.body = body;
        request.send_body = true;
        const auto response = state_->send(request, "CreateBucket " + bucket);
        state::check(response);
    }
}
