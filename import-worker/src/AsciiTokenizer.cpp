#include "AsciiTokenizer.h"

#include <charconv>

namespace import_worker {

namespace {

// Generous for any legitimate STL/PLY keyword or number token (the longest
// realistic case, a full-precision signed double with exponent, is well
// under 32 characters) -- bounds the per-token scan itself, per the design
// doc's "fixed-size blocks," rather than letting a hostile non-whitespace
// run force an unbounded scan looking for its end.
constexpr size_t kMaxTokenLength = 512;

bool IsAsciiWhitespace(char c)
{
    return c == ' ' || c == '\t' || c == '\r' || c == '\n';
}

} // namespace

AsciiTokenizer::AsciiTokenizer(std::span<const std::byte> source, size_t startOffset)
    : text_(reinterpret_cast<const char*>(source.data()), source.size())
    , cursor_(startOffset < source.size() ? startOffset : source.size())
{
}

bool AsciiTokenizer::AtEnd() const
{
    size_t pos = cursor_;
    while (pos < text_.size() && IsAsciiWhitespace(text_[pos])) {
        ++pos;
    }
    return pos >= text_.size();
}

std::optional<std::string_view> AsciiTokenizer::NextToken()
{
    while (cursor_ < text_.size() && IsAsciiWhitespace(text_[cursor_])) {
        ++cursor_;
    }
    if (cursor_ >= text_.size()) {
        return std::nullopt;
    }

    size_t start = cursor_;
    size_t length = 0;
    while (cursor_ < text_.size() && !IsAsciiWhitespace(text_[cursor_])) {
        ++cursor_;
        ++length;
        if (length > kMaxTokenLength) {
            return std::nullopt; // bounded scan only -- never chase the true end
        }
    }
    return text_.substr(start, length);
}

std::optional<double> AsciiTokenizer::NextNumber()
{
    auto tokenOpt = NextToken();
    if (!tokenOpt) {
        return std::nullopt;
    }
    std::string_view token = *tokenOpt;
    double value = 0.0;
    auto result = std::from_chars(token.data(), token.data() + token.size(), value);
    if (result.ec != std::errc() || result.ptr != token.data() + token.size()) {
        return std::nullopt; // not a fully-consumed valid number
    }
    return value;
}

} // namespace import_worker
