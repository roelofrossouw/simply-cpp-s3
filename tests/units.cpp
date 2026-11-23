// sc-s3 without a server: SigV4 against AWS's published examples (the sig-v4 test suite's get-vanilla, the
// S3 documentation's GET object, list and PUT examples, the IAM signing-key example), sizes, checksums, XML,
// part sizes, times, restore status, ListObjectsV2 and AssumeRole answers, settings files and s3:// URLs.
#include <sc/s3/s3.h>
#include <sc_test.h>

#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <utility>

#include <unistd.h>

namespace {
    std::chrono::system_clock::time_point at(const int year, const int month, const int day, const int hour, const int minute, const int second) {
        std::tm parts{};
        parts.tm_year = year - 1900;
        parts.tm_mon = month - 1;
        parts.tm_mday = day;
        parts.tm_hour = hour;
        parts.tm_min = minute;
        parts.tm_sec = second;
        return std::chrono::system_clock::from_time_t(timegm(&parts));
    }

    // The S3 documentation's example secret ("/bPx"), not the test suite's ("+bPx").
    const sc::s3::credentials example_key{"AKIAIOSFODNN7EXAMPLE", "wJalrXUtnFEMI/K7MDENG/bPxRfiCYEXAMPLEKEY", {}, std::nullopt};
    const std::string empty_hash = "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855";

    std::string signature_of(const std::string &authorization) { return authorization.substr(authorization.rfind('=') + 1); }
}

int main() {
    using namespace sc::s3;
    using namespace sc::s3::data;

    SECTION("SigV4: AWS test suite get-vanilla");
    {
        signed_request request;
        request.method = "GET";
        request.path = "/";
        request.headers = {{"Host", "example.amazonaws.com"}};
        const credentials key{"AKIDEXAMPLE", "wJalrXUtnFEMI/K7MDENG+bPxRfiCYEXAMPLEKEY", {}, std::nullopt};
        const auto authorization = sigv4::sign(request, key, "us-east-1", "service", at(2015, 8, 30, 12, 36, 0));
        CHECK_EQ(signature_of(authorization), std::string{"5fa00fa31553b73ebf1942676e86291e8372ff2a2260956d9b8aae1d763fbf31"});
        CHECK_EQ(sigv4::signed_headers(request), std::string{"host;x-amz-date"});
        CHECK(authorization.starts_with("AWS4-HMAC-SHA256 Credential=AKIDEXAMPLE/20150830/us-east-1/service/aws4_request, SignedHeaders=host;x-amz-date, "));
    }

    SECTION("SigV4: the signing key (IAM example)");
    CHECK_EQ(hex(sigv4::signing_key("wJalrXUtnFEMI/K7MDENG+bPxRfiCYEXAMPLEKEY", "20120215", "us-east-1", "iam")),
             std::string{"f4780e2d9f65fa895f9c67b32ce1baf0b0d8a43505a000a1a9e090d414db404d"});

    SECTION("SigV4: S3 GET object with a range (S3 documentation)");
    {
        signed_request request;
        request.path = "/test.txt";
        request.headers = {{"Host", "examplebucket.s3.amazonaws.com"}, {"Range", "bytes=0-9"}};
        request.payload_hash = empty_hash;
        const auto authorization = sigv4::sign(request, example_key, "us-east-1", "s3", at(2013, 5, 24, 0, 0, 0));
        CHECK_EQ(signature_of(authorization), std::string{"f0e8bdb87c964420e857bd35b5d6ed310bd44f0170aba48dd91039c6036bdb41"});
        CHECK_EQ(sigv4::signed_headers(request), std::string{"host;range;x-amz-content-sha256;x-amz-date"});
    }

    SECTION("SigV4: S3 list objects (S3 documentation)");
    {
        signed_request request;
        request.path = "/";
        request.query = {{"max-keys", "2"}, {"prefix", "J"}};
        request.headers = {{"Host", "examplebucket.s3.amazonaws.com"}};
        const auto authorization = sigv4::sign(request, example_key, "us-east-1", "s3", at(2013, 5, 24, 0, 0, 0));
        CHECK_EQ(signature_of(authorization), std::string{"34b48302e7b5fa45bde8084f4b7868a86f0a534bc59db6670ed5711ef69dc6f7"});
    }

    SECTION("SigV4: S3 PUT object (S3 documentation)");
    {
        signed_request request;
        request.method = "PUT";
        request.path = "/" + sigv4::uri_encode("test$file.text", true);
        request.headers = {{"Host", "examplebucket.s3.amazonaws.com"}, {"Date", "Fri, 24 May 2013 00:00:00 GMT"},
                           {"x-amz-storage-class", "REDUCED_REDUNDANCY"}};
        request.payload_hash = hex(sha256("Welcome to Amazon S3."));
        CHECK_EQ(request.payload_hash, std::string{"44ce7dd67c959e0d3524ffac1771dfbba87d2b6b4b4e99e42034a8b803f8b072"});
        const auto authorization = sigv4::sign(request, example_key, "us-east-1", "s3", at(2013, 5, 24, 0, 0, 0));
        CHECK_EQ(signature_of(authorization), std::string{"98ad721746da40c64f1a55b78f14c238d841ea1380cd77a1b5971af0ece108bd"});
    }

    SECTION("SigV4: session token, re-signing, no host");
    {
        signed_request request;
        request.headers = {{"host", "example.com"}};
        const credentials temporary{"ASIAEXAMPLE", "secret", "token-value", std::nullopt};
        sigv4::sign(request, temporary, "af-south-1", "s3");
        sigv4::sign(request, temporary, "af-south-1", "s3"); // again: replaces, doesn't pile up
        int tokens = 0, authorizations = 0;
        for (const auto &[name, value]: request.headers) {
            if (name == "x-amz-security-token") ++tokens;
            if (name == "Authorization") ++authorizations;
        }
        CHECK_EQ(tokens, 1);
        CHECK_EQ(authorizations, 1);
        CHECK(sigv4::signed_headers(request).find("x-amz-security-token") != std::string::npos);
        signed_request hostless;
        CHECK_THROWS_AS(sigv4::sign(hostless, temporary, "af-south-1", "s3"), std::invalid_argument);
        CHECK_THROWS_AS(sigv4::sign(request, credentials{}, "af-south-1", "s3"), std::invalid_argument);
    }

    SECTION("uri_encode");
    CHECK_EQ(sigv4::uri_encode("a b/c~d$e"), std::string{"a%20b%2Fc~d%24e"});
    CHECK_EQ(sigv4::uri_encode("a b/c", true), std::string{"a%20b/c"});
    CHECK_EQ(sigv4::uri_encode("é"), std::string{"%C3%A9"});

    SECTION("sizes");
    CHECK_EQ(bytes::parse("64M").count(), 64ULL << 20);
    CHECK_EQ(bytes::parse("64MiB").count(), 64ULL << 20);
    CHECK_EQ(bytes::parse("100kB").count(), 100000ULL);
    CHECK_EQ(bytes::parse("1.5G").count(), 3ULL << 29);
    CHECK_EQ(bytes::parse("4096").count(), 4096ULL);
    CHECK_THROWS_AS(bytes::parse("ten"), std::invalid_argument);
    CHECK_THROWS_AS(bytes::parse("5X"), std::invalid_argument);
    CHECK(MiB(64) == bytes{64ULL << 20});
    CHECK_NEAR(MiB(64).in<GiB>().value(), 0.0625, 1e-12);

    SECTION("checksums");
    CHECK_EQ(crc{checksum::crc32}.update("1234").update("56789").value(), 0xCBF43926U);
    CHECK_EQ(crc{checksum::crc32}.update("123456789").base64(), std::string{"y/Q5Jg=="});
    CHECK_EQ(crc{checksum::crc32c}.update("123456789").value(), 0xE3069283U);
    CHECK_EQ(checksum_of(checksum::sha256, ""), std::string{"47DEQpj8HBSa+/TImW+5JCeuQeRkm5NMpJWZG3hSuFU="});
    CHECK_EQ(checksum_of(checksum::none, "x"), std::string{});
    CHECK(checksum_from("CRC32C") == checksum::crc32c);
    CHECK_THROWS_AS(checksum_from("md5"), std::invalid_argument);

    SECTION("part sizes");
    CHECK(part_size_for(1, MiB(8)) == MiB(8));
    CHECK(part_size_for(1000, MiB(8)) == MiB(8));
    CHECK(part_size_for(1001, MiB(8)) == MiB(16));
    CHECK(part_size_for(10000, MiB(8)) == MiB(8 * 512));
    CHECK(part_size_for(10000, GiB(1)) == GiB(5)); // capped
    CHECK(part_size_for(1, MiB(1)) == MiB(5));       // S3's least
    CHECK(first_part_size(MiB(8)) == MiB(8));
    CHECK(first_part_size(MiB(8), TiB(1)) >= bytes{(1ULL << 40) / 9000});
    CHECK(largest_upload(MiB(5)) > TB(5));
    CHECK(largest_upload(MiB(64)) > TB(24)); // 64 MiB doubling to the 5 GiB cap: 23,488 MiB x 1000

    SECTION("names");
    CHECK(storage_class_from("deep_archive") == storage_class::deep_archive);
    CHECK_EQ(std::string{to_string(storage_class::glacier_ir)}, std::string{"GLACIER_IR"});
    CHECK(archived(storage_class::deep_archive));
    CHECK(!archived(storage_class::glacier_ir));
    CHECK_THROWS_AS(storage_class_from("cold"), std::invalid_argument);
    CHECK(restore_tier_from("BULK") == restore_tier::bulk);
    CHECK_EQ(std::string{to_string(restore_tier::expedited)}, std::string{"Expedited"});

    SECTION("times and restore status");
    CHECK(parse_time("Fri, 21 Dec 2012 00:00:00 GMT") == at(2012, 12, 21, 0, 0, 0));
    CHECK(parse_time("2012-12-21T13:14:15.000Z") == at(2012, 12, 21, 13, 14, 15));
    CHECK_THROWS_AS(parse_time("yesterday"), std::invalid_argument);
    {
        const auto ongoing = restore_status::parse(R"(ongoing-request="true")");
        CHECK(ongoing.requested && ongoing.ongoing && !ongoing.available());
        const auto done = restore_status::parse(R"(ongoing-request="false", expiry-date="Fri, 21 Dec 2012 00:00:00 GMT")");
        CHECK(done.available());
        CHECK(done.expiry && *done.expiry == at(2012, 12, 21, 0, 0, 0));
        CHECK(!restore_status::parse("").requested);
    }

    SECTION("object_info from headers");
    {
        const auto info = object_info::from_headers("a/b", {{"content-length", "10"}, {"content-range", "bytes 0-9/443"}, {"etag", "\"abc-3\""},
                                                            {"x-amz-storage-class", "DEEP_ARCHIVE"}, {"x-amz-meta-host", "hv2"},
                                                            {"x-amz-checksum-crc32", "y/Q5Jg=="}, {"x-amz-restore", R"(ongoing-request="true")"},
                                                            {"last-modified", "Fri, 21 Dec 2012 00:00:00 GMT"}});
        CHECK_EQ(info.size.count(), 443ULL);
        CHECK_EQ(info.etag, std::string{"abc-3"});
        CHECK(info.kind == storage_class::deep_archive);
        CHECK_EQ(info.metadata.at("host"), std::string{"hv2"});
        CHECK_EQ(info.checksums.at(checksum::crc32), std::string{"y/Q5Jg=="});
        CHECK(info.restore.ongoing);
        CHECK(info.last_modified == at(2012, 12, 21, 0, 0, 0));
    }

    SECTION("ListObjectsV2");
    {
        const auto page = list_page::parse(R"(<?xml version="1.0" encoding="UTF-8"?>
<ListBucketResult xmlns="http://s3.amazonaws.com/doc/2006-03-01/"><Name>b</Name><Prefix>t</Prefix><KeyCount>2</KeyCount>
<MaxKeys>2</MaxKeys><EncodingType>url</EncodingType><IsTruncated>true</IsTruncated>
<Contents><Key>test/a+b%26c.txt</Key><LastModified>2026-11-23T08:00:00.000Z</LastModified><ETag>&quot;e1&quot;</ETag><Size>12</Size><StorageClass>DEEP_ARCHIVE</StorageClass></Contents>
<Contents><Key>test/x</Key><LastModified>2026-11-23T08:00:01.000Z</LastModified><ETag>"e2-4"</ETag><Size>34359738368</Size><StorageClass>STANDARD</StorageClass></Contents>
<CommonPrefixes><Prefix>test/sub/</Prefix></CommonPrefixes>
<NextContinuationToken>1ueGcxLPRx1Tr/XYExHnhbYLgveDs2J/wm36Hy4vbOwM=</NextContinuationToken></ListBucketResult>)");
        CHECK_EQ(page.objects.size(), std::size_t{2});
        CHECK_EQ(page.objects[0].key, std::string{"test/a b&c.txt"});
        CHECK_EQ(page.objects[0].etag, std::string{"e1"});
        CHECK(page.objects[0].kind == storage_class::deep_archive);
        CHECK(page.objects[1].size == GiB(32));
        CHECK_EQ(page.objects[1].etag, std::string{"e2-4"});
        CHECK_EQ(page.prefixes.size(), std::size_t{1});
        CHECK(page.truncated);
        CHECK_EQ(page.next_token, std::string{"1ueGcxLPRx1Tr/XYExHnhbYLgveDs2J/wm36Hy4vbOwM="});
    }

    SECTION("GetObjectAttributes");
    {
        bool truncated = true;
        int next = 0;
        const auto attributes = object_attributes::parse(R"(<GetObjectAttributesResponse xmlns="http://s3.amazonaws.com/doc/2006-03-01/">
<ETag>"abc-3"</ETag><Checksum><ChecksumCRC32>LAQfEg==-3</ChecksumCRC32><ChecksumType>COMPOSITE</ChecksumType></Checksum>
<ObjectParts><IsTruncated>false</IsTruncated><MaxParts>1000</MaxParts><NextPartNumberMarker>3</NextPartNumberMarker><PartNumberMarker>0</PartNumberMarker>
<Part><ChecksumCRC32>AAAAAQ==</ChecksumCRC32><PartNumber>1</PartNumber><Size>5242880</Size></Part>
<Part><ChecksumCRC32>AAAAAg==</ChecksumCRC32><PartNumber>2</PartNumber><Size>5242880</Size></Part>
<Part><ChecksumCRC32>AAAAAw==</ChecksumCRC32><PartNumber>3</PartNumber><Size>1514240</Size></Part>
<PartsCount>3</PartsCount></ObjectParts><StorageClass>DEEP_ARCHIVE</StorageClass><ObjectSize>12000000</ObjectSize></GetObjectAttributesResponse>)",
                                                         &truncated, &next);
        CHECK(!truncated);
        CHECK_EQ(next, 3);
        CHECK_EQ(attributes.etag, std::string{"abc-3"});
        CHECK(attributes.size == bytes{12000000});
        CHECK(attributes.kind == storage_class::deep_archive);
        CHECK_EQ(attributes.checksums.at(checksum::crc32), std::string{"LAQfEg=="}); // without "-3"
        CHECK_EQ(attributes.parts_count, 3);
        CHECK_EQ(attributes.parts.size(), std::size_t{3});
        CHECK_EQ(attributes.parts[1].checksums.at(checksum::crc32), std::string{"AAAAAg=="});
        CHECK((attributes.part_range(1) == std::pair<std::uint64_t, std::uint64_t>{0, 5242879}));
        CHECK((attributes.part_range(3) == std::pair<std::uint64_t, std::uint64_t>{10485760, 11999999}));
        CHECK_THROWS_AS((void)attributes.part_range(4), std::invalid_argument);
        // A composite of the raw part checksums 00000001 00000002 00000003.
        CHECK_EQ(composite_checksum(checksum::crc32, {std::string("\0\0\0\1", 4), std::string("\0\0\0\2", 4), std::string("\0\0\0\3", 4)}),
                 crc{checksum::crc32}.update(std::string("\0\0\0\1\0\0\0\2\0\0\0\3", 12)).base64());
    }

    SECTION("AssumeRole's answer");
    {
        const auto key = parse_assume_role(R"(<AssumeRoleResponse xmlns="https://sts.amazonaws.com/doc/2011-06-15/"><AssumeRoleResult>
<AssumedRoleUser><Arn>arn:aws:sts::123456789012:assumed-role/upload/s</Arn></AssumedRoleUser>
<Credentials><AccessKeyId>ASIAEXAMPLE</AccessKeyId><SecretAccessKey>secret</SecretAccessKey><SessionToken>token</SessionToken>
<Expiration>2020-11-23T12:00:00Z</Expiration></Credentials></AssumeRoleResult></AssumeRoleResponse>)");
        CHECK_EQ(key.access_key_id, std::string{"ASIAEXAMPLE"});
        CHECK_EQ(key.session_token, std::string{"token"});
        CHECK(key.expiration && *key.expiration == at(2020, 11, 23, 12, 0, 0));
        CHECK(key.expired());
        CHECK_THROWS_AS((void)parse_assume_role("<AssumeRoleResponse></AssumeRoleResponse>"), std::runtime_error);
    }

    SECTION("settings");
    {
        const auto folder = std::filesystem::temp_directory_path() / ("sc-s3-test-" + std::to_string(::getpid()));
        std::filesystem::create_directories(folder / "s3.conf.d");
        std::ofstream{folder / "s3.conf"} << "region = af-south-1\npath_style = true\nrate_limit = 10M\naccess_key_id = AKIAFILE\nsecret_access_key = filesecret\n";
        std::ofstream{folder / "s3.conf.d" / "50-local.conf"} << "[s3]\nendpoint = http://127.0.0.1:9000/\nretries = 2\n";
        std::filesystem::permissions(folder / "s3.conf", std::filesystem::perms::owner_read | std::filesystem::perms::owner_write);
        const auto loaded = settings::load(folder / "s3.conf", false);
        CHECK_EQ(loaded.region, std::string{"af-south-1"});
        CHECK(loaded.path_style);
        CHECK(loaded.rate_limit == MiB(10));
        CHECK_EQ(loaded.retries, 2);
        CHECK_EQ(loaded.s3_url(), std::string{"http://127.0.0.1:9000"});
        CHECK_EQ(loaded.credentials.access_key_id, std::string{"AKIAFILE"});

        setenv("AWS_ACCESS_KEY_ID", "AKIAENV", 1);
        setenv("AWS_SECRET_ACCESS_KEY", "envsecret", 1);
        setenv("AWS_REGION", "eu-west-1", 1);
        const auto overridden = settings::load(folder / "s3.conf");
        CHECK_EQ(overridden.credentials.access_key_id, std::string{"AKIAENV"});
        CHECK_EQ(overridden.credentials.secret_access_key, std::string{"envsecret"});
        CHECK_EQ(overridden.region, std::string{"eu-west-1"});
        unsetenv("AWS_ACCESS_KEY_ID");
        unsetenv("AWS_SECRET_ACCESS_KEY");
        unsetenv("AWS_REGION");

        std::ofstream{folder / "bad.conf"} << "colour = blue\n";
        CHECK_THROWS_AS(settings::load(folder / "bad.conf", false), std::runtime_error);
        CHECK_THROWS_AS(settings::load(folder / "missing.conf", false), std::runtime_error);
        std::filesystem::remove_all(folder);

        settings aws;
        aws.region = "af-south-1";
        CHECK_EQ(aws.s3_url(), std::string{"https://s3.af-south-1.amazonaws.com"});
        CHECK_EQ(aws.sts_url(), std::string{"https://sts.af-south-1.amazonaws.com"});
    }

    SECTION("s3:// URLs");
    CHECK((parse_url("s3://bucket/a/b.txt") == std::pair<std::string, std::string>{"bucket", "a/b.txt"}));
    CHECK((parse_url("s3://bucket") == std::pair<std::string, std::string>{"bucket", ""}));
    CHECK_THROWS_AS(parse_url("https://bucket/key"), std::invalid_argument);
    CHECK_THROWS_AS(parse_url("s3:///key"), std::invalid_argument);

    SECTION("XML");
    {
        const auto root = xml::parse(R"(<?xml version="1.0"?><a:Root xmlns:a="x"><Item n="1">one &amp; <![CDATA[<two>]]></Item><Item>3</Item></a:Root>)");
        CHECK_EQ(root.name, std::string{"Root"});
        CHECK_EQ(root.all("Item").size(), std::size_t{2});
        CHECK_EQ(root.value("Item"), std::string{"one & <two>"});
        CHECK_EQ(xml::escape("<a & 'b'>"), std::string{"&lt;a &amp; &apos;b&apos;&gt;"});
        CHECK_THROWS_AS((void)xml::parse("<a><b></a>"), std::runtime_error);
    }

    TEST_SUMMARY();
}
