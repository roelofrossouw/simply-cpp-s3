#pragma once

#include <sc/s3/bytes.h>
#include <sc/s3/checksum.h>
#include <sc/s3/settings.h>

#include <chrono>
#include <functional>
#include <istream>
#include <map>
#include <memory>
#include <optional>
#include <ostream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

namespace sc::s3 {
    /// Where S3 keeps an object, from instant and dear to archived and cheap. glacier and
    /// deep_archive objects must be restored (client::restore) before they can be read.
    enum class storage_class { standard, reduced_redundancy, standard_ia, onezone_ia, intelligent_tiering, glacier_ir, glacier, deep_archive };

    /// S3's name: "STANDARD", "STANDARD_IA", "GLACIER_IR", "DEEP_ARCHIVE" ...
    [[nodiscard]] std::string_view to_string(storage_class kind);
    /// From S3's name, in any case ("deep_archive", "GLACIER_IR"); throws std::invalid_argument otherwise.
    [[nodiscard]] storage_class storage_class_from(std::string_view name);
    /// Whether objects of that class must be restored before they can be read.
    [[nodiscard]] bool archived(storage_class kind);

    /// How fast (and dear) a restore is: Deep Archive takes about 48 hours in bulk, 12 standard;
    /// Glacier 5 to 12 hours in bulk, 3 to 5 standard, minutes expedited (Glacier only).
    enum class restore_tier { bulk, standard, expedited };

    /// "Bulk", "Standard", "Expedited".
    [[nodiscard]] std::string_view to_string(restore_tier tier);
    /// From "bulk", "Standard" ... in any case; throws std::invalid_argument otherwise.
    [[nodiscard]] restore_tier restore_tier_from(std::string_view name);

    /// What S3 refused, or why a request got no answer.
    class error : public std::runtime_error {
    public:
        /// status: the HTTP status (0 when no answer came); code: S3's ("NoSuchKey", "SlowDown")
        /// or, without an answer, "Network"; request_id: S3's x-amz-request-id, for AWS support.
        error(long status, std::string code, const std::string &message, std::string request_id = {});

        [[nodiscard]] long status() const { return status_; }
        [[nodiscard]] const std::string &code() const { return code_; }
        [[nodiscard]] const std::string &request_id() const { return request_id_; }
        /// Whether trying again may work: no answer, a timeout, a 5xx, 429 or S3's SlowDown,
        /// RequestTimeout or InternalError.
        [[nodiscard]] bool retriable() const;

    private:
        long status_;
        std::string code_;
        std::string request_id_;
    };

    /// What a restore of an archived object is doing, from x-amz-restore.
    struct restore_status {
        /// Whether a restore was asked for (and the copy not yet expired).
        bool requested = false;
        /// Still being restored; when false and requested, the copy can be read until expiry.
        bool ongoing = false;
        std::optional<std::chrono::system_clock::time_point> expiry;

        /// Whether the object can be read now (requested, done, not expired).
        [[nodiscard]] bool available() const { return requested && !ongoing; }

        /// Reads x-amz-restore's value: ongoing-request="false", expiry-date="Fri, 21 Dec 2012 00:00:00 GMT".
        static restore_status parse(std::string_view header);
    };

    /// An object as HEAD and GET describe it.
    struct object_info {
        std::string key;
        bytes size;
        /// The ETag, without its quotes ("…-3" for an object uploaded in 3 parts).
        std::string etag;
        std::chrono::system_clock::time_point last_modified;
        /// STANDARD when S3 leaves it out, as it does for STANDARD.
        storage_class kind = storage_class::standard;
        std::string content_type;
        std::string version_id;
        /// x-amz-meta-* by their name without the prefix, lowercase.
        std::map<std::string, std::string> metadata;
        /// x-amz-checksum-* (when asked for, with checksum mode) by algorithm.
        std::map<checksum, std::string> checksums;
        sc::s3::restore_status restore;

        /// From a response's headers, by lowercase name (what parse_head() reads).
        static object_info from_headers(std::string key, const std::map<std::string, std::string> &headers);
    };

    /// An object in a listing.
    struct object_entry {
        std::string key;
        bytes size;
        std::string etag;
        std::chrono::system_clock::time_point last_modified;
        storage_class kind = storage_class::standard;
    };

    /// One page of a listing (up to max_keys).
    struct list_page {
        std::vector<object_entry> objects;
        /// With a delimiter: the "folders", the keys' beginnings up to and including it.
        std::vector<std::string> prefixes;
        /// More pages follow: pass next_token as the next request's continuation_token.
        bool truncated = false;
        std::string next_token;

        /// Reads ListObjectsV2's XML (url-encoded keys or not).
        static list_page parse(std::string_view xml);
    };

    struct list_options {
        std::string prefix;
        /// "/" to list one level: deeper keys come back as prefixes.
        std::string delimiter;
        /// 1 to 1000.
        int max_keys = 1000;
        std::string continuation_token;
        /// Keys after this one (the first page only).
        std::string start_after;
    };

    /// A multipart upload that was started and neither completed nor aborted; S3 bills its parts
    /// until it's aborted (or a lifecycle rule does).
    struct pending_upload {
        std::string key;
        std::string upload_id;
        std::chrono::system_clock::time_point initiated;
        storage_class kind = storage_class::standard;

        static std::vector<pending_upload> parse(std::string_view xml, bool *truncated = nullptr, std::string *next_key = nullptr,
                                                 std::string *next_upload_id = nullptr);
    };

    struct put_options {
        storage_class kind = storage_class::standard;
        sc::s3::checksum checksum = checksum::crc32;
        /// Also send Content-MD5 (SigV4 already has S3 check the body's SHA-256; some buckets,
        /// with Object Lock, want MD5 as well).
        bool content_md5 = false;
        std::string content_type = "application/octet-stream";
        /// x-amz-meta-<name>: lowercase names, ASCII values.
        std::map<std::string, std::string> metadata;
    };

    /// How far an upload has got: after each part.
    struct upload_progress {
        int parts = 0;
        bytes sent;
        std::chrono::steady_clock::duration elapsed{};
    };

    struct upload_options : put_options {
        /// The first parts' size, at least 5 MiB (S3's least but for the last part). To fit S3's
        /// 10,000 parts it doubles after every 1,000 (up to 5 GiB): from 64 MiB that holds about
        /// 24 TB; from 5 MiB about 5 TB. Each part is read into memory, two at a time (one
        /// being sent while the next is read).
        bytes part_size = data::MiB(64);
        /// When the total is about known (zfs send -nP), parts big enough for it in 9,000 parts
        /// from the start; 0 when unknown.
        bytes expected_size{};
        /// Called after each part is stored.
        std::function<void(const upload_progress &)> on_progress;
        /// Asked before each part and before completing: true stops the upload. It's aborted (its parts
        /// deleted) and upload() throws sc::s3::error with code "Stopped". For a signal handler's flag.
        std::function<bool()> stop;
        /// Asked once the stream has ended, before the object is stored (the single PUT, or completing the
        /// multipart upload): false and nothing is stored (the upload aborted), upload() throws sc::s3::error with
        /// code "NotConfirmed". For a producer that may fail part way (pg_basebackup | ...): its exit status, so a
        /// truncated stream never becomes an object.
        std::function<bool()> confirm;
    };

    /// A finished upload.
    struct upload_result {
        std::string key;
        bytes size;
        std::string etag;
        std::string version_id;
        /// 1 for a single PUT.
        int parts = 0;
        /// The object's checksum as S3 reports it ("…==-3", of the parts' checksums, after a
        /// multipart upload); empty when none was asked for or S3 sent none back.
        std::string checksum;
    };

    struct get_options {
        /// Bytes from first to last (both included); nullopt for the whole object.
        std::optional<std::pair<std::uint64_t, std::uint64_t>> range;
        /// Ask for the object's stored checksums (x-amz-checksum-mode: ENABLED).
        bool checksums = false;
    };

    /// GetObjectAttributes' answer: the object's size, ETag, storage class and checksum, and for an object
    /// uploaded in parts each part's number, size and checksum (what S3 checked when the part arrived).
    struct object_attributes {
        struct part {
            int number = 0;
            bytes size;
            /// The part's checksum in base64, by algorithm (the one the upload used).
            std::map<checksum, std::string> checksums;
        };
        std::string etag;
        bytes size;
        storage_class kind = storage_class::standard;
        /// The object's checksum in base64 ("...==" for a single PUT; of the parts' checksums, without S3's
        /// "-<parts>" suffix, for a multipart upload), by algorithm.
        std::map<checksum, std::string> checksums;
        /// Empty for an object stored in one PUT.
        std::vector<part> parts;
        int parts_count = 0;

        /// Reads GetObjectAttributes' XML (one page of parts).
        static object_attributes parse(std::string_view xml, bool *truncated = nullptr, int *next_part_marker = nullptr);
        /// The bytes [first, last] the part covers in the object (parts are numbered from 1, in order).
        [[nodiscard]] std::pair<std::uint64_t, std::uint64_t> part_range(int number) const;
    };

    /// What a restore request did.
    enum class restore_result {
        /// Started (202): the copy is there in hours (see restore_tier).
        started,
        /// Already restored (200): the days were updated from now.
        extended,
        /// A restore is already running (409 RestoreAlreadyInProgress).
        in_progress
    };

    /// An S3 client on libcurl: objects put from memory, files or a pipe of any length, listed,
    /// described, read (streamed), deleted and restored, signed with SigV4. One client is one
    /// connection, kept open between requests; use one a thread.
    ///
    ///     sc::s3::client s3{sc::s3::settings::load()};
    ///     sc::s3::upload_options options;
    ///     options.kind = sc::s3::storage_class::deep_archive;
    ///     auto stored = s3.upload("backups", "db3/base.tar", std::cin, options);   // pg_basebackup -Ft -D - | ...
    ///     for (const auto &object: s3.list("backups", {.prefix = "db3/"}).objects) ...
    ///     if (auto info = s3.head("backups", "db3/base.tar"); info && sc::s3::archived(info->kind)) s3.restore(...);
    class client {
    public:
        explicit client(sc::s3::settings settings);
        ~client();
        client(const client &) = delete;
        client &operator=(const client &) = delete;
        client(client &&) noexcept;
        client &operator=(client &&) noexcept;

        [[nodiscard]] const sc::s3::settings &settings() const;
        /// Replaces the credentials (fresh temporary ones, say).
        void use(const sc::s3::credentials &key);

        /// Stores body as the object (PutObject, up to 5 GiB).
        upload_result put(const std::string &bucket, const std::string &key, std::string_view body, const put_options &options = {});
        /// Stores in as the object, reading it to its end without knowing its length: one PUT when
        /// it's shorter than a part, else a multipart upload, aborted when it fails (so no parts are
        /// left to be billed). Each part is tried again settings().retries times. Throws error
        /// (or the stream's failure as std::runtime_error).
        upload_result upload(const std::string &bucket, const std::string &key, std::istream &in, const upload_options &options = {});
        /// The same from a file (a regular file, a pipe, /dev/stdin).
        upload_result upload_file(const std::string &bucket, const std::string &key, const std::filesystem::path &file,
                                  const upload_options &options = {});

        /// One page of the bucket's objects (ListObjectsV2).
        list_page list(const std::string &bucket, const list_options &options = {});
        /// Every page's objects, handed to each in turn until it returns false; prefixes (with a
        /// delimiter) to prefix if given. Returns how many objects it saw.
        std::uint64_t list_all(const std::string &bucket, list_options options, const std::function<bool(const object_entry &)> &each,
                               const std::function<void(const std::string &)> &prefix = {});

        /// The object's description (HeadObject), or nullopt when there's no such object.
        std::optional<object_info> head(const std::string &bucket, const std::string &key, bool checksums = false);

        /// Writes the object (or a range of it) to out as it arrives, without holding it. When the
        /// connection breaks part way, it goes on from where it got to (a ranged GET of the same
        /// ETag). Throws error: NoSuchKey, InvalidObjectState for an archived object not restored.
        object_info get(const std::string &bucket, const std::string &key, std::ostream &out, const get_options &options = {});
        /// The same, handing each piece to sink.
        object_info get(const std::string &bucket, const std::string &key, const std::function<void(std::string_view)> &sink,
                        const get_options &options = {});

        /// The object's attributes and, for a multipart upload, every part's size and checksum
        /// (GetObjectAttributes, all pages); nullopt when there's no such object. Works on archived objects too.
        std::optional<object_attributes> attributes(const std::string &bucket, const std::string &key);

        /// Deletes the object; deleting one that isn't there isn't an error (S3's way).
        void remove(const std::string &bucket, const std::string &key);

        /// Asks for an archived object to be restored for days, at tier's speed (RestoreObject).
        restore_result restore(const std::string &bucket, const std::string &key, std::chrono::days days,
                               restore_tier tier = restore_tier::bulk);

        /// Multipart uploads started and not finished (ListMultipartUploads), under prefix.
        std::vector<pending_upload> pending_uploads(const std::string &bucket, const std::string &prefix = {});
        /// Aborts one, dropping its parts.
        void abort_upload(const std::string &bucket, const std::string &key, const std::string &upload_id);

        /// A bucket's configuration by its subresource, as S3's XML ("versioning", "object-lock",
        /// "lifecycle", "encryption", "policy", ...; since 2.1.0), or nullopt when the bucket has none
        /// (S3's NoSuch... / ...NotFoundError). Throws error otherwise (NoSuchBucket, AccessDenied).
        std::optional<std::string> bucket_config(const std::string &bucket, const std::string &subresource);

        /// Creates a bucket in the settings' region (for tests and S3-compatible servers).
        void create_bucket(const std::string &bucket);

    private:
        struct state;
        std::unique_ptr<state> state_;
    };

    /// The size of part number (from 1) of an upload whose first parts are base: base, doubled
    /// after every 1,000 parts, at most 5 GiB.
    [[nodiscard]] bytes part_size_for(int part_number, bytes base);
    /// The first parts' size for an upload: requested, at least 5 MiB, and big enough for
    /// expected (when not 0) in 9,000 parts, rounded up to a MiB.
    [[nodiscard]] bytes first_part_size(bytes requested, bytes expected = {});
    /// The most an upload starting at base can hold in 10,000 parts.
    [[nodiscard]] bytes largest_upload(bytes base);

    /// "Fri, 21 Dec 2012 00:00:00 GMT" (HTTP dates) and "2012-12-21T00:00:00.000Z" (ISO 8601, as
    /// S3's XML has them); throws std::invalid_argument for anything else.
    [[nodiscard]] std::chrono::system_clock::time_point parse_time(std::string_view text);

    /// "s3://bucket/key/with/slashes" into bucket and key (key may be empty); throws std::invalid_argument
    /// for text that isn't an s3:// URL with a bucket.
    [[nodiscard]] std::pair<std::string, std::string> parse_url(std::string_view url);
}
