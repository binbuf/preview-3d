#pragma once

// A small, bounded whitespace-delimited tokenizer shared by the ASCII STL and
// ASCII PLY adapters (Gate 3 slices 4/5). Per
// .docs/design/03-file-formats-and-ingestion.md's ASCII-parsing posture ("a
// streaming tokenizer with fixed-size blocks, a token length cap, locale-
// independent number parsing, and no recursive grammar"): a single forward
// scan with no recursion, a per-token length cap so a hostile file can't
// force an unbounded single-token scan, and std::from_chars for numbers
// (locale-independent, unlike strtod/atof).

#include <cstddef>
#include <optional>
#include <span>
#include <string_view>

namespace import_worker {

// Treats '\n' as ordinary whitespace, same as ' '/'\t'/'\r' -- callers that
// need PLY-style "one record per line" structure don't rely on this
// tokenizer to enforce line boundaries; both adapters using it parse a flat
// keyword/number token stream instead (see PlyAdapter.cpp's ASCII body
// comment for why that's a deliberate, spec-compatible simplification).
class AsciiTokenizer {
public:
    explicit AsciiTokenizer(std::span<const std::byte> source, size_t startOffset = 0);

    // Returns the next token, or nullopt at end of input or if the next
    // token would exceed the token-length cap. Both are collapsed into one
    // "no token" outcome because every caller already maps that to
    // MalformedData (a legitimate ASCII STL/PLY file never has an
    // oversized token).
    std::optional<std::string_view> NextToken();

    // Reads the next token and parses it as a locale-independent number.
    // nullopt if there is no next token (see above) or it doesn't fully
    // parse as a number -- trailing garbage after the numeric grammar is
    // rejected, never silently truncated.
    std::optional<double> NextNumber();

    bool AtEnd() const;

private:
    std::string_view text_;
    size_t cursor_ = 0;
};

} // namespace import_worker
