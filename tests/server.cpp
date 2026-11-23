// sc::s3::client against an S3-compatible server, when SC_S3_TEST_ENDPOINT names one (MinIO: http://127.0.0.1:9000), or
// AWS itself when it is "aws" (AWS_REGION), with AWS_ACCESS_KEY_ID/AWS_SECRET_ACCESS_KEY for it; SC_S3_TEST_BUCKET (default
// sc-s3-test) is created when there is no such bucket.
// Every key goes under test/<pid>/ and is deleted again. Skipped (passing) without an endpoint.
//
// put/head/get, ranges, checksums the server checks, listing with paging and folders,
// a multipart upload from a stream of unknown length (3 parts) read back byte for byte, a stream that fails half
// way (the upload is aborted: no pending uploads left), the rate limit, remove, errors (NoSuchKey, no bucket).
#include <sc/s3/s3.h>
#include <sc_test.h>

#include <unistd.h>

#include <chrono>
#include <cstdlib>
#include <iostream>
#include <istream>
#include <sstream>
#include <streambuf>

namespace {
    // length bytes of a repeating pattern, read in odd-sized pieces, then the end (or a failure).
    class pattern_buffer : public std::streambuf {
    public:
        pattern_buffer(const std::uint64_t length, const bool fail_at_end) : left_(length), fail_(fail_at_end) {}

    protected:
        int_type underflow() override {
            if (left_ == 0) {
                if (fail_) throw std::runtime_error("the source broke");
                return traits_type::eof();
            }
            const auto n = static_cast<std::size_t>(std::min<std::uint64_t>(left_, chunk_.size()));
            for (std::size_t i = 0; i < n; ++i) chunk_[i] = static_cast<char>('a' + (position_ + i) % 23);
            position_ += n;
            left_ -= n;
            setg(chunk_.data(), chunk_.data(), chunk_.data() + n);
            return traits_type::to_int_type(chunk_[0]);
        }

    private:
        std::array<char, 77777> chunk_{};
        std::uint64_t left_;
        std::uint64_t position_ = 0;
        bool fail_;
    };

    std::string pattern(const std::uint64_t length) {
        std::string out(length, '\0');
        for (std::uint64_t i = 0; i < length; ++i) out[i] = static_cast<char>('a' + i % 23);
        return out;
    }
}

int main() {
    using namespace sc::s3;
    using namespace sc::s3::data;

    const char *endpoint = std::getenv("SC_S3_TEST_ENDPOINT");
    if (!endpoint || !*endpoint) {
        std::cout << "SC_S3_TEST_ENDPOINT isn't set: no server to test against, skipped" << std::endl;
        return 0;
    }
    const char *bucket_name = std::getenv("SC_S3_TEST_BUCKET");
    const std::string bucket = bucket_name && *bucket_name ? bucket_name : "sc-s3-test";
    const std::string prefix = "test/" + std::to_string(::getpid()) + "/";

    auto options = settings::load({}, true);
    // "aws": AWS itself in AWS_REGION (virtual-hosted names); anything else is a server's URL (path style).
    if (std::string{endpoint} != "aws") {
        options.endpoint = endpoint;
        options.path_style = true;
    }
    options.retries = 1;
    client s3{options};
    try {
        (void)s3.list(bucket, {.max_keys = 1});
    } catch (const error &failure) {
        if (failure.code() != "NoSuchBucket") throw;
        s3.create_bucket(bucket);
    }

    SECTION("put, head, get");
    {
        put_options put;
        put.content_type = "text/plain";
        put.metadata = {{"Host", "hv2"}};
        const auto stored = s3.put(bucket, prefix + "hello world.txt", "hello, world\n", put);
        CHECK_EQ(stored.size.count(), 13ULL);
        CHECK(!stored.etag.empty());
        CHECK(!stored.checksum.empty());
        const auto info = s3.head(bucket, prefix + "hello world.txt", true);
        CHECK(info.has_value());
        if (info) {
            CHECK_EQ(info->size.count(), 13ULL);
            CHECK_EQ(info->content_type, std::string{"text/plain"});
            CHECK_EQ(info->metadata.count("host"), std::size_t{1});
            CHECK_EQ(info->checksums.count(checksum::crc32), std::size_t{1});
        }
        std::ostringstream out;
        const auto got = s3.get(bucket, prefix + "hello world.txt", out);
        CHECK_EQ(out.str(), std::string{"hello, world\n"});
        CHECK_EQ(got.size.count(), 13ULL);
        std::string piece;
        s3.get(bucket, prefix + "hello world.txt", [&](const std::string_view data) { piece += data; }, {.range = std::pair{7ULL, 11ULL}});
        CHECK_EQ(piece, std::string{"world"});
        CHECK(!s3.head(bucket, prefix + "nothing here"));
    }

    SECTION("checksums: sha256, crc32c, Content-MD5");
    {
        put_options sha;
        sha.checksum = checksum::sha256;
        CHECK(!s3.put(bucket, prefix + "sha", "data", sha).checksum.empty());
        put_options c;
        c.checksum = checksum::crc32c;
        CHECK_NOTHROW(s3.put(bucket, prefix + "crc32c", "data", c));
        put_options md5;
        md5.content_md5 = true;
        CHECK_NOTHROW(s3.put(bucket, prefix + "md5", "data", md5));
    }

    SECTION("a stream of unknown length: 3 parts, read back");
    {
        pattern_buffer source{MiB(12).count() + 12345, false};
        std::istream in{&source};
        upload_options upload;
        upload.part_size = MiB(5);
        int reports = 0;
        upload.on_progress = [&](const upload_progress &progress) { reports = progress.parts; };
        const auto stored = s3.upload(bucket, prefix + "big.bin", in, upload);
        CHECK_EQ(stored.parts, 3);
        CHECK_EQ(reports, 3);
        CHECK_EQ(stored.size.count(), MiB(12).count() + 12345);
        CHECK(stored.etag.ends_with("-3"));
        std::string back;
        s3.get(bucket, prefix + "big.bin", [&](const std::string_view data) { back += data; });
        CHECK_EQ(back.size(), static_cast<std::size_t>(MiB(12).count() + 12345));
        CHECK(back == pattern(MiB(12).count() + 12345));

        // GetObjectAttributes: each part's checksum is what a ranged read of it gives, and the parts' composite is the
        // object's checksum as the upload reported it.
        const auto attributes = s3.attributes(bucket, prefix + "big.bin");
        CHECK(attributes.has_value());
        if (attributes) {
            CHECK_EQ(attributes->parts.size(), std::size_t{3});
            CHECK(attributes->size == bytes{MiB(12).count() + 12345});
            std::vector<std::string> raw;
            for (const auto &part: attributes->parts) {
                std::string data;
                s3.get(bucket, prefix + "big.bin", [&](const std::string_view piece) { data += piece; }, {.range = attributes->part_range(part.number)});
                CHECK_EQ(crc{checksum::crc32}.update(data).base64(), part.checksums.at(checksum::crc32));
                raw.push_back(crc{checksum::crc32}.update(data).bytes());
            }
            CHECK_EQ(composite_checksum(checksum::crc32, raw) + "-3", stored.checksum);
        }
        CHECK(!s3.attributes(bucket, prefix + "no such object"));

        pattern_buffer small{1000, false};
        std::istream short_in{&small};
        const auto single = s3.upload(bucket, prefix + "small.bin", short_in, upload);
        CHECK_EQ(single.parts, 1);
        CHECK_EQ(single.size.count(), 1000ULL);

        pattern_buffer exact{MiB(5).count(), false}; // exactly one part: a plain PUT
        std::istream exact_in{&exact};
        CHECK_EQ(s3.upload(bucket, prefix + "exact.bin", exact_in, upload).parts, 1);
    }

    SECTION("a stream that breaks half way: aborted, nothing left");
    {
        pattern_buffer source{MiB(7).count(), true};
        std::istream in{&source};
        in.exceptions(std::ios::badbit);
        upload_options upload;
        upload.part_size = MiB(5);
        bool failed = false;
        try {
            s3.upload(bucket, prefix + "broken.bin", in, upload);
        } catch (const std::exception &) {
            failed = true;
        }
        CHECK(failed);
        CHECK(s3.pending_uploads(bucket, prefix).empty());
        CHECK(!s3.head(bucket, prefix + "broken.bin"));
    }

    SECTION("confirm: a source that failed stores nothing");
    {
        upload_options refuse;
        refuse.part_size = MiB(5);
        refuse.confirm = [] { return false; };
        for (const auto length: {std::uint64_t{1000}, MiB(11).count()}) {
            pattern_buffer source{length, false};
            std::istream in{&source};
            const auto key = prefix + "unconfirmed-" + std::to_string(length);
            std::string code;
            try {
                s3.upload(bucket, key, in, refuse);
            } catch (const error &failure) {
                code = failure.code();
            }
            CHECK_EQ(code, std::string{"NotConfirmed"});
            CHECK(!s3.head(bucket, key));
        }
        CHECK(s3.pending_uploads(bucket, prefix).empty());
    }

    SECTION("listing: pages and folders");
    {
        for (int i = 0; i < 5; ++i) s3.put(bucket, prefix + "list/" + std::to_string(i), "x");
        s3.put(bucket, prefix + "list/sub/deeper", "y");
        const auto first = s3.list(bucket, {.prefix = prefix + "list/", .max_keys = 2});
        CHECK_EQ(first.objects.size(), std::size_t{2});
        CHECK(first.truncated);
        std::vector<std::string> keys, folders;
        const auto seen = s3.list_all(bucket, {.prefix = prefix + "list/", .delimiter = "/", .max_keys = 2},
                                      [&](const object_entry &object) {
                                          keys.push_back(object.key);
                                          return true;
                                      },
                                      [&](const std::string &folder) { folders.push_back(folder); });
        CHECK_EQ(seen, 5ULL);
        CHECK_EQ(folders.size(), std::size_t{1});
        std::uint64_t everything = 0;
        s3.list_all(bucket, {.prefix = prefix + "list/"}, [&](const object_entry &) { return ++everything < 100; });
        CHECK_EQ(everything, 6ULL);
    }

    SECTION("rate limit");
    {
        auto slow = options;
        slow.rate_limit = MiB(1);
        client limited{slow};
        const auto started = std::chrono::steady_clock::now();
        limited.put(bucket, prefix + "slow.bin", pattern(MiB(2).count()));
        const double seconds = std::chrono::duration<double>(std::chrono::steady_clock::now() - started).count();
        CHECK_MSG(seconds >= 1.5, "2 MiB at 1 MiB/s took " + std::to_string(seconds) + " s");
    }

    SECTION("errors");
    {
        std::ostringstream out;
        try {
            s3.get(bucket, prefix + "missing", out);
            CHECK_MSG(false, "a missing key gave no error");
        } catch (const error &failure) {
            CHECK_EQ(failure.status(), 404L);
            CHECK_EQ(failure.code(), std::string{"NoSuchKey"});
            CHECK(!failure.retriable());
        }
        try {
            (void)s3.list("sc-s3-no-such-bucket-" + std::to_string(::getpid()));
            CHECK_MSG(false, "a missing bucket gave no error");
        } catch (const error &failure) {
            CHECK_EQ(failure.code(), std::string{"NoSuchBucket"});
        }
        auto wrong = options;
        wrong.credentials.secret_access_key = "not-the-secret";
        client refused{wrong};
        try {
            (void)refused.list(bucket);
            CHECK_MSG(false, "a wrong secret gave no error");
        } catch (const error &failure) {
            CHECK_EQ(failure.status(), 403L);
            CHECK_EQ(failure.code(), std::string{"SignatureDoesNotMatch"});
        }
    }

    SECTION("remove");
    {
        std::uint64_t removed = 0;
        s3.list_all(bucket, {.prefix = prefix}, [&](const object_entry &object) {
            s3.remove(bucket, object.key);
            ++removed;
            return true;
        });
        CHECK(removed >= 10);
        CHECK_NOTHROW(s3.remove(bucket, prefix + "never there"));
        CHECK(s3.list(bucket, {.prefix = prefix}).objects.empty());
    }

    TEST_SUMMARY();
}
