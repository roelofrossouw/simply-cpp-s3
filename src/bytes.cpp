#include <sc/s3/bytes.h>

#include <cctype>
#include <charconv>
#include <stdexcept>
#include <string>

namespace sc::s3 {
    bytes bytes::parse(std::string_view text) {
        const std::string original{text};
        while (!text.empty() && std::isspace(static_cast<unsigned char>(text.front()))) text.remove_prefix(1);
        while (!text.empty() && std::isspace(static_cast<unsigned char>(text.back()))) text.remove_suffix(1);
        std::size_t digits = 0;
        while (digits < text.size() && (std::isdigit(static_cast<unsigned char>(text[digits])) || text[digits] == '.')) ++digits;
        if (digits == 0) throw std::invalid_argument("not a size: \"" + original + "\" (a number, optionally with K, M, G, T, KiB ... or kB ...)");
        double amount = 0;
        const auto number = text.substr(0, digits);
        if (const auto [end, problem] = std::from_chars(number.data(), number.data() + number.size(), amount);
            problem != std::errc{} || end != number.data() + number.size() || amount < 0) {
            throw std::invalid_argument("not a size: \"" + original + "\"");
        }
        auto unit = std::string{text.substr(digits)};
        while (!unit.empty() && std::isspace(static_cast<unsigned char>(unit.front()))) unit.erase(0, 1);

        double per = 0;
        if (unit.empty() || unit == "B" || unit == "b") {
            per = 1;
        } else {
            const char prefix = static_cast<char>(std::toupper(static_cast<unsigned char>(unit.front())));
            const auto rest = unit.substr(1);
            int power = 0;
            switch (prefix) {
                case 'K': power = 1; break;
                case 'M': power = 2; break;
                case 'G': power = 3; break;
                case 'T': power = 4; break;
                default: break;
            }
            if (power == 0) throw std::invalid_argument("not a size: \"" + original + "\" (unknown unit " + unit + ")");
            double base = 0;
            if (rest.empty() || rest == "iB" || rest == "ib" || rest == "IB") base = 1024;   // K, KiB: binary
            else if (rest == "B") base = 1000;                                                 // kB, MB: decimal
            else throw std::invalid_argument("not a size: \"" + original + "\" (unknown unit " + unit + ")");
            per = 1;
            for (int i = 0; i < power; ++i) per *= base;
        }
        const double total = amount * per;
        if (total > 1.8e19) throw std::invalid_argument("size too large: \"" + original + "\"");
        return bytes{static_cast<std::uint64_t>(total + 0.5)};
    }
}
