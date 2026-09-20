#pragma once

// Product-owned ISO 10303-21 physical-file admission scanner. It is
// deliberately a small, bounded lexical reader -- not a second STEP
// implementation -- and it reads only through the inherited read-only source
// handle. It verifies the Part-21 envelope and bounded HEADER/DATA/ENDSEC/
// END-ISO-10303-21 structure, counts entity records/references/sections/
// nesting and lexed bytes with checked arithmetic, rejects duplicate or
// impossible entity identifiers and unterminated strings/comments, refuses
// binary/XML/compressed encodings, and discovers external-document
// declarations so the broker policy can be applied before any semantic
// transfer. It never resolves a filesystem path, URL, or child process.
//
// The production OCCT reader cannot be reached until this returns Ok.

#include <windows.h>

#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <string_view>
#include <unordered_set>

namespace step_host {

enum class StepPreflightStatus : std::uint32_t {
    Ok = 0,
    NotPart21 = 1,          // missing/incorrect ISO 10303-21 physical envelope
    UnsupportedEncoding = 2, // binary/UTF-16/compressed/XML bytes
    MalformedSyntax = 3,    // unterminated string/comment, bad record shape
    DuplicateEntity = 4,    // repeated or impossible entity identifier
    EntityLimit = 5,
    ReferenceLimit = 6,
    DepthLimit = 7,
    RecordLengthLimit = 8,
    StringLengthLimit = 9,
    SourceLimit = 10,
    ExternalDocument = 11,  // declares required external STEP content
    ReadFailure = 12,
};

struct StepPreflightLimits {
    // Admission ceilings fixed by STEP-001/STEP-002. They are admission
    // limits, not a promise that an OCCT translation of every limit-sized
    // file fits; the broker's primary-source cap remains independent.
    std::uint64_t maxLexedBytes = 2ull * 1024 * 1024 * 1024;
    std::uint32_t maxEntityRecords = 5'000'000;
    std::uint32_t maxReferenceCount = 100'000'000;
    std::uint32_t maxNestingDepth = 256;
    std::uint32_t maxRecordBytes = 1u << 20; // 1 MiB
    std::uint32_t maxStringBytes = 1u << 20; // 1 MiB
    std::uint32_t maxDataSections = 4096;
    std::uint32_t maxExternalDocuments = 0; // STEP-005 is a no-go; any is required-unsupported
};

struct StepPreflightResult {
    StepPreflightStatus status = StepPreflightStatus::Ok;
    std::uint32_t entityRecords = 0;
    std::uint32_t references = 0;
    std::uint32_t dataSections = 0;
    std::uint32_t maxDepth = 0;
    std::uint32_t externalDocuments = 0;
    std::uint64_t lexedBytes = 0;
    // First FILE_SCHEMA name, for classification/diagnostics only -- never an
    // extension-based trust decision. NUL-terminated, bounded, uppercased.
    char schema[64]{};

    bool ok() const { return status == StepPreflightStatus::Ok; }
};

// Streaming scanner. Feed the whole source in bounded chunks; Feed returns
// false once a terminal failure is recorded and further input is ignored.
class StepPart21Scanner {
public:
    explicit StepPart21Scanner(StepPreflightLimits limits = {});

    bool Feed(std::span<const std::byte> chunk);
    bool Finish();
    const StepPreflightResult& Result() const { return result_; }

private:
    enum class Phase { Signature, Body, Ended, Failed };

    void Fail(StepPreflightStatus status);
    void Consume(std::byte value);
    void FinishRecord();
    void ParseRecord(std::string_view text);
    void ScanTokens(std::string_view text);

    StepPreflightLimits limits_{};
    StepPreflightResult result_{};
    Phase phase_ = Phase::Signature;
    bool sawSignature_ = false;
    bool leadingChecked_ = false;
    bool inHeader_ = false;
    bool inData_ = false;
    bool inString_ = false;
    bool inComment_ = false;
    bool commentSlash_ = false;   // previous byte was '/' outside a string
    bool pendingQuoteEnd_ = false; // a quote was seen; decide escape vs end
    char quote_ = '\'';
    std::uint32_t depth_ = 0;
    std::uint32_t currentStringBytes_ = 0;
    std::uint64_t totalBytes_ = 0;
    std::string record_;
    std::unordered_set<std::uint32_t> entityIds_;
};

// Convenience wrappers. StepPreflightBytes is the testable/fuzzable pure form;
// StepPreflightHandle streams an inherited read-only FILE handle.
StepPreflightResult StepPreflightBytes(std::span<const std::byte> bytes,
                                       StepPreflightLimits limits = {});
StepPreflightResult StepPreflightHandle(HANDLE handle, std::uint64_t size,
                                        StepPreflightLimits limits = {});

} // namespace step_host
