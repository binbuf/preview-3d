#pragma once

// Product-owned bounded admission for the two non-Part-21 CAD interchange
// formats added in T08: IGES (fixed 80-column section-letter records) and
// OCCT BREP (the `CASCADE Topology` ASCII shape set). Both scanners are small,
// streaming, allocation-bounded lexical readers -- not second format
// implementations -- and run before the OCCT reader is ever reached.
//
// Neither scanner opens a path: the caller feeds bytes from the inherited
// read-only handle or from the host's existing read-only mapping.

#include <windows.h>

#include <cstddef>
#include <cstdint>
#include <span>
#include <string>

namespace step_host {

enum class CadPreflightStatus : std::uint32_t {
    Ok = 0,
    NotIges = 1,            // not a fixed-column IGES record stream
    NotBrep = 2,            // missing/unsupported CASCADE Topology header
    UnsupportedEncoding = 3, // control bytes / non-ASCII record framing
    MalformedSyntax = 4,     // bad section order, record shape, or TShapes table
    RecordLimit = 5,
    EntityLimit = 6,
    ReferenceLimit = 7,
    SourceLimit = 8,
    ReadFailure = 9,
    // T09 OCCT binary XCAF document: the fixed `BINFILE` + format-version
    // header was absent or not the supported binary document version.
    NotXbf = 10,
};

// Documented admission ceilings. They are deliberately independent of the
// per-definition tessellation budgets: admission rejects a hostile lexical
// shape cheaply, and the adapter still enforces its own scene/face/triangle
// limits after a syntactically valid file has been transferred.
struct CadPreflightLimits {
    std::uint64_t maxBytes = 2ull * 1024 * 1024 * 1024;
    std::uint32_t maxRecords = 5'000'000;
    std::uint32_t maxEntities = 5'000'000;
    std::uint64_t maxReferences = 100'000'000;
    // BREP "TShapes <n>" and "Locations <n>" ceilings. A single BREP file can
    // describe an arbitrary topology graph; these bound the shape table before
    // BRepTools allocates it.
    std::uint32_t maxBrepShapes = 2'000'000;
    std::uint32_t maxBrepLocations = 1'000'000;
};

struct CadPreflightResult {
    CadPreflightStatus status = CadPreflightStatus::Ok;
    std::uint32_t records = 0;
    std::uint32_t entities = 0;    // IGES D-section records / BREP TShapes
    std::uint64_t references = 0;  // IGES parameter tokens
    std::uint32_t brepLocations = 0;
    std::uint32_t unitFlag = 0;    // IGES global parameter 14; 0 = absent/unknown
    std::uint64_t lexedBytes = 0;

    bool ok() const { return status == CadPreflightStatus::Ok; }
};

// Streaming scanner; Feed returns false once a terminal failure is recorded.
class IgesPreflightScanner {
public:
    explicit IgesPreflightScanner(CadPreflightLimits limits = {});
    bool Feed(std::span<const std::byte> chunk);
    bool Finish();
    const CadPreflightResult& Result() const { return result_; }

private:
    void Consume(char value);
    void FinishLine();
    void ProcessLine(std::string_view line);
    void Fail(CadPreflightStatus status);

    CadPreflightLimits limits_{};
    CadPreflightResult result_{};
    std::string line_;
    bool pendingCarriageReturn_ = false;
    bool failed_ = false;
    std::uint32_t lastRank_ = 0;
    std::uint32_t lastSequence_ = 0;
    bool sawStart_ = false;
    bool sawGlobal_ = false;
    bool sawTerminate_ = false;
    bool sawDefinition_ = false;
    bool inGlobal_ = false;
    std::string globalParameters_;
};

class BrepPreflightScanner {
public:
    explicit BrepPreflightScanner(CadPreflightLimits limits = {});
    bool Feed(std::span<const std::byte> chunk);
    bool Finish();
    const CadPreflightResult& Result() const { return result_; }

private:
    void Consume(char value);
    void FinishLine();
    void ProcessLine(std::string_view line);
    void Fail(CadPreflightStatus status);

    CadPreflightLimits limits_{};
    CadPreflightResult result_{};
    std::string line_;
    bool pendingCarriageReturn_ = false;
    bool failed_ = false;
    bool sawDrawablePrefix_ = false;
    bool sawHeader_ = false;
    bool sawShapes_ = false;
    bool sawContentAfterShapes_ = false;
};

CadPreflightResult IgesPreflightBytes(std::span<const std::byte> bytes,
                                      CadPreflightLimits limits = {});
CadPreflightResult IgesPreflightHandle(HANDLE handle, std::uint64_t size,
                                       CadPreflightLimits limits = {});
CadPreflightResult BrepPreflightBytes(std::span<const std::byte> bytes,
                                      CadPreflightLimits limits = {});
CadPreflightResult BrepPreflightHandle(HANDLE handle, std::uint64_t size,
                                       CadPreflightLimits limits = {});

// T09 XBF admission. The OCCT binary XCAF document starts with the seven ASCII
// bytes `BINFILE` followed by the little-endian 64-bit format signature
// 0x0000000704030201. Only the header and the documented byte cap are checked
// here; the XCAF read itself still runs under the adapter's own limits.
CadPreflightResult XbfPreflightBytes(std::span<const std::byte> bytes,
                                     CadPreflightLimits limits = {});
CadPreflightResult XbfPreflightHandle(HANDLE handle, std::uint64_t size,
                                      CadPreflightLimits limits = {});

} // namespace step_host
