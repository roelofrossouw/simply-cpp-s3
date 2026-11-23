#include <sc/s3/xml.h>

#include <cstdint>
#include <stdexcept>

namespace sc::s3::xml {
    namespace {
        void append_utf8(std::string &out, const std::uint32_t code) {
            if (code < 0x80) {
                out += static_cast<char>(code);
            } else if (code < 0x800) {
                out += static_cast<char>(0xC0 | code >> 6);
                out += static_cast<char>(0x80 | (code & 0x3F));
            } else if (code < 0x10000) {
                out += static_cast<char>(0xE0 | code >> 12);
                out += static_cast<char>(0x80 | (code >> 6 & 0x3F));
                out += static_cast<char>(0x80 | (code & 0x3F));
            } else {
                out += static_cast<char>(0xF0 | code >> 18);
                out += static_cast<char>(0x80 | (code >> 12 & 0x3F));
                out += static_cast<char>(0x80 | (code >> 6 & 0x3F));
                out += static_cast<char>(0x80 | (code & 0x3F));
            }
        }

        // Text with its entities decoded; an unknown one is kept as written.
        std::string decode(const std::string_view text) {
            std::string out;
            out.reserve(text.size());
            for (std::size_t i = 0; i < text.size(); ++i) {
                if (text[i] != '&') {
                    out += text[i];
                    continue;
                }
                const auto end = text.find(';', i);
                if (end == std::string_view::npos || end - i > 12) {
                    out += text[i];
                    continue;
                }
                const auto name = text.substr(i + 1, end - i - 1);
                if (name == "amp") out += '&';
                else if (name == "lt") out += '<';
                else if (name == "gt") out += '>';
                else if (name == "quot") out += '"';
                else if (name == "apos") out += '\'';
                else if (name.size() > 1 && name[0] == '#') {
                    std::uint32_t code = 0;
                    bool ok = true;
                    if (name[1] == 'x' || name[1] == 'X') {
                        for (const char c: name.substr(2)) {
                            code *= 16;
                            if (c >= '0' && c <= '9') code += static_cast<std::uint32_t>(c - '0');
                            else if (c >= 'a' && c <= 'f') code += static_cast<std::uint32_t>(c - 'a' + 10);
                            else if (c >= 'A' && c <= 'F') code += static_cast<std::uint32_t>(c - 'A' + 10);
                            else ok = false;
                        }
                    } else {
                        for (const char c: name.substr(1)) {
                            if (c < '0' || c > '9') ok = false;
                            code = code * 10 + static_cast<std::uint32_t>(c - '0');
                        }
                    }
                    if (!ok || code > 0x10FFFF) {
                        out.append(text.substr(i, end - i + 1));
                    } else {
                        append_utf8(out, code);
                    }
                } else {
                    out.append(text.substr(i, end - i + 1));
                }
                i = end;
            }
            return out;
        }

        std::string local_name(std::string_view name) {
            if (const auto colon = name.find(':'); colon != std::string_view::npos) name.remove_prefix(colon + 1);
            return std::string{name};
        }

        class parser {
        public:
            explicit parser(const std::string_view text) : text_(text) {}

            element document() {
                skip_misc();
                if (pos_ >= text_.size() || text_[pos_] != '<') throw std::runtime_error("XML: no root element");
                auto root = parse_element();
                return root;
            }

        private:
            std::string_view text_;
            std::size_t pos_ = 0;

            [[noreturn]] void fail(const std::string &what) const {
                throw std::runtime_error("XML: " + what + " at offset " + std::to_string(pos_));
            }

            void skip_until(const std::string_view end) {
                const auto at = text_.find(end, pos_);
                if (at == std::string_view::npos) fail("unterminated " + std::string{end});
                pos_ = at + end.size();
            }

            // Whitespace, <?...?>, <!--...-->, <!DOCTYPE ...>.
            void skip_misc() {
                while (pos_ < text_.size()) {
                    if (std::string_view{" \t\r\n"}.find(text_[pos_]) != std::string_view::npos) {
                        ++pos_;
                    } else if (text_.substr(pos_, 2) == "<?") {
                        skip_until("?>");
                    } else if (text_.substr(pos_, 4) == "<!--") {
                        skip_until("-->");
                    } else if (text_.substr(pos_, 2) == "<!" && text_.substr(pos_, 9) != "<![CDATA[") {
                        skip_until(">");
                    } else {
                        break;
                    }
                }
            }

            element parse_element() {
                ++pos_; // '<'
                const auto name_start = pos_;
                while (pos_ < text_.size() && std::string_view{" \t\r\n/>"}.find(text_[pos_]) == std::string_view::npos) ++pos_;
                const auto qualified = text_.substr(name_start, pos_ - name_start);
                if (qualified.empty()) fail("empty tag name");
                element node;
                node.name = local_name(qualified);
                // Attributes, skipped (a quoted value may hold '>').
                while (pos_ < text_.size() && text_[pos_] != '>' && text_.substr(pos_, 2) != "/>") {
                    if (text_[pos_] == '"' || text_[pos_] == '\'') {
                        const auto quote = text_[pos_];
                        const auto end = text_.find(quote, pos_ + 1);
                        if (end == std::string_view::npos) fail("unterminated attribute value");
                        pos_ = end + 1;
                    } else {
                        ++pos_;
                    }
                }
                if (pos_ >= text_.size()) fail("unterminated tag <" + std::string{qualified} + ">");
                if (text_[pos_] == '/') {
                    pos_ += 2;
                    return node;
                }
                ++pos_; // '>'
                std::string raw_text;
                while (true) {
                    if (pos_ >= text_.size()) fail("<" + std::string{qualified} + "> not closed");
                    if (text_[pos_] != '<') {
                        const auto next = text_.find('<', pos_);
                        const auto end = next == std::string_view::npos ? text_.size() : next;
                        node.text += decode(text_.substr(pos_, end - pos_));
                        pos_ = end;
                    } else if (text_.substr(pos_, 9) == "<![CDATA[") {
                        const auto end = text_.find("]]>", pos_ + 9);
                        if (end == std::string_view::npos) fail("unterminated CDATA");
                        node.text.append(text_.substr(pos_ + 9, end - pos_ - 9));
                        pos_ = end + 3;
                    } else if (text_.substr(pos_, 4) == "<!--") {
                        skip_until("-->");
                    } else if (text_.substr(pos_, 2) == "<?") {
                        skip_until("?>");
                    } else if (text_.substr(pos_, 2) == "</") {
                        pos_ += 2;
                        const auto end = text_.find('>', pos_);
                        if (end == std::string_view::npos) fail("unterminated closing tag");
                        auto closing = text_.substr(pos_, end - pos_);
                        while (!closing.empty() && std::string_view{" \t\r\n"}.find(closing.back()) != std::string_view::npos) closing.remove_suffix(1);
                        if (closing != qualified) fail("</" + std::string{closing} + "> closes <" + std::string{qualified} + ">");
                        pos_ = end + 1;
                        break;
                    } else {
                        node.children.push_back(parse_element());
                    }
                }
                // Whitespace between child elements isn't text.
                if (!node.children.empty() && node.text.find_first_not_of(" \t\r\n") == std::string::npos) node.text.clear();
                return node;
            }
        };
    }

    const element *element::child(const std::string_view child_name) const {
        for (const auto &c: children) {
            if (c.name == child_name) return &c;
        }
        return nullptr;
    }

    std::string element::value(const std::string_view child_name, const std::string_view fallback) const {
        if (const auto *c = child(child_name)) return c->text;
        return std::string{fallback};
    }

    std::vector<const element *> element::all(const std::string_view child_name) const {
        std::vector<const element *> found;
        for (const auto &c: children) {
            if (c.name == child_name) found.push_back(&c);
        }
        return found;
    }

    const element *element::find(std::string_view path) const {
        const element *at = this;
        while (at && !path.empty()) {
            const auto slash = path.find('/');
            at = at->child(path.substr(0, slash));
            path = slash == std::string_view::npos ? std::string_view{} : path.substr(slash + 1);
        }
        return at;
    }

    element parse(const std::string_view document) { return parser{document}.document(); }

    std::string escape(const std::string_view text) {
        std::string out;
        out.reserve(text.size());
        for (const char c: text) {
            switch (c) {
                case '&': out += "&amp;"; break;
                case '<': out += "&lt;"; break;
                case '>': out += "&gt;"; break;
                case '"': out += "&quot;"; break;
                case '\'': out += "&apos;"; break;
                default: out += c;
            }
        }
        return out;
    }
}
