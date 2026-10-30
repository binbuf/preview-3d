#define NOMINMAX

#include "CadPreflight.h"

#include <algorithm>
#include <array>
#include <cstring>
#include <limits>
#include <vector>

namespace step_host {
namespace {

constexpr std::size_t kIgesRecordBytes = 80;
constexpr std::size_t kMaxGlobalParameterBytes = 1u << 20;

bool IsControlByte(unsigned char value)
{
    return value < 0x20 || value == 0x7f;
}

// IGES global parameters are comma separated, with Hollerith strings written
// as `nH...`. The scanner only needs parameter 14 (the unit flag), but it must
// still skip strings correctly so a comma inside a string cannot shift the
// parameter ordinal.
std::uint32_t ParseIgesUnitFlag(std::string_view parameters)
{
    std::uint32_t tokenIndex = 0;
    std::size_t index = 0;
    while (index <= parameters.size()) {
        const std::size_t tokenStart = index;
        // Hollerith string prefix: digits followed by H/h.
        std::size_t digits = 0;
        while (tokenStart + digits < parameters.size()
               && parameters[tokenStart + digits] >= '0'
               && parameters[tokenStart + digits] <= '9')
            ++digits;
        if (digits > 0 && tokenStart + digits < parameters.size()
            && (parameters[tokenStart + digits] == 'H'
                || parameters[tokenStart + digits] == 'h')) {
            unsigned long long length = 0;
            for (std::size_t i = 0; i < digits && i < 9; ++i) {
                length = length * 10 + static_cast<unsigned long long>(parameters[tokenStart + i] - '0');
                if (length > kMaxGlobalParameterBytes) return 0;
            }
            index = tokenStart + digits + 1 + static_cast<std::size_t>(length);
            if (index > parameters.size()) return 0; // truncated Hollerith
        } else {
            index = parameters.find(',', tokenStart);
            if (index == std::string_view::npos) index = parameters.size();
        }
        if (tokenIndex == 13) {
            const std::size_t end = (index < parameters.size()) ? index : parameters.size();
            if (end <= tokenStart) return 0;
            unsigned long long value = 0;
            for (std::size_t i = tokenStart; i < end; ++i) {
                const char c = parameters[i];
                if (c == ' ' || c == '\t') continue;
                if (c < '0' || c > '9') return 0;
                value = value * 10 + static_cast<unsigned long long>(c - '0');
                if (value > 100) return 0;
            }
            return static_cast<std::uint32_t>(value);
        }
        ++tokenIndex;
        if (index >= parameters.size()) break;
        ++index; // skip comma
    }
    return 0;
}

std::uint64_t CountIgesParameterTokens(std::string_view data)
{
    // A bounded proxy for reference count: every comma-separated parameter is
    // one authored token. The exact reference graph is OCCT's concern, but the
    // token count is monotone in the record size and stops a hostile D-section
    // with an unbounded parameter list before the reader allocates it.
    std::uint64_t tokens = 1;
    for (const char c : data) {
        if (c == ',') ++tokens;
        if (tokens > std::numeric_limits<std::uint32_t>::max()) break;
    }
    return tokens;
}

bool ParseUnsigned(std::string_view text, std::uint64_t& value)
{
    value = 0;
    bool any = false;
    for (const char c : text) {
        if (c == ' ' || c == '\t') {
            if (any) break;
            continue;
        }
        if (c < '0' || c > '9') return false;
        any = true;
        if (value > (std::numeric_limits<std::uint64_t>::max() - 9) / 10) return false;
        value = value * 10 + static_cast<std::uint64_t>(c - '0');
        if (value > 0xffffffffull) return false;
    }
    return any;
}

bool IsBrepHeader(std::string_view line)
{
    return line == "CASCADE Topology V1, (c) Matra-Datavision"
        || line == "CASCADE Topology V2, (c) Matra-Datavision"
        || line == "CASCADE Topology V3, (c) Open Cascade";
}

template <typename Scanner>
CadPreflightResult ScanHandle(HANDLE handle, std::uint64_t size, Scanner scanner)
{
    LARGE_INTEGER beginning{};
    if (!SetFilePointerEx(handle, beginning, nullptr, FILE_BEGIN)) {
        CadPreflightResult result;
        result.status = CadPreflightStatus::ReadFailure;
        return result;
    }
    std::array<std::byte, 64 * 1024> buffer{};
    std::uint64_t total = 0;
    for (;;) {
        DWORD read = 0;
        if (!ReadFile(handle, buffer.data(), static_cast<DWORD>(buffer.size()), &read, nullptr)) {
            CadPreflightResult result;
            result.status = CadPreflightStatus::ReadFailure;
            return result;
        }
        if (read == 0) break;
        total += read;
        if (total > size) {
            CadPreflightResult result;
            result.status = CadPreflightStatus::SourceLimit;
            return result;
        }
        if (!scanner.Feed(std::span<const std::byte>(buffer.data(), read))) break;
    }
    scanner.Finish();
    return scanner.Result();
}

} // namespace

// --- IGES -------------------------------------------------------------------

IgesPreflightScanner::IgesPreflightScanner(CadPreflightLimits limits)
    : limits_(limits)
{
}

void IgesPreflightScanner::Fail(CadPreflightStatus status)
{
    if (failed_) return;
    failed_ = true;
    result_.status = status;
}

bool IgesPreflightScanner::Feed(std::span<const std::byte> chunk)
{
    if (failed_) return false;
    for (const std::byte byte : chunk) {
        if (failed_) break;
        Consume(static_cast<char>(static_cast<unsigned char>(byte)));
    }
    return !failed_;
}

void IgesPreflightScanner::Consume(char value)
{
    result_.lexedBytes++;
    if (result_.lexedBytes > limits_.maxBytes) {
        Fail(CadPreflightStatus::SourceLimit);
        return;
    }
    if (pendingCarriageReturn_) {
        if (value != '\n') {
            Fail(CadPreflightStatus::MalformedSyntax);
            return;
        }
        pendingCarriageReturn_ = false;
        FinishLine();
        return;
    }
    if (value == '\r') {
        pendingCarriageReturn_ = true;
        return;
    }
    if (value == '\n') {
        FinishLine();
        return;
    }
    if (IsControlByte(static_cast<unsigned char>(value))) {
        Fail(CadPreflightStatus::UnsupportedEncoding);
        return;
    }
    if (line_.size() >= kIgesRecordBytes) {
        Fail(CadPreflightStatus::MalformedSyntax);
        return;
    }
    line_.push_back(value);
}

void IgesPreflightScanner::FinishLine()
{
    if (line_.empty()) {
        Fail(result_.records == 0 ? CadPreflightStatus::NotIges
                                  : CadPreflightStatus::MalformedSyntax);
        return;
    }
    ProcessLine(line_);
    line_.clear();
}

void IgesPreflightScanner::ProcessLine(std::string_view line)
{
    if (failed_) return;
    if (line.size() != kIgesRecordBytes) {
        Fail(CadPreflightStatus::MalformedSyntax);
        return;
    }
    if (++result_.records > limits_.maxRecords) {
        Fail(CadPreflightStatus::RecordLimit);
        return;
    }
    const char section = line[72];
    std::uint32_t rank = 0;
    switch (section) {
    case 'S': rank = 1; break;
    case 'G': rank = 2; break;
    case 'D': rank = 3; break;
    case 'P': rank = 4; break;
    case 'T': rank = 5; break;
    default:
        Fail(result_.records == 1 ? CadPreflightStatus::NotIges
                                  : CadPreflightStatus::MalformedSyntax);
        return;
    }
    if (sawTerminate_) {
        Fail(CadPreflightStatus::MalformedSyntax);
        return;
    }

    std::uint64_t sequence = 0;
    if (!ParseUnsigned(line.substr(73, 7), sequence) || sequence == 0) {
        Fail(CadPreflightStatus::MalformedSyntax);
        return;
    }

    if (rank < lastRank_) {
        Fail(CadPreflightStatus::MalformedSyntax);
        return;
    }
    if (rank > lastRank_) {
        // S -> G -> D -> P -> T, with D/P optional but never out of order.
        if (rank == 2 && !sawStart_) { Fail(CadPreflightStatus::MalformedSyntax); return; }
        if ((rank == 3 || rank == 4 || rank == 5) && !sawGlobal_) {
            Fail(CadPreflightStatus::MalformedSyntax);
            return;
        }
        lastRank_ = rank;
        lastSequence_ = 0;
    }
    if (sequence <= lastSequence_) {
        Fail(CadPreflightStatus::MalformedSyntax);
        return;
    }
    lastSequence_ = static_cast<std::uint32_t>(sequence);

    const std::string_view data = line.substr(0, 72);
    switch (section) {
    case 'S':
        sawStart_ = true;
        break;
    case 'G': {
        sawGlobal_ = true;
        inGlobal_ = true;
        if (globalParameters_.size() + data.size() > kMaxGlobalParameterBytes) {
            Fail(CadPreflightStatus::ReferenceLimit);
            return;
        }
        globalParameters_.append(data.data(), data.size());
        break;
    }
    case 'D':
        inGlobal_ = false;
        sawDefinition_ = true;
        if (++result_.entities > limits_.maxEntities) {
            Fail(CadPreflightStatus::EntityLimit);
            return;
        }
        result_.references += CountIgesParameterTokens(data);
        if (result_.references > limits_.maxReferences) {
            Fail(CadPreflightStatus::ReferenceLimit);
            return;
        }
        break;
    case 'P':
        inGlobal_ = false;
        break;
    case 'T':
        inGlobal_ = false;
        sawTerminate_ = true;
        break;
    default:
        break;
    }
}

bool IgesPreflightScanner::Finish()
{
    if (failed_) return false;
    if (!line_.empty() || pendingCarriageReturn_) {
        // A final record is acceptable only if it is a complete 80-column
        // record; a short tail is exactly the truncation this scanner rejects.
        if (!line_.empty()) ProcessLine(line_);
        line_.clear();
        pendingCarriageReturn_ = false;
    }
    if (failed_) return false;
    if (!sawStart_ || !sawGlobal_ || !sawTerminate_ || lastRank_ != 5) {
        Fail(CadPreflightStatus::MalformedSyntax);
        return false;
    }
    result_.unitFlag = ParseIgesUnitFlag(globalParameters_);
    return true;
}

// --- BREP -------------------------------------------------------------------

BrepPreflightScanner::BrepPreflightScanner(CadPreflightLimits limits)
    : limits_(limits)
{
}

void BrepPreflightScanner::Fail(CadPreflightStatus status)
{
    if (failed_) return;
    failed_ = true;
    result_.status = status;
}

bool BrepPreflightScanner::Feed(std::span<const std::byte> chunk)
{
    if (failed_) return false;
    for (const std::byte byte : chunk) {
        if (failed_) break;
        Consume(static_cast<char>(static_cast<unsigned char>(byte)));
    }
    return !failed_;
}

void BrepPreflightScanner::Consume(char value)
{
    result_.lexedBytes++;
    if (result_.lexedBytes > limits_.maxBytes) {
        Fail(CadPreflightStatus::SourceLimit);
        return;
    }
    if (pendingCarriageReturn_) {
        if (value != '\n') {
            Fail(CadPreflightStatus::MalformedSyntax);
            return;
        }
        pendingCarriageReturn_ = false;
        FinishLine();
        return;
    }
    if (value == '\r') {
        pendingCarriageReturn_ = true;
        return;
    }
    if (value == '\n') {
        FinishLine();
        return;
    }
    if (IsControlByte(static_cast<unsigned char>(value)) && value != '\t') {
        Fail(CadPreflightStatus::UnsupportedEncoding);
        return;
    }
    line_.push_back(value);
}

void BrepPreflightScanner::FinishLine()
{
    if (line_.empty()) {
        if (!sawHeader_) return; // leading blank line is valid
        if (sawShapes_) return;  // blank separators are valid
    }
    ProcessLine(line_);
    line_.clear();
}

void BrepPreflightScanner::ProcessLine(std::string_view line)
{
    if (failed_) return;
    if (++result_.records > limits_.maxRecords) {
        Fail(CadPreflightStatus::RecordLimit);
        return;
    }
    if (line.find("Interrupted by the user") != std::string_view::npos) {
        Fail(CadPreflightStatus::MalformedSyntax);
        return;
    }
    if (!sawHeader_) {
        if (line.empty()) return;
        // BRepTools::Write emits this Draw-compatible marker before the
        // versioned shape set. Accept exactly that one optional prefix.
        if (line == "DBRep_DrawableShape" && !sawDrawablePrefix_) {
            sawDrawablePrefix_ = true;
            return;
        }
        if (!IsBrepHeader(line)) {
            Fail(CadPreflightStatus::NotBrep);
            return;
        }
        sawHeader_ = true;
        return;
    }
    if (!sawShapes_) {
        if (line.rfind("Locations ", 0) == 0) {
            std::uint64_t count = 0;
            if (ParseUnsigned(line.substr(std::string_view("Locations ").size()), count)) {
                result_.brepLocations = static_cast<std::uint32_t>(count);
                if (count > limits_.maxBrepLocations) {
                    Fail(CadPreflightStatus::RecordLimit);
                    return;
                }
            }
        }
        if (line.rfind("TShapes ", 0) == 0) {
            std::uint64_t count = 0;
            if (!ParseUnsigned(line.substr(std::string_view("TShapes ").size()), count)) {
                Fail(CadPreflightStatus::MalformedSyntax);
                return;
            }
            if (count > limits_.maxBrepShapes) {
                Fail(CadPreflightStatus::EntityLimit);
                return;
            }
            result_.entities = static_cast<std::uint32_t>(count);
            sawShapes_ = true;
        }
        return;
    }
    if (!line.empty()) sawContentAfterShapes_ = true;
}

bool BrepPreflightScanner::Finish()
{
    if (failed_) return false;
    if (!line_.empty()) ProcessLine(line_);
    line_.clear();
    pendingCarriageReturn_ = false;
    if (failed_) return false;
    if (!sawHeader_) {
        Fail(CadPreflightStatus::NotBrep);
        return false;
    }
    if (!sawShapes_) {
        Fail(CadPreflightStatus::MalformedSyntax);
        return false;
    }
    if (result_.entities > 0 && !sawContentAfterShapes_) {
        Fail(CadPreflightStatus::MalformedSyntax);
        return false;
    }
    return true;
}

// --- convenience wrappers ----------------------------------------------------

CadPreflightResult IgesPreflightBytes(std::span<const std::byte> bytes,
                                      CadPreflightLimits limits)
{
    IgesPreflightScanner scanner(limits);
    scanner.Feed(bytes);
    scanner.Finish();
    return scanner.Result();
}

CadPreflightResult IgesPreflightHandle(HANDLE handle, std::uint64_t size,
                                       CadPreflightLimits limits)
{
    return ScanHandle(handle, size, IgesPreflightScanner(limits));
}

CadPreflightResult BrepPreflightBytes(std::span<const std::byte> bytes,
                                      CadPreflightLimits limits)
{
    BrepPreflightScanner scanner(limits);
    scanner.Feed(bytes);
    scanner.Finish();
    return scanner.Result();
}

CadPreflightResult BrepPreflightHandle(HANDLE handle, std::uint64_t size,
                                       CadPreflightLimits limits)
{
    return ScanHandle(handle, size, BrepPreflightScanner(limits));
}

// --- XBF --------------------------------------------------------------------

namespace {

constexpr char kXbfMagic[] = {'B', 'I', 'N', 'F', 'I', 'L', 'E'};
constexpr std::size_t kXbfHeaderBytes = 15; // 7-byte magic + 8-byte version
constexpr std::uint64_t kXbfFormatVersion = 0x0000000704030201ull;

CadPreflightResult InspectXbfHeader(std::span<const std::byte> header,
                                   std::uint64_t sourceBytes,
                                   CadPreflightLimits limits)
{
    CadPreflightResult result;
    if (sourceBytes == 0) {
        result.status = CadPreflightStatus::ReadFailure;
        return result;
    }
    if (sourceBytes > limits.maxBytes || header.size() > limits.maxBytes) {
        result.status = CadPreflightStatus::SourceLimit;
        return result;
    }
    if (header.size() < kXbfHeaderBytes) {
        result.status = CadPreflightStatus::NotXbf;
        return result;
    }
    for (std::size_t index = 0; index < sizeof(kXbfMagic); ++index) {
        if (header[index] != static_cast<std::byte>(kXbfMagic[index])) {
            result.status = CadPreflightStatus::NotXbf;
            return result;
        }
    }
    std::uint64_t version = 0;
    for (std::size_t index = 0; index < 8; ++index) {
        version |= static_cast<std::uint64_t>(
                       std::to_integer<std::uint8_t>(header[7 + index]))
            << (index * 8);
    }
    if (version != kXbfFormatVersion) {
        result.status = CadPreflightStatus::NotXbf;
        return result;
    }
    result.records = 1;
    result.lexedBytes = sourceBytes;
    return result;
}

} // namespace

CadPreflightResult XbfPreflightBytes(std::span<const std::byte> bytes,
                                     CadPreflightLimits limits)
{
    return InspectXbfHeader(bytes.first((std::min)(bytes.size(), kXbfHeaderBytes)),
                            bytes.size(), limits);
}

CadPreflightResult XbfPreflightHandle(HANDLE handle, std::uint64_t size,
                                      CadPreflightLimits limits)
{
    if (size == 0) {
        CadPreflightResult result;
        result.status = CadPreflightStatus::ReadFailure;
        return result;
    }
    if (size > limits.maxBytes) {
        CadPreflightResult result;
        result.status = CadPreflightStatus::SourceLimit;
        return result;
    }
    LARGE_INTEGER beginning{};
    if (!SetFilePointerEx(handle, beginning, nullptr, FILE_BEGIN)) {
        CadPreflightResult result;
        result.status = CadPreflightStatus::ReadFailure;
        return result;
    }
    std::array<std::byte, kXbfHeaderBytes> header{};
    DWORD read = 0;
    if (!ReadFile(handle, header.data(), static_cast<DWORD>(header.size()), &read, nullptr)) {
        CadPreflightResult result;
        result.status = CadPreflightStatus::ReadFailure;
        return result;
    }
    return InspectXbfHeader(std::span<const std::byte>(header.data(), read), size, limits);
}

} // namespace step_host
