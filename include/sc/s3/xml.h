#pragma once

#include <string>
#include <string_view>
#include <vector>

namespace sc::s3::xml {
    /// An element of the small XML documents S3 and STS answer with: its name (namespace prefix
    /// dropped), its text (entities and CDATA decoded, children's text not included) and its
    /// children in order. Attributes, comments and processing instructions are skipped.
    struct element {
        std::string name;
        std::string text;
        std::vector<element> children;

        /// The first child of that name, or nullptr.
        [[nodiscard]] const element *child(std::string_view child_name) const;
        /// The first child's text, or fallback when there is no such child.
        [[nodiscard]] std::string value(std::string_view child_name, std::string_view fallback = {}) const;
        /// Every child of that name, in order.
        [[nodiscard]] std::vector<const element *> all(std::string_view child_name) const;
        /// The element at a path of names below this one ("Credentials/AccessKeyId"), or nullptr.
        [[nodiscard]] const element *find(std::string_view path) const;
    };

    /// The document's root element. Throws std::runtime_error for text that isn't well-formed
    /// enough to read (an unclosed or mismatched tag).
    [[nodiscard]] element parse(std::string_view document);

    /// text with & < > " ' written as entities.
    [[nodiscard]] std::string escape(std::string_view text);
}
