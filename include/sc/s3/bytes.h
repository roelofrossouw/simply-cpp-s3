#pragma once

#include <cmath>
#include <compare>
#include <concepts>
#include <cstdint>
#include <ostream>
#include <string_view>

namespace sc::s3 {
    /// A number of bytes: an object's size, a part's, a rate's per second. The units in sc::s3::data
    /// (data::KiB, data::MiB, data::GiB, data::TiB, and the decimal data::kB, data::MB, data::GB,
    /// data::TB) are sizes given in their own unit; a plain number is bytes.
    ///
    ///     using namespace sc::s3::data;
    ///     sc::s3::bytes part = MiB(64);                 // 67108864 bytes
    ///     std::cout << part.in<GiB>();                 // "0.0625GiB"
    ///     if (object.size > GiB(5)) ...
    ///
    /// count() reads it back, << writes it as bytes ("67108864B") and a unit in its own ("64MiB").
    class bytes {
    public:
        constexpr bytes() = default;

        constexpr explicit bytes(const std::uint64_t count) : count_(count) {}

        [[nodiscard]] constexpr std::uint64_t count() const { return count_; }

        /// The same size in a unit: size.in<sc::s3::data::MiB>().
        template<typename Unit>
            requires std::derived_from<Unit, bytes>
        [[nodiscard]] constexpr Unit in() const {
            return Unit(*this);
        }

        constexpr auto operator<=>(const bytes &) const = default;

        constexpr bytes operator+(const bytes other) const { return bytes{count_ + other.count_}; }
        constexpr bytes operator-(const bytes other) const { return bytes{count_ > other.count_ ? count_ - other.count_ : 0}; }
        constexpr bytes operator*(const std::uint64_t times) const { return bytes{count_ * times}; }
        constexpr bytes &operator+=(const bytes other) {
            count_ += other.count_;
            return *this;
        }

        /// Reads "64M", "64MiB", "1.5G", "100kB" or "4096": K, M, G and T alone (any case) and KiB,
        /// MiB ... are binary (1024s, as curl and dd count), kB, MB, GB and TB decimal, B or nothing
        /// bytes. Throws std::invalid_argument for anything else.
        static bytes parse(std::string_view text);

    private:
        std::uint64_t count_ = 0;
    };

    inline std::ostream &operator<<(std::ostream &out, const bytes &size) { return out << size.count() << 'B'; }

    /// The units a size can be given in, each a size converting its own unit to bytes.
    namespace data {
        /// A unit of per bytes, written with suffix.
        template<std::uint64_t Per, const std::string_view &Suffix>
        class unit : public bytes {
        public:
            static constexpr std::uint64_t per = Per;

            constexpr unit() = default;

            /// amount of this unit, rounded to whole bytes.
            constexpr explicit unit(const double amount) : bytes(static_cast<std::uint64_t>(std::llround(amount * static_cast<double>(Per)))) {}

            /// The same size, in this unit: static_cast<sc::s3::data::MiB>(size).
            constexpr explicit unit(const bytes &size) : bytes(size) {}

            /// How many of this unit it is (64 for MiB(64)).
            [[nodiscard]] constexpr double value() const { return static_cast<double>(count()) / static_cast<double>(Per); }

            friend std::ostream &operator<<(std::ostream &out, const unit &size) { return out << size.value() << Suffix; }
        };

        inline constexpr std::string_view kib_suffix = "KiB", mib_suffix = "MiB", gib_suffix = "GiB", tib_suffix = "TiB";
        inline constexpr std::string_view kb_suffix = "kB", mb_suffix = "MB", gb_suffix = "GB", tb_suffix = "TB";

        using KiB = unit<1024ULL, kib_suffix>;
        using MiB = unit<1024ULL * 1024, mib_suffix>;
        using GiB = unit<1024ULL * 1024 * 1024, gib_suffix>;
        using TiB = unit<1024ULL * 1024 * 1024 * 1024, tib_suffix>;
        using kB = unit<1000ULL, kb_suffix>;
        using MB = unit<1000ULL * 1000, mb_suffix>;
        using GB = unit<1000ULL * 1000 * 1000, gb_suffix>;
        using TB = unit<1000ULL * 1000 * 1000 * 1000, tb_suffix>;
    }
}
