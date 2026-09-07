// Error types for XML parsing, XPath evaluation, and index persistence.
//
// Ported from Rust `error.rs` (thiserror-based enum) to C++23 std::expected.
// The Rust enum uses `thiserror::Error` derives; in C++ we represent the
// discriminated union as an enum class carrying a payload via a struct,
// and provide a `std::expected<T, SimdXmlError>` alias.
#pragma once

#include <cstdint>
#include <expected>
#include <format>
#include <ostream>
#include <string>
#include <string_view>
#include <variant>

namespace simdxml {

/// All errors produced by simdxml.
///
/// Mirrors the Rust `SimdXmlError` enum. Each variant carries either a byte
/// offset, a free-form message string, or both. Variants are discriminated
/// by the `Kind` enum; payloads are stored in the `payload_` string and
/// `offset_` integer fields.
class SimdXmlError {
public:
    /// Discriminator for the error variant.
    enum class Kind {
        ParseError,        ///< Malformed markup at a byte offset.
        XPathParseError,   ///< XPath expression failed to parse.
        XPathEvalError,    ///< Runtime XPath evaluation failure.
        UnclosedTag,       ///< `<` without matching `>`.
        MismatchedCloseTag, ///< `</name>` does not match open tag.
        InvalidXml,        ///< Input is not valid XML.
        Io,                ///< I/O error reading files.
        InvalidSxi,        ///< `.sxi` index file corrupt or incompatible.
        StaleSxi,          ///< `.sxi` index is stale; XML changed.
    };

    SimdXmlError() = default;

    /// Build a ParseError variant.
    static SimdXmlError parse_error(std::size_t offset, std::string message) {
        SimdXmlError e;
        e.kind_ = Kind::ParseError;
        e.offset_ = offset;
        e.payload_ = std::move(message);
        return e;
    }

    /// Build an XPathParseError variant.
    static SimdXmlError xpath_parse_error(std::string message) {
        SimdXmlError e;
        e.kind_ = Kind::XPathParseError;
        e.payload_ = std::move(message);
        return e;
    }

    /// Build an XPathEvalError variant.
    static SimdXmlError xpath_eval_error(std::string message) {
        SimdXmlError e;
        e.kind_ = Kind::XPathEvalError;
        e.payload_ = std::move(message);
        return e;
    }

    /// Build an UnclosedTag variant. `offset` points to the `<`.
    static SimdXmlError unclosed_tag(std::size_t offset) {
        SimdXmlError e;
        e.kind_ = Kind::UnclosedTag;
        e.offset_ = offset;
        return e;
    }

    /// Build a MismatchedCloseTag variant.
    static SimdXmlError mismatched_close_tag(std::string expected,
                                             std::string found,
                                             std::size_t offset) {
        SimdXmlError e;
        e.kind_ = Kind::MismatchedCloseTag;
        e.payload_ = std::move(expected);
        e.aux_ = std::move(found);
        e.offset_ = offset;
        return e;
    }

    /// Build an InvalidXml variant.
    static SimdXmlError invalid_xml(std::string message) {
        SimdXmlError e;
        e.kind_ = Kind::InvalidXml;
        e.payload_ = std::move(message);
        return e;
    }

    /// Build an Io variant wrapping a system error message.
    static SimdXmlError io(std::string message) {
        SimdXmlError e;
        e.kind_ = Kind::Io;
        e.payload_ = std::move(message);
        return e;
    }

    /// Build an InvalidSxi variant.
    static SimdXmlError invalid_sxi(std::string message) {
        SimdXmlError e;
        e.kind_ = Kind::InvalidSxi;
        e.payload_ = std::move(message);
        return e;
    }

    /// Build a StaleSxi variant.
    static SimdXmlError stale_sxi() {
        SimdXmlError e;
        e.kind_ = Kind::StaleSxi;
        return e;
    }

    Kind kind() const noexcept { return kind_; }
    std::size_t offset() const noexcept { return offset_; }
    std::string_view message() const noexcept { return payload_; }
    std::string_view expected_name() const noexcept { return payload_; }
    std::string_view found_name() const noexcept { return aux_; }

    /// Format the error as a human-readable string, mirroring the Rust
    /// `#[error("...")]` format strings.
    [[nodiscard]] std::string to_string() const {
        switch (kind_) {
            case Kind::ParseError:
                return std::format("XML parse error at byte {}: {}",
                                   offset_, payload_);
            case Kind::XPathParseError:
                return std::format("XPath parse error: {}", payload_);
            case Kind::XPathEvalError:
                return std::format("XPath evaluation error: {}", payload_);
            case Kind::UnclosedTag:
                return std::format("Unclosed tag at byte {}", offset_);
            case Kind::MismatchedCloseTag:
                return std::format(
                    "Mismatched close tag: expected </{}>, got </{}> at byte {}",
                    payload_, aux_, offset_);
            case Kind::InvalidXml:
                return std::format("Invalid XML: {}", payload_);
            case Kind::Io:
                return std::format("IO error: {}", payload_);
            case Kind::InvalidSxi:
                return std::format("Invalid .sxi file: {}", payload_);
            case Kind::StaleSxi:
                return "Stale .sxi index: XML content has changed since index was built";
        }
        return "Unknown error";
    }

    friend std::ostream& operator<<(std::ostream& os, const SimdXmlError& e) {
        return os << e.to_string();
    }

    bool operator==(SimdXmlError const& other) const noexcept {
        return kind_ == other.kind_ && offset_ == other.offset_ &&
               payload_ == other.payload_ && aux_ == other.aux_;
    }

private:
    Kind kind_ = Kind::InvalidXml;
    std::size_t offset_ = 0;
    std::string payload_;  ///< message / expected name
    std::string aux_;      ///< found name (MismatchedCloseTag only)
};

/// Alias for `std::expected<T, SimdXmlError>`. Mirrors Rust `Result<T>`.
template <typename T>
using Result = std::expected<T, SimdXmlError>;

}  // namespace simdxml

/// Inject a formatter specialization for `std::format` compatibility.
template <>
struct std::formatter<simdxml::SimdXmlError> : std::formatter<std::string> {
    template <typename FormatContext>
    auto format(simdxml::SimdXmlError const& e, FormatContext& ctx) const {
        return std::formatter<std::string>::format(e.to_string(), ctx);
    }
};