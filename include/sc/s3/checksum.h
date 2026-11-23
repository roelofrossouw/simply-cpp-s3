#pragma once

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace sc::s3 {
    /// The checksums S3 keeps with an object, sent as x-amz-checksum-<name> (base64, the bytes in
    /// network order) and checked by S3 before it stores anything. none still has every request's
    /// body checked: SigV4 signs its SHA-256.
    enum class checksum { none, crc32, crc32c, sha256 };

    /// "CRC32", "CRC32C", "SHA256" (S3's x-amz-checksum-algorithm), "" for none.
    [[nodiscard]] std::string_view to_string(checksum algorithm);
    /// From "crc32", "CRC32C", "sha256" or "none", in any case; throws std::invalid_argument otherwise.
    [[nodiscard]] checksum checksum_from(std::string_view name);

    /// SHA-256 of data: its 32 bytes.
    [[nodiscard]] std::string sha256(std::string_view data);
    /// HMAC-SHA256 of data with key: 32 bytes.
    [[nodiscard]] std::string hmac_sha256(std::string_view key, std::string_view data);
    /// MD5 of data: 16 bytes (Content-MD5 is its base64).
    [[nodiscard]] std::string md5(std::string_view data);
    /// bytes as lowercase hex, two characters a byte.
    [[nodiscard]] std::string hex(std::string_view bytes);

    /// A CRC-32 (IEEE, as zlib and S3's CRC32) or CRC-32C (Castagnoli, S3's CRC32C) of data given
    /// in pieces.
    ///
    ///     sc::s3::crc crc{sc::s3::checksum::crc32};
    ///     crc.update("1234"); crc.update("56789");
    ///     crc.value();   // 0xCBF43926
    ///     crc.base64();  // "y/Q5Jg==", as S3 shows it
    class crc {
    public:
        /// algorithm: crc32 or crc32c (anything else throws std::invalid_argument).
        explicit crc(checksum algorithm = checksum::crc32);

        crc &update(std::string_view data);

        [[nodiscard]] std::uint32_t value() const { return ~state_; }
        /// The value's 4 bytes, most significant first.
        [[nodiscard]] std::string bytes() const;
        /// bytes() in base64, as x-amz-checksum-crc32 carries it.
        [[nodiscard]] std::string base64() const;

    private:
        const std::uint32_t *table_;
        std::uint32_t state_ = 0xFFFFFFFFU;
    };

    /// The checksum of data as S3 carries it (base64 of the raw value); "" for none.
    [[nodiscard]] std::string checksum_of(checksum algorithm, std::string_view data);
    /// The raw value (4 or 32 bytes) of data; "" for none.
    [[nodiscard]] std::string raw_checksum_of(checksum algorithm, std::string_view data);
    /// A multipart upload's checksum as S3 computes it: the checksum of the parts' raw checksums one after the
    /// other, in base64 ("...=="; S3 shows it with "-<parts>" after it). parts: each part's raw checksum.
    [[nodiscard]] std::string composite_checksum(checksum algorithm, const std::vector<std::string> &parts);
}
