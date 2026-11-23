// sc-s3: S3 from the command line, for scripts and tests: put (a file or a pipe of any length), get,
// ls, head, restore, rm, and the multipart uploads left behind (uploads, abort).
#include <sc/core/params.h>
#include <sc/s3/s3.h>

#include <unistd.h>

#include <atomic>
#include <csignal>

#include <chrono>
#include <cstdio>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <sstream>

namespace {
    // Set by SIGINT, SIGTERM, SIGHUP and SIGPIPE (the caller's ssh gone: progress can't be written): an upload
    // stops before its next part and is aborted, so no parts are left to be billed.
    std::atomic<bool> stop_asked{false};

    void ask_stop(int) { stop_asked.store(true); }

    // JSON string text: quotes, backslashes and control characters escaped.
    std::string json_text(const std::string &text) {
        std::string out = "\"";
        for (const char c: text) {
            if (c == '"' || c == '\\') out += std::string{"\\"} + c;
            else if (static_cast<unsigned char>(c) < 0x20) {
                char escaped[8];
                std::snprintf(escaped, sizeof escaped, "\\u%04x", static_cast<unsigned>(c));
                out += escaped;
            } else out += c;
        }
        return out + "\"";
    }

    std::string when(const std::chrono::system_clock::time_point time) {
        const std::time_t seconds = std::chrono::system_clock::to_time_t(time);
        std::tm utc{};
        gmtime_r(&seconds, &utc);
        char text[32];
        std::strftime(text, sizeof text, "%Y-%m-%d %H:%M:%S", &utc);
        return text;
    }

    // A size as people read it: "312 B", "64 MiB", "1.5 GiB".
    std::string readable(const sc::s3::bytes size) {
        using namespace sc::s3::data;
        std::ostringstream out;
        out << std::setprecision(3);
        if (size >= TiB(1)) out << size.in<TiB>().value() << " TiB";
        else if (size >= GiB(1)) out << size.in<GiB>().value() << " GiB";
        else if (size >= MiB(1)) out << size.in<MiB>().value() << " MiB";
        else if (size >= KiB(1)) out << size.in<KiB>().value() << " KiB";
        else out << size.count() << " B";
        return out.str();
    }

    std::string text(const sc::params &args, const char *name, std::string fallback = {}) {
        const auto *value = args.lookup(name);
        return value ? value->get<std::string>() : fallback;
    }
}

int main(int argc, char **argv) {
    const sc::params args{argc, argv, {
        {"name", "sc-s3"},
        {"description", "Stores, reads, lists, describes, restores and deletes objects in AWS S3 or an S3-compatible server, "
                        "without the AWS CLI. put reads a file or standard input to its end without knowing its length "
                        "(zfs send, pg_basebackup -Ft -D -) and goes as a multipart upload when it's bigger than a part."},
        {"version", SC_S3_VERSION},
        {"epilog", "COMMAND is put, get, ls, head, restore, rm, uploads (multipart uploads not finished) or abort (one of them). "
                   "URL is s3://bucket/key (s3://bucket/prefix for ls and uploads). put reads FILE (default: standard input) "
                   "and get writes FILE (default: standard output); - is either. Settings come from --config "
                   "(/etc/sc-s3/sc-s3.conf if it's there), then AWS_REGION, AWS_ENDPOINT_URL, AWS_ACCESS_KEY_ID, "
                   "AWS_SECRET_ACCESS_KEY and AWS_SESSION_TOKEN, then the options. Exits 1 when S3 refuses, 2 for wrong use."},
        {"options", {
            {"config", {{"short", "c"}, {"value", "FILE"}, {"help", "the settings file (and its .d/ drop-ins)"}}},
            {"region", {{"value", "REGION"}, {"help", "the region, af-south-1"}}},
            {"endpoint", {{"value", "URL"}, {"help", "an S3-compatible server, http://127.0.0.1:9000 (default: AWS)"}}},
            {"path-style", {{"type", "boolean"}, {"help", "the bucket in the path, not the host name (most S3-compatible servers)"}}},
            {"rate-limit", {{"value", "SIZE"}, {"help", "at most SIZE a second each way, 10M"}}},
            {"storage-class", {{"short", "s"}, {"value", "CLASS"}, {"default", "STANDARD"},
                               {"help", "put: STANDARD, STANDARD_IA, ONEZONE_IA, INTELLIGENT_TIERING, GLACIER_IR, GLACIER or DEEP_ARCHIVE"}}},
            {"checksum", {{"value", "ALGORITHM"}, {"default", "crc32"}, {"choices", {"crc32", "crc32c", "sha256", "none"}},
                          {"help", "put: the checksum S3 checks and keeps"}}},
            {"part-size", {{"value", "SIZE"}, {"default", "64M"}, {"help", "put: the first parts' size (doubling every 1,000 parts)"}}},
            {"expected-size", {{"value", "SIZE"}, {"help", "put: about how much comes, for big enough parts from the start"}}},
            {"content-type", {{"value", "TYPE"}, {"default", "application/octet-stream"}, {"help", "put: the object's Content-Type"}}},
            {"days", {{"type", "integer"}, {"value", "DAYS"}, {"default", 7}, {"help", "restore: how long the restored copy stays"}}},
            {"tier", {{"value", "TIER"}, {"default", "bulk"}, {"choices", {"bulk", "standard", "expedited"}},
                      {"help", "restore: bulk (cheapest, Deep Archive within 48 h), standard (12 h) or expedited (Glacier only)"}}},
            {"recursive", {{"short", "r"}, {"type", "boolean"}, {"help", "ls: every key below, not one level"}}},
            {"upload-id", {{"value", "ID"}, {"help", "abort: the upload to abort (from uploads)"}}},
            {"role-arn", {{"value", "ARN"}, {"help", "assume this role (STS) first and use its temporary credentials"}}},
            {"session-name", {{"value", "NAME"}, {"default", "sc-s3"}, {"help", "with --role-arn: the session's name in CloudTrail"}}},
            {"progress", {{"short", "p"}, {"type", "boolean"}, {"help", "put: report each part on standard error"}}},
            {"json", {{"type", "boolean"}, {"help", "put: the result as one line of JSON (bucket, key, size, parts, etag, checksum, seconds)"}}}}},
        {"positional", {
            {"command", {{"help", "put, get, ls, head, restore, rm, uploads or abort"}}},
            {"url", {{"help", "s3://bucket/key"}}},
            {"file", {{"required", false}, {"help", "put's source or get's destination (default: standard input/output)"}}}}}}};

    for (const int signal: {SIGINT, SIGTERM, SIGHUP, SIGPIPE}) std::signal(signal, ask_stop);
    try {
        auto settings = sc::s3::settings::load(text(args, "config", sc::s3::settings::default_file));
        if (const auto *region = args.lookup("region")) settings.region = region->get<std::string>();
        if (const auto *endpoint = args.lookup("endpoint")) settings.endpoint = endpoint->get<std::string>();
        if (args.as<bool>("path-style")) settings.path_style = true;
        if (const auto *limit = args.lookup("rate-limit")) settings.rate_limit = sc::s3::bytes::parse(limit->get<std::string>());
        if (const auto *role = args.lookup("role-arn")) {
            settings.credentials = sc::s3::assume_role(settings, {.role_arn = role->get<std::string>(), .session_name = args.as<std::string>("session-name")});
        }
        if (settings.credentials.empty()) throw std::invalid_argument("no credentials: set them in the settings file or AWS_ACCESS_KEY_ID and AWS_SECRET_ACCESS_KEY");

        const auto command = args.as<std::string>("command");
        const auto [bucket, key] = sc::s3::parse_url(args.as<std::string>("url"));
        const auto file = text(args, "file", "-");
        sc::s3::client s3{settings};

        const auto need_key = [&] {
            if (key.empty() || key.ends_with('/')) throw std::invalid_argument(command + " needs a key: s3://bucket/key");
        };

        if (command == "put") {
            need_key();
            sc::s3::upload_options options;
            options.kind = sc::s3::storage_class_from(args.as<std::string>("storage-class"));
            options.checksum = sc::s3::checksum_from(args.as<std::string>("checksum"));
            options.part_size = sc::s3::bytes::parse(args.as<std::string>("part-size"));
            if (const auto *expected = args.lookup("expected-size")) options.expected_size = sc::s3::bytes::parse(expected->get<std::string>());
            options.content_type = args.as<std::string>("content-type");
            if (args.as<bool>("progress")) {
                options.on_progress = [](const sc::s3::upload_progress &progress) {
                    const double seconds = std::chrono::duration<double>(progress.elapsed).count();
                    std::cerr << "part " << progress.parts << ": " << readable(progress.sent) << " in " << std::fixed << std::setprecision(1) << seconds
                              << " s (" << readable(sc::s3::bytes{static_cast<std::uint64_t>(seconds > 0 ? progress.sent.count() / seconds : 0)})
                              << "/s)" << std::defaultfloat << std::endl;
                };
            }
            options.stop = [] { return stop_asked.load(); };
            if (file == "-" && isatty(STDIN_FILENO)) throw std::invalid_argument("put: nothing piped in; give FILE or pipe the data");
            const auto started = std::chrono::steady_clock::now();
            const auto stored = file == "-" ? s3.upload(bucket, key, std::cin, options) : s3.upload_file(bucket, key, file, options);
            if (args.as<bool>("json")) {
                const double seconds = std::chrono::duration<double>(std::chrono::steady_clock::now() - started).count();
                std::cout << "{\"bucket\":" << json_text(bucket) << ",\"key\":" << json_text(key) << ",\"size\":" << stored.size.count()
                          << ",\"parts\":" << stored.parts << ",\"etag\":" << json_text(stored.etag) << ",\"checksum\":" << json_text(stored.checksum)
                          << ",\"seconds\":" << std::fixed << std::setprecision(1) << seconds << "}" << std::endl;
                return 0;
            }
            std::cout << "s3://" << bucket << '/' << key << ": " << readable(stored.size) << " (" << stored.size.count() << " bytes), " << stored.parts
                      << (stored.parts == 1 ? " part" : " parts") << ", etag " << stored.etag;
            if (!stored.checksum.empty()) std::cout << ", " << args.as<std::string>("checksum") << ' ' << stored.checksum;
            std::cout << std::endl;
        } else if (command == "get") {
            need_key();
            if (file == "-") {
                s3.get(bucket, key, std::cout);
                std::cout.flush();
            } else {
                const auto partial = file + ".part";
                {
                    std::ofstream out{partial, std::ios::binary | std::ios::trunc};
                    if (!out) throw std::runtime_error("can't write " + partial);
                    s3.get(bucket, key, out);
                    out.close();
                    if (!out) throw std::runtime_error("writing " + partial + " failed");
                }
                std::filesystem::rename(partial, file);
            }
        } else if (command == "ls") {
            sc::s3::list_options options;
            options.prefix = key;
            if (!args.as<bool>("recursive")) options.delimiter = "/";
            s3.list_all(bucket, options, [](const sc::s3::object_entry &object) {
                std::cout << when(object.last_modified) << "  " << std::setw(12) << object.size.count() << "  " << std::left << std::setw(12)
                          << sc::s3::to_string(object.kind) << std::right << "  " << object.key << '\n';
                return true;
            }, [](const std::string &prefix) { std::cout << std::string(21, ' ') << std::setw(12) << "PRE" << "  " << std::setw(12) << "" << "  " << prefix << '\n'; });
            std::cout.flush();
        } else if (command == "head") {
            need_key();
            const auto info = s3.head(bucket, key, true);
            if (!info) {
                std::cerr << "s3://" << bucket << '/' << key << ": no such object" << std::endl;
                return 1;
            }
            std::cout << "key: " << info->key << "\nsize: " << info->size.count() << " (" << readable(info->size) << ")\netag: " << info->etag
                      << "\nlast modified: " << when(info->last_modified) << " UTC\nstorage class: " << sc::s3::to_string(info->kind)
                      << "\ncontent type: " << info->content_type << '\n';
            if (!info->version_id.empty()) std::cout << "version: " << info->version_id << '\n';
            for (const auto &[algorithm, value]: info->checksums) std::cout << "checksum " << sc::s3::to_string(algorithm) << ": " << value << '\n';
            for (const auto &[name, value]: info->metadata) std::cout << "meta " << name << ": " << value << '\n';
            if (sc::s3::archived(info->kind) || info->restore.requested) {
                std::cout << "restore: "
                          << (!info->restore.requested ? "not asked for (restore it to read it)"
                              : info->restore.ongoing  ? "in progress"
                                                       : "available");
                if (info->restore.expiry) std::cout << " until " << when(*info->restore.expiry) << " UTC";
                std::cout << '\n';
            }
            std::cout.flush();
        } else if (command == "restore") {
            need_key();
            const auto days = args.as<int>("days");
            switch (s3.restore(bucket, key, std::chrono::days{days}, sc::s3::restore_tier_from(args.as<std::string>("tier")))) {
                case sc::s3::restore_result::started: std::cout << "restore started (head shows when it's there)" << std::endl; break;
                case sc::s3::restore_result::extended: std::cout << "already restored; kept " << days << " days from now" << std::endl; break;
                case sc::s3::restore_result::in_progress: std::cout << "a restore is already in progress" << std::endl; break;
            }
        } else if (command == "rm") {
            need_key();
            s3.remove(bucket, key);
        } else if (command == "uploads") {
            for (const auto &upload: s3.pending_uploads(bucket, key)) {
                std::cout << when(upload.initiated) << "  " << upload.upload_id << "  " << upload.key << '\n';
            }
            std::cout.flush();
        } else if (command == "abort") {
            need_key();
            const auto id = text(args, "upload-id");
            if (id.empty()) throw std::invalid_argument("abort needs --upload-id (see uploads)");
            s3.abort_upload(bucket, key, id);
        } else {
            throw std::invalid_argument("unknown command " + command + " (put, get, ls, head, restore, rm, uploads, abort)");
        }
        return 0;
    } catch (const sc::s3::error &failure) {
        std::cerr << "sc-s3: " << failure.what();
        if (!failure.request_id().empty()) std::cerr << " [request " << failure.request_id() << ']';
        std::cerr << std::endl;
        return 1;
    } catch (const std::invalid_argument &failure) {
        std::cerr << "sc-s3: " << failure.what() << std::endl;
        return 2;
    } catch (const std::exception &failure) {
        std::cerr << "sc-s3: " << failure.what() << std::endl;
        return 1;
    }
}
