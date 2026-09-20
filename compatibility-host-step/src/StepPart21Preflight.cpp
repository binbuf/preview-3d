#include "StepPart21Preflight.h"

#include <algorithm>
#include <array>
#include <cctype>
#include <cstring>
#include <vector>

namespace step_host {
namespace {

bool IsSpaceByte(std::byte value)
{
    const auto c = static_cast<unsigned char>(value);
    return c == ' ' || c == '\t' || c == '\r' || c == '\n' || c == '\f' || c == '\v';
}

bool IsDigitByte(char c)
{
    return c >= '0' && c <= '9';
}

bool IsKeywordChar(char c)
{
    return (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || IsDigitByte(c)
        || c == '-' || c == '_';
}

char ToUpperAscii(char c)
{
    return (c >= 'a' && c <= 'z') ? char(c - 'a' + 'A') : c;
}

std::string_view Trim(std::string_view text)
{
    std::size_t begin = 0;
    while (begin < text.size() && (text[begin] == ' ' || text[begin] == '\t'
        || text[begin] == '\r' || text[begin] == '\n' || text[begin] == '\f'
        || text[begin] == '\v'))
        ++begin;
    std::size_t end = text.size();
    while (end > begin && (text[end - 1] == ' ' || text[end - 1] == '\t'
        || text[end - 1] == '\r' || text[end - 1] == '\n' || text[end - 1] == '\f'
        || text[end - 1] == '\v'))
        --end;
    return text.substr(begin, end - begin);
}

std::string_view FirstWord(std::string_view text)
{
    std::size_t end = 0;
    while (end < text.size() && IsKeywordChar(text[end])) ++end;
    return text.substr(0, end);
}

bool ContainsIgnoreCase(std::string_view text, std::string_view needle)
{
    if (needle.empty() || needle.size() > text.size()) return false;
    const std::size_t limit = text.size() - needle.size();
    for (std::size_t i = 0; i <= limit; ++i) {
        bool match = true;
        for (std::size_t j = 0; j < needle.size(); ++j) {
            if (ToUpperAscii(text[i + j]) != ToUpperAscii(needle[j])) { match = false; break; }
        }
        if (match) return true;
    }
    return false;
}

} // namespace

StepPart21Scanner::StepPart21Scanner(StepPreflightLimits limits) : limits_(limits)
{
    record_.reserve(4096);
}

void StepPart21Scanner::Fail(StepPreflightStatus status)
{
    if (phase_ == Phase::Failed) return;
    phase_ = Phase::Failed;
    result_.status = status;
}

bool StepPart21Scanner::Feed(std::span<const std::byte> chunk)
{
    if (phase_ == Phase::Failed) return false;

    std::size_t start = 0;
    if (!leadingChecked_ && !chunk.empty()) {
        leadingChecked_ = true;
        // UTF-8 BOM is transparent; UTF-16 BOM and compressed/XML signatures
        // are not ISO 10303-21 clear text and fail before any lexing.
        if (chunk.size() >= 3 && chunk[0] == std::byte{0xEF} && chunk[1] == std::byte{0xBB}
            && chunk[2] == std::byte{0xBF}) {
            start = 3;
        } else if (chunk.size() >= 2
            && ((chunk[0] == std::byte{0xFF} && chunk[1] == std::byte{0xFE})
                || (chunk[0] == std::byte{0xFE} && chunk[1] == std::byte{0xFF}))) {
            Fail(StepPreflightStatus::UnsupportedEncoding);
            return false;
        } else if (chunk.size() >= 4 && chunk[0] == std::byte{0x50} && chunk[1] == std::byte{0x4B}
            && chunk[2] == std::byte{0x03} && chunk[3] == std::byte{0x04}) {
            Fail(StepPreflightStatus::UnsupportedEncoding);
            return false;
        } else if (chunk.size() >= 2 && chunk[0] == std::byte{0x1F} && chunk[1] == std::byte{0x8B}) {
            Fail(StepPreflightStatus::UnsupportedEncoding);
            return false;
        }
        std::size_t probe = start;
        while (probe < chunk.size() && IsSpaceByte(chunk[probe])) ++probe;
        if (probe < chunk.size() && chunk[probe] == std::byte{'<'}) {
            Fail(StepPreflightStatus::UnsupportedEncoding);
            return false;
        }
        // Count the optional UTF-8 BOM once, but do not pass its bytes to the
        // Part-21 lexer: they are neither whitespace nor part of the signature.
        for (std::size_t i = 0; i < start; ++i) {
            if (totalBytes_ >= limits_.maxLexedBytes) { Fail(StepPreflightStatus::SourceLimit); return false; }
            ++totalBytes_;
        }
    }

    for (const std::byte value : chunk.subspan(start)) {
        if (phase_ == Phase::Failed) return false;
        if (totalBytes_ >= limits_.maxLexedBytes) { Fail(StepPreflightStatus::SourceLimit); return false; }
        ++totalBytes_;
        Consume(value);
    }
    return phase_ != Phase::Failed;
}

void StepPart21Scanner::Consume(std::byte value)
{
    const char c = static_cast<char>(static_cast<unsigned char>(value));

    if (inComment_) {
        if (commentSlash_ && c == '/') { inComment_ = false; commentSlash_ = false; return; }
        commentSlash_ = (c == '*');
        return;
    }

    if (inString_) {
        if (pendingQuoteEnd_) {
            if (c == quote_) {
                // Doubled quote is an escaped quote inside the string.
                pendingQuoteEnd_ = false;
                if (record_.size() >= limits_.maxRecordBytes) { Fail(StepPreflightStatus::RecordLengthLimit); return; }
                record_.push_back(c);
                if (++currentStringBytes_ > limits_.maxStringBytes) { Fail(StepPreflightStatus::StringLengthLimit); return; }
                return;
            }
            // The tentative quote really did close the string; emit it before
            // processing this byte outside the string.
            if (record_.size() >= limits_.maxRecordBytes) { Fail(StepPreflightStatus::RecordLengthLimit); return; }
            record_.push_back(quote_);
            pendingQuoteEnd_ = false;
            inString_ = false;
            // fall through: process this byte outside the string
        } else if (c == quote_) {
            pendingQuoteEnd_ = true;
            return;
        } else {
            if (record_.size() >= limits_.maxRecordBytes) { Fail(StepPreflightStatus::RecordLengthLimit); return; }
            record_.push_back(c);
            if (++currentStringBytes_ > limits_.maxStringBytes) { Fail(StepPreflightStatus::StringLengthLimit); return; }
            return;
        }
    }

    if (c == '/') {
        if (commentSlash_) { commentSlash_ = false; /* "//" is ordinary text */ }
        else { commentSlash_ = true; return; }
    } else if (commentSlash_) {
        if (c == '*') { commentSlash_ = false; inComment_ = true; return; }
        // The previous '/' was ordinary text; emit it and continue with c.
        if (record_.size() >= limits_.maxRecordBytes) { Fail(StepPreflightStatus::RecordLengthLimit); return; }
        record_.push_back('/');
        commentSlash_ = false;
    }

    if (c < 0x20 && c != '\t' && c != '\n' && c != '\r' && c != '\f' && c != '\v') {
        Fail(StepPreflightStatus::UnsupportedEncoding);
        return;
    }

    if (c == '\'' || c == '"') {
        inString_ = true;
        quote_ = c;
        currentStringBytes_ = 0;
        if (record_.size() >= limits_.maxRecordBytes) { Fail(StepPreflightStatus::RecordLengthLimit); return; }
        record_.push_back(c);
        return;
    }

    if (c == '(') {
        ++depth_;
        result_.maxDepth = (std::max)(result_.maxDepth, depth_);
        if (depth_ > limits_.maxNestingDepth) { Fail(StepPreflightStatus::DepthLimit); return; }
    } else if (c == ')') {
        if (depth_ > 0) --depth_;
    } else if (c == ';') {
        if (depth_ == 0) { FinishRecord(); return; }
    }

    if (record_.size() >= limits_.maxRecordBytes) { Fail(StepPreflightStatus::RecordLengthLimit); return; }
    record_.push_back(c);
}

void StepPart21Scanner::FinishRecord()
{
    const std::string text = std::move(record_);
    record_.clear();
    currentStringBytes_ = 0;
    ParseRecord(text);
}

void StepPart21Scanner::ParseRecord(std::string_view raw)
{
    std::string_view text = Trim(raw);
    if (text.empty()) return;

    if (!sawSignature_) {
        if (FirstWord(text) == "ISO-10303-21") { sawSignature_ = true; return; }
        Fail(StepPreflightStatus::NotPart21);
        return;
    }

    if (phase_ == Phase::Ended) { Fail(StepPreflightStatus::MalformedSyntax); return; }

    if (text.front() == '#') {
        if (!inData_) { Fail(StepPreflightStatus::MalformedSyntax); return; }
        std::size_t index = 1;
        std::uint64_t id = 0;
        while (index < text.size() && IsDigitByte(text[index])) {
            id = id * 10 + std::uint64_t(text[index] - '0');
            if (id > 0xFFFFFFFFull) { Fail(StepPreflightStatus::DuplicateEntity); return; }
            ++index;
        }
        if (index == 1 || id == 0) { Fail(StepPreflightStatus::DuplicateEntity); return; }
        while (index < text.size() && (text[index] == ' ' || text[index] == '\t')) ++index;
        if (index >= text.size() || text[index] != '=') { Fail(StepPreflightStatus::MalformedSyntax); return; }
        if (!entityIds_.insert(static_cast<std::uint32_t>(id)).second) {
            Fail(StepPreflightStatus::DuplicateEntity);
            return;
        }
        if (++result_.entityRecords > limits_.maxEntityRecords) {
            Fail(StepPreflightStatus::EntityLimit);
            return;
        }
        ScanTokens(text.substr(index + 1));
        return;
    }

    const std::string_view word = FirstWord(text);
    if (word == "HEADER") {
        if (inHeader_ || inData_) { Fail(StepPreflightStatus::MalformedSyntax); return; }
        inHeader_ = true;
        return;
    }
    if (word == "DATA") {
        if (inHeader_ || inData_) { Fail(StepPreflightStatus::MalformedSyntax); return; }
        inData_ = true;
        if (++result_.dataSections > limits_.maxDataSections) {
            Fail(StepPreflightStatus::SourceLimit);
            return;
        }
        return;
    }
    if (word == "ENDSEC") {
        if (!inHeader_ && !inData_) { Fail(StepPreflightStatus::MalformedSyntax); return; }
        inHeader_ = false;
        inData_ = false;
        return;
    }
    if (word == "END-ISO-10303-21") {
        if (inHeader_ || inData_) { Fail(StepPreflightStatus::MalformedSyntax); return; }
        phase_ = Phase::Ended;
        return;
    }
    if (inHeader_) {
        if (word == "FILE_SCHEMA") {
            const auto open = text.find('(');
            const auto quote = text.find('\'', open == std::string_view::npos ? 0 : open);
            if (quote != std::string_view::npos) {
                std::size_t end = quote + 1;
                std::string schema;
                while (end < text.size() && text[end] != '\'' && schema.size() < sizeof(result_.schema) - 1) {
                    schema.push_back(ToUpperAscii(text[end]));
                    ++end;
                }
                std::memcpy(result_.schema, schema.c_str(), schema.size());
                result_.schema[schema.size()] = '\0';
            }
        }
        ScanTokens(text);
        return;
    }
    if (inData_) {
        // Part-21 edition 3 scope markers carry no entity identity.
        if (word == "SCOPE" || word == "ENDSCOPE") { ScanTokens(text); return; }
        Fail(StepPreflightStatus::MalformedSyntax);
        return;
    }
    Fail(StepPreflightStatus::MalformedSyntax);
}

void StepPart21Scanner::ScanTokens(std::string_view text)
{
    bool inString = false;
    bool inComment = false;
    char quote = '\'';
    bool pendingQuoteEnd = false;
    bool previousSlash = false;

    for (std::size_t i = 0; i < text.size(); ++i) {
        const char c = text[i];
        if (inComment) {
            if (previousSlash && c == '/') { inComment = false; previousSlash = false; continue; }
            previousSlash = (c == '*');
            continue;
        }
        if (inString) {
            if (pendingQuoteEnd) {
                if (c == quote) { pendingQuoteEnd = false; continue; }
                pendingQuoteEnd = false;
                inString = false;
                // fall through to process c outside the string
            } else if (c == quote) {
                pendingQuoteEnd = true;
                continue;
            } else {
                continue;
            }
        }
        if (c == '/') {
            if (!previousSlash) { previousSlash = true; continue; }
            previousSlash = false;
        } else {
            if (previousSlash && c == '*') { previousSlash = false; inComment = true; continue; }
            previousSlash = false;
        }
        if (c == '\'' || c == '"') { inString = true; quote = c; continue; }
        if (c == '#') {
            std::size_t index = i + 1;
            while (index < text.size() && IsDigitByte(text[index])) ++index;
            if (index > i + 1) {
                if (++result_.references > limits_.maxReferenceCount) {
                    Fail(StepPreflightStatus::ReferenceLimit);
                    return;
                }
                i = index - 1;
            }
        }
    }

    // External STEP documents are declared through FILE_POPULATION /
    // DOCUMENT_FILE. STEP-005 is a no-go, so any declaration is a required
    // unsupported feature until a brokered stream resolver exists.
    if (ContainsIgnoreCase(text, "FILE_POPULATION") || ContainsIgnoreCase(text, "DOCUMENT_FILE")) {
        if (++result_.externalDocuments > limits_.maxExternalDocuments) {
            Fail(StepPreflightStatus::ExternalDocument);
        }
    }
}

bool StepPart21Scanner::Finish()
{
    if (phase_ == Phase::Failed) return false;
    result_.lexedBytes = totalBytes_;
    if (inString_ || inComment_ || commentSlash_ || pendingQuoteEnd_) {
        Fail(StepPreflightStatus::MalformedSyntax);
        return false;
    }
    if (depth_ != 0) { Fail(StepPreflightStatus::MalformedSyntax); return false; }
    if (!record_.empty()) { FinishRecord(); if (phase_ == Phase::Failed) return false; }
    if (!sawSignature_) { Fail(StepPreflightStatus::NotPart21); return false; }
    if (phase_ != Phase::Ended) { Fail(StepPreflightStatus::MalformedSyntax); return false; }
    return true;
}

StepPreflightResult StepPreflightBytes(std::span<const std::byte> bytes, StepPreflightLimits limits)
{
    StepPart21Scanner scanner(limits);
    if (!scanner.Feed(bytes)) return scanner.Result();
    scanner.Finish();
    return scanner.Result();
}

StepPreflightResult StepPreflightHandle(HANDLE handle, std::uint64_t size, StepPreflightLimits limits)
{
    StepPart21Scanner scanner(limits);
    if (!handle || handle == INVALID_HANDLE_VALUE) {
        StepPreflightResult result;
        result.status = StepPreflightStatus::ReadFailure;
        return result;
    }
    LARGE_INTEGER origin{};
    if (!SetFilePointerEx(handle, origin, nullptr, FILE_BEGIN)) {
        StepPreflightResult result;
        result.status = StepPreflightStatus::ReadFailure;
        return result;
    }
    std::vector<std::byte> buffer(64 * 1024);
    std::uint64_t remaining = size;
    while (remaining > 0) {
        const DWORD want = static_cast<DWORD>((std::min)(remaining, std::uint64_t(buffer.size())));
        DWORD read = 0;
        if (!ReadFile(handle, buffer.data(), want, &read, nullptr)) {
            StepPreflightResult result;
            result.status = StepPreflightStatus::ReadFailure;
            return result;
        }
        if (read == 0) break;
        if (!scanner.Feed(std::span(buffer.data(), read))) return scanner.Result();
        remaining -= read;
    }
    if (remaining != 0) {
        StepPreflightResult result;
        result.status = StepPreflightStatus::ReadFailure;
        return result;
    }
    scanner.Finish();
    return scanner.Result();
}

} // namespace step_host
