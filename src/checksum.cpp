#include <sc/s3/checksum.h>

#include <sc/core/base64.h>

#include <openssl/evp.h>
#include <openssl/hmac.h>

#include <array>
#include <cctype>
#include <memory>
#include <stdexcept>
#include <string>

namespace sc::s3 {
    namespace {
        // Reflected CRC-32 tables: IEEE (0xEDB88320) and Castagnoli (0x82F63B78).
        constexpr std::array<std::uint32_t, 256> crc_table(const std::uint32_t polynomial) {
            std::array<std::uint32_t, 256> table{};
            for (std::uint32_t i = 0; i < 256; ++i) {
                std::uint32_t value = i;
                for (int bit = 0; bit < 8; ++bit) value = value & 1U ? (value >> 1) ^ polynomial : value >> 1;
                table[i] = value;
            }
            return table;
        }

        constexpr auto ieee_table = crc_table(0xEDB88320U);
        constexpr auto castagnoli_table = crc_table(0x82F63B78U);

        std::string digest(const EVP_MD *algorithm, const std::string_view data) {
            std::unique_ptr<EVP_MD_CTX, decltype(&EVP_MD_CTX_free)> context{EVP_MD_CTX_new(), EVP_MD_CTX_free};
            unsigned char out[EVP_MAX_MD_SIZE];
            unsigned int length = 0;
            if (!context || EVP_DigestInit_ex(context.get(), algorithm, nullptr) != 1 ||
                EVP_DigestUpdate(context.get(), data.data(), data.size()) != 1 || EVP_DigestFinal_ex(context.get(), out, &length) != 1) {
                throw std::runtime_error("OpenSSL couldn't compute a digest");
            }
            return {reinterpret_cast<const char *>(out), length};
        }

        std::string upper(std::string_view text) {
            std::string out{text};
            for (auto &c: out) c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
            return out;
        }
    }

    std::string_view to_string(const checksum algorithm) {
        switch (algorithm) {
            case checksum::crc32: return "CRC32";
            case checksum::crc32c: return "CRC32C";
            case checksum::sha256: return "SHA256";
            case checksum::none: break;
        }
        return "";
    }

    checksum checksum_from(const std::string_view name) {
        const auto wanted = upper(name);
        if (wanted == "CRC32") return checksum::crc32;
        if (wanted == "CRC32C") return checksum::crc32c;
        if (wanted == "SHA256" || wanted == "SHA-256") return checksum::sha256;
        if (wanted == "NONE" || wanted.empty()) return checksum::none;
        throw std::invalid_argument("unknown checksum \"" + std::string{name} + "\" (crc32, crc32c, sha256 or none)");
    }

    std::string sha256(const std::string_view data) { return digest(EVP_sha256(), data); }

    std::string md5(const std::string_view data) { return digest(EVP_md5(), data); }

    std::string hmac_sha256(const std::string_view key, const std::string_view data) {
        unsigned char out[EVP_MAX_MD_SIZE];
        unsigned int length = 0;
        if (!HMAC(EVP_sha256(), key.data(), static_cast<int>(key.size()), reinterpret_cast<const unsigned char *>(data.data()), data.size(), out,
                  &length)) {
            throw std::runtime_error("OpenSSL couldn't compute an HMAC");
        }
        return {reinterpret_cast<const char *>(out), length};
    }

    std::string hex(const std::string_view bytes) {
        static constexpr char digits[] = "0123456789abcdef";
        std::string out;
        out.reserve(bytes.size() * 2);
        for (const unsigned char c: bytes) {
            out += digits[c >> 4];
            out += digits[c & 0x0F];
        }
        return out;
    }

    crc::crc(const checksum algorithm) {
        if (algorithm == checksum::crc32) table_ = ieee_table.data();
        else if (algorithm == checksum::crc32c) table_ = castagnoli_table.data();
        else throw std::invalid_argument("sc::s3::crc is CRC32 or CRC32C");
    }

    crc &crc::update(const std::string_view data) {
        auto state = state_;
        for (const unsigned char c: data) state = table_[(state ^ c) & 0xFFU] ^ (state >> 8);
        state_ = state;
        return *this;
    }

    std::string crc::bytes() const {
        const auto v = value();
        return {static_cast<char>(v >> 24), static_cast<char>(v >> 16 & 0xFF), static_cast<char>(v >> 8 & 0xFF), static_cast<char>(v & 0xFF)};
    }

    std::string crc::base64() const { return sc::base64::encode(bytes()); }

    std::string raw_checksum_of(const checksum algorithm, const std::string_view data) {
        switch (algorithm) {
            case checksum::crc32:
            case checksum::crc32c: return crc{algorithm}.update(data).bytes();
            case checksum::sha256: return sha256(data);
            case checksum::none: break;
        }
        return {};
    }

    std::string checksum_of(const checksum algorithm, const std::string_view data) {
        if (algorithm == checksum::none) return {};
        return sc::base64::encode(raw_checksum_of(algorithm, data));
    }
}
