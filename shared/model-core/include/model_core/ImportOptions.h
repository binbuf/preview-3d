#pragma once

// T06: the closed, validated per-format import-option value shared by the
// viewer, the broker, the worker and both import hosts.
//
// The set is product-owned and deliberately small. It is not a free-form
// key/value bag and it never carries reader-supplied text: a parser receives
// only these typed fields after every value has passed the same validation on
// the trusted side and again inside the sandbox.
//
// Option names follow the F3D reader-option shape (`FORMAT.name=value`) but the
// runtime is this product's own. Supported formats and names:
//
//   STEP, IGES, BREP, XBF, FCStd:
//     read_wire            0|1|true|false   (default: fallback)
//     quality              draft|balanced|full (default: balanced)
//     linear_deflection    finite, > 0      (relative fraction or absolute)
//     angular_deflection   finite, > 0      (radians)
//     relative_deflection  0|1|true|false   (default: true)
//     unit_scale           finite, > 0      (metres per transferred unit)
//   USD:
//     subdivision_level    0..4             (default: 0; >0 is rejected when
//                                            the build has no subdivision
//                                            evaluator)
//
// `read_wire` is tri-state on the wire: an absent assignment preserves the T05
// fallback (edges only for a definition with no faces), `1` adds edges
// alongside faces, and `0` suppresses both the fallback and the overlay.
//
// Design authority: docs/tasks/0.8.0/T06-per-format-import-options.md,
// ADR-0100 (generic per-format import options).

#include <array>
#include <bit>
#include <charconv>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <string_view>
#include <system_error>

namespace model_core {

enum class ImportQuality : uint32_t {
    Draft = 0,
    Balanced = 1,
    Full = 2,
};
constexpr uint32_t kImportQualityLast = static_cast<uint32_t>(ImportQuality::Full);

enum class ImportWireMode : uint32_t {
    // T05 behaviour: the edge pass runs only when a definition produced no
    // triangles.
    Fallback = 0,
    // Explicit read_wire=1: edges are emitted alongside faces.
    On = 1,
    // Explicit read_wire=0: no fallback and no overlay.
    Off = 2,
};
constexpr uint32_t kImportWireModeLast = static_cast<uint32_t>(ImportWireMode::Off);

// Closed product-owned format identities. The value order is part of the
// persisted settings and cache identity; append only.
enum class ImportOptionFormat : uint32_t {
    Step = 0,
    Iges = 1,
    Brep = 2,
    Xbf = 3,
    Fcstd = 4,
    Usd = 5,
};
constexpr uint32_t kImportOptionFormatCount = 6;
constexpr uint32_t kImportOptionFormatLast =
    static_cast<uint32_t>(ImportOptionFormat::Usd);

constexpr bool IsKnownImportOptionFormat(ImportOptionFormat format) noexcept
{
    return static_cast<uint32_t>(format) <= kImportOptionFormatLast;
}

constexpr bool IsCadImportOptionFormat(ImportOptionFormat format) noexcept
{
    return format == ImportOptionFormat::Step || format == ImportOptionFormat::Iges
        || format == ImportOptionFormat::Brep || format == ImportOptionFormat::Xbf
        || format == ImportOptionFormat::Fcstd;
}

constexpr const char* ImportOptionFormatName(ImportOptionFormat format) noexcept
{
    switch (format) {
    case ImportOptionFormat::Step: return "STEP";
    case ImportOptionFormat::Iges: return "IGES";
    case ImportOptionFormat::Brep: return "BREP";
    case ImportOptionFormat::Xbf: return "XBF";
    case ImportOptionFormat::Fcstd: return "FCStd";
    case ImportOptionFormat::Usd: return "USD";
    }
    return "";
}

constexpr char ImportOptionAsciiLower(char value) noexcept
{
    return value >= 'A' && value <= 'Z' ? static_cast<char>(value - 'A' + 'a') : value;
}

inline bool ImportOptionEqualsIgnoreCase(std::string_view first, std::string_view second) noexcept
{
    if (first.size() != second.size()) return false;
    for (std::size_t index = 0; index < first.size(); ++index) {
        if (ImportOptionAsciiLower(first[index]) != ImportOptionAsciiLower(second[index]))
            return false;
    }
    return true;
}

inline bool ParseImportOptionFormat(std::string_view name, ImportOptionFormat& format) noexcept
{
    for (uint32_t index = 0; index <= kImportOptionFormatLast; ++index) {
        const auto candidate = static_cast<ImportOptionFormat>(index);
        if (ImportOptionEqualsIgnoreCase(name, ImportOptionFormatName(candidate))) {
            format = candidate;
            return true;
        }
    }
    return false;
}

// Bounded, format-specific option ranges. The relative fraction is expressed
// against a definition's bounding diagonal; the absolute window is in the
// transferred length unit. Both are enforced before a host sees the value and
// again inside the host.
constexpr double kImportOptionMinLinearDeflection = 1e-9;
constexpr double kImportOptionMaxRelativeDeflection = 0.5;
constexpr double kImportOptionMaxAbsoluteDeflection = 1e6;
constexpr double kImportOptionMinAngularDeflection = 1e-9;
constexpr double kImportOptionMaxAngularDeflection = 3.14159265358979323846;
constexpr double kImportOptionMinUnitScale = 1e-9;
constexpr double kImportOptionMaxUnitScale = 1e9;
constexpr uint32_t kImportOptionMaxUsdSubdivisionLevel = 4;

struct ImportOptions {
    ImportQuality quality = ImportQuality::Balanced;
    ImportWireMode wireMode = ImportWireMode::Fallback;
    // Unset means "use the selected quality's versioned profile value".
    std::optional<double> linearDeflection;
    std::optional<double> angularDeflection;
    // Preserves the pre-T06 relative-deflection profile by default.
    bool relativeDeflection = true;
    // Metres per transferred unit. Unset means "use the format's authored or
    // documented unit resolution".
    std::optional<double> unitScaleOverride;
    // Meaningful only for ImportOptionFormat::Usd.
    uint32_t usdSubdivisionLevel = 0;

    friend bool operator==(const ImportOptions&, const ImportOptions&) = default;
};

// One entry per closed format. Indexing is safe only after a value has passed
// IsKnownImportOptionFormat; the parser does that before it commits anything.
struct ImportOptionsTable {
    std::array<ImportOptions, kImportOptionFormatCount> formats{};

    ImportOptions& operator[](ImportOptionFormat format) noexcept
    {
        return formats[static_cast<std::size_t>(format)];
    }
    const ImportOptions& operator[](ImportOptionFormat format) const noexcept
    {
        return formats[static_cast<std::size_t>(format)];
    }
};

enum class ImportOptionError : uint32_t {
    None = 0,
    UnknownFormat,
    UnknownName,
    InvalidValue,
    OutOfRange,
    NotApplicable,
};

constexpr const char* ImportOptionErrorName(ImportOptionError error) noexcept
{
    switch (error) {
    case ImportOptionError::None: return "none";
    case ImportOptionError::UnknownFormat: return "unknown-format";
    case ImportOptionError::UnknownName: return "unknown-name";
    case ImportOptionError::InvalidValue: return "invalid-value";
    case ImportOptionError::OutOfRange: return "out-of-range";
    case ImportOptionError::NotApplicable: return "not-applicable";
    }
    return "unknown";
}

inline bool ImportOptionFiniteInRange(double value, double minimum, double maximum) noexcept
{
    return std::isfinite(value) && value >= minimum && value <= maximum;
}

// Validates the whole record for `format`. A field that does not apply to the
// format is rejected rather than silently ignored, so the accepted set is
// exactly the documented set.
inline bool ValidateImportOptions(ImportOptionFormat format, const ImportOptions& options,
                                  ImportOptionError& error) noexcept
{
    error = ImportOptionError::None;
    if (!IsKnownImportOptionFormat(format)) {
        error = ImportOptionError::UnknownFormat;
        return false;
    }
    if (static_cast<uint32_t>(options.quality) > kImportQualityLast) {
        error = ImportOptionError::InvalidValue;
        return false;
    }
    if (static_cast<uint32_t>(options.wireMode) > kImportWireModeLast) {
        error = ImportOptionError::InvalidValue;
        return false;
    }
    if (options.linearDeflection) {
        if (!ImportOptionFiniteInRange(*options.linearDeflection,
                                       kImportOptionMinLinearDeflection,
                                       kImportOptionMaxAbsoluteDeflection)) {
            error = ImportOptionError::OutOfRange;
            return false;
        }
        if (options.relativeDeflection
            && *options.linearDeflection > kImportOptionMaxRelativeDeflection) {
            error = ImportOptionError::OutOfRange;
            return false;
        }
    }
    if (options.angularDeflection
        && !ImportOptionFiniteInRange(*options.angularDeflection,
                                      kImportOptionMinAngularDeflection,
                                      kImportOptionMaxAngularDeflection)) {
        error = ImportOptionError::OutOfRange;
        return false;
    }
    if (options.unitScaleOverride
        && !ImportOptionFiniteInRange(*options.unitScaleOverride,
                                      kImportOptionMinUnitScale,
                                      kImportOptionMaxUnitScale)) {
        error = ImportOptionError::OutOfRange;
        return false;
    }
    if (options.usdSubdivisionLevel > kImportOptionMaxUsdSubdivisionLevel) {
        error = ImportOptionError::OutOfRange;
        return false;
    }

    if (format == ImportOptionFormat::Usd) {
        if (options.quality != ImportQuality::Balanced
            || options.wireMode != ImportWireMode::Fallback || options.linearDeflection
            || options.angularDeflection || !options.relativeDeflection
            || options.unitScaleOverride) {
            error = ImportOptionError::NotApplicable;
            return false;
        }
    } else {
        if (options.usdSubdivisionLevel != 0) {
            error = ImportOptionError::NotApplicable;
            return false;
        }
    }
    return true;
}

namespace import_options_detail {

inline bool ParseFiniteDouble(std::string_view text, double& value) noexcept
{
    if (text.empty()) return false;
    double parsed = 0.0;
    const auto result = std::from_chars(text.data(), text.data() + text.size(), parsed,
                                        std::chars_format::general);
    if (result.ec != std::errc() || result.ptr != text.data() + text.size()
        || !std::isfinite(parsed)) {
        return false;
    }
    value = parsed;
    return true;
}

inline bool ParseUnsigned(std::string_view text, uint32_t& value) noexcept
{
    if (text.empty()) return false;
    uint32_t parsed = 0;
    const auto result = std::from_chars(text.data(), text.data() + text.size(), parsed);
    if (result.ec != std::errc() || result.ptr != text.data() + text.size()) return false;
    value = parsed;
    return true;
}

inline bool ParseBool(std::string_view text, bool& value) noexcept
{
    if (ImportOptionEqualsIgnoreCase(text, "1") || ImportOptionEqualsIgnoreCase(text, "true")) {
        value = true;
        return true;
    }
    if (ImportOptionEqualsIgnoreCase(text, "0") || ImportOptionEqualsIgnoreCase(text, "false")) {
        value = false;
        return true;
    }
    return false;
}

} // namespace import_options_detail

// Parses one `FORMAT.name=value` assignment and commits it to `table` only when
// the whole candidate record validates. Format and name are ASCII
// case-insensitive; the value is never trimmed, so accidental whitespace is a
// typed rejection rather than a silently different value.
inline bool ParseImportOptionAssignment(std::string_view text, ImportOptionsTable& table,
                                        ImportOptionError& error) noexcept
{
    error = ImportOptionError::None;
    const std::size_t equals = text.find('=');
    if (equals == std::string_view::npos || equals == 0 || equals + 1 >= text.size()) {
        error = ImportOptionError::InvalidValue;
        return false;
    }
    const std::string_view key = text.substr(0, equals);
    const std::string_view value = text.substr(equals + 1);
    const std::size_t dot = key.find('.');
    if (dot == std::string_view::npos || dot == 0 || dot + 1 >= key.size()) {
        error = ImportOptionError::InvalidValue;
        return false;
    }
    ImportOptionFormat format = ImportOptionFormat::Step;
    if (!ParseImportOptionFormat(key.substr(0, dot), format)) {
        error = ImportOptionError::UnknownFormat;
        return false;
    }
    const std::string_view name = key.substr(dot + 1);
    if (name.empty() || name.find('.') != std::string_view::npos) {
        error = ImportOptionError::UnknownName;
        return false;
    }

    ImportOptions candidate = table[format];
    bool known = false;
    if (ImportOptionEqualsIgnoreCase(name, "quality")) {
        known = true;
        if (ImportOptionEqualsIgnoreCase(value, "draft")) candidate.quality = ImportQuality::Draft;
        else if (ImportOptionEqualsIgnoreCase(value, "balanced")) candidate.quality = ImportQuality::Balanced;
        else if (ImportOptionEqualsIgnoreCase(value, "full")) candidate.quality = ImportQuality::Full;
        else {
            error = ImportOptionError::InvalidValue;
            return false;
        }
    } else if (ImportOptionEqualsIgnoreCase(name, "read_wire")) {
        known = true;
        bool enabled = false;
        if (!import_options_detail::ParseBool(value, enabled)) {
            error = ImportOptionError::InvalidValue;
            return false;
        }
        candidate.wireMode = enabled ? ImportWireMode::On : ImportWireMode::Off;
    } else if (ImportOptionEqualsIgnoreCase(name, "linear_deflection")) {
        known = true;
        double parsed = 0.0;
        if (!import_options_detail::ParseFiniteDouble(value, parsed)) {
            error = ImportOptionError::InvalidValue;
            return false;
        }
        candidate.linearDeflection = parsed;
    } else if (ImportOptionEqualsIgnoreCase(name, "angular_deflection")) {
        known = true;
        double parsed = 0.0;
        if (!import_options_detail::ParseFiniteDouble(value, parsed)) {
            error = ImportOptionError::InvalidValue;
            return false;
        }
        candidate.angularDeflection = parsed;
    } else if (ImportOptionEqualsIgnoreCase(name, "relative_deflection")) {
        known = true;
        bool parsed = false;
        if (!import_options_detail::ParseBool(value, parsed)) {
            error = ImportOptionError::InvalidValue;
            return false;
        }
        candidate.relativeDeflection = parsed;
    } else if (ImportOptionEqualsIgnoreCase(name, "unit_scale")) {
        known = true;
        double parsed = 0.0;
        if (!import_options_detail::ParseFiniteDouble(value, parsed)) {
            error = ImportOptionError::InvalidValue;
            return false;
        }
        candidate.unitScaleOverride = parsed;
    } else if (ImportOptionEqualsIgnoreCase(name, "subdivision_level")) {
        known = true;
        uint32_t parsed = 0;
        if (!import_options_detail::ParseUnsigned(value, parsed)) {
            error = ImportOptionError::InvalidValue;
            return false;
        }
        candidate.usdSubdivisionLevel = parsed;
    }
    if (!known) {
        error = ImportOptionError::UnknownName;
        return false;
    }
    if (!ValidateImportOptions(format, candidate, error)) return false;
    table[format] = candidate;
    return true;
}

// Packed control-channel form. `version == 0` is the legacy zero-filled request
// every pre-T06 producer already sends; the host then derives quality from the
// existing STEP flag bits and uses documented defaults for every other field.
constexpr uint32_t kImportOptionsPayloadVersion = 1;

enum : uint32_t {
    kImportOptionQuality = 1u << 0,
    kImportOptionReadWire = 1u << 1,
    kImportOptionLinearDeflection = 1u << 2,
    kImportOptionAngularDeflection = 1u << 3,
    kImportOptionRelativeDeflection = 1u << 4,
    kImportOptionUnitScale = 1u << 5,
    kImportOptionUsdSubdivision = 1u << 6,
    kImportOptionKnownMask = (1u << 7) - 1u,
};

#pragma pack(push, 1)
struct ImportOptionsPayload {
    uint32_t version;             // 0 legacy, kImportOptionsPayloadVersion current
    uint32_t optionsMask;         // closed kImportOption* bits
    uint32_t quality;             // ImportQuality
    uint32_t wireMode;            // ImportWireMode
    uint32_t relativeDeflection;  // 0 or 1
    uint32_t usdSubdivisionLevel;
    double linearDeflection;
    double angularDeflection;
    double unitScaleOverride;
};
static_assert(sizeof(ImportOptionsPayload) == 48,
              "ImportOptionsPayload layout changed");
#pragma pack(pop)

inline ImportOptionsPayload MakeImportOptionsPayload(const ImportOptions& options) noexcept
{
    ImportOptionsPayload payload{};
    payload.version = kImportOptionsPayloadVersion;
    payload.optionsMask = kImportOptionQuality | kImportOptionReadWire
        | kImportOptionRelativeDeflection | kImportOptionUsdSubdivision;
    payload.quality = static_cast<uint32_t>(options.quality);
    payload.wireMode = static_cast<uint32_t>(options.wireMode);
    payload.relativeDeflection = options.relativeDeflection ? 1u : 0u;
    payload.usdSubdivisionLevel = options.usdSubdivisionLevel;
    if (options.linearDeflection) {
        payload.optionsMask |= kImportOptionLinearDeflection;
        payload.linearDeflection = *options.linearDeflection;
    }
    if (options.angularDeflection) {
        payload.optionsMask |= kImportOptionAngularDeflection;
        payload.angularDeflection = *options.angularDeflection;
    }
    if (options.unitScaleOverride) {
        payload.optionsMask |= kImportOptionUnitScale;
        payload.unitScaleOverride = *options.unitScaleOverride;
    }
    return payload;
}

// Reconstructs and re-validates the record inside the worker/host. Returns false
// with a typed reason for an unknown version/mask, an out-of-range enum, a
// non-finite number, or a field that does not apply to `format`.
inline bool ImportOptionsFromPayload(const ImportOptionsPayload& payload,
                                     ImportOptionFormat format,
                                     ImportOptions& options,
                                     ImportOptionError& error) noexcept
{
    error = ImportOptionError::None;
    if (payload.version == 0) {
        options = ImportOptions{};
        return true;
    }
    if (payload.version != kImportOptionsPayloadVersion
        || (payload.optionsMask & ~kImportOptionKnownMask) != 0) {
        error = ImportOptionError::InvalidValue;
        return false;
    }
    if (payload.quality > kImportQualityLast || payload.wireMode > kImportWireModeLast
        || payload.relativeDeflection > 1) {
        error = ImportOptionError::InvalidValue;
        return false;
    }
    ImportOptions parsed;
    parsed.quality = static_cast<ImportQuality>(payload.quality);
    parsed.wireMode = static_cast<ImportWireMode>(payload.wireMode);
    parsed.relativeDeflection = payload.relativeDeflection != 0;
    parsed.usdSubdivisionLevel = payload.usdSubdivisionLevel;
    if (payload.optionsMask & kImportOptionLinearDeflection)
        parsed.linearDeflection = payload.linearDeflection;
    if (payload.optionsMask & kImportOptionAngularDeflection)
        parsed.angularDeflection = payload.angularDeflection;
    if (payload.optionsMask & kImportOptionUnitScale)
        parsed.unitScaleOverride = payload.unitScaleOverride;
    // A field whose mask bit is clear must carry the neutral value. This keeps
    // the wire record canonical and rejects a hostile producer that tries to
    // smuggle a value past the applicability check.
    if (!(payload.optionsMask & kImportOptionQuality)
        && payload.quality != static_cast<uint32_t>(ImportQuality::Balanced)) {
        error = ImportOptionError::InvalidValue;
        return false;
    }
    if (!(payload.optionsMask & kImportOptionReadWire)
        && payload.wireMode != static_cast<uint32_t>(ImportWireMode::Fallback)) {
        error = ImportOptionError::InvalidValue;
        return false;
    }
    if (!(payload.optionsMask & kImportOptionRelativeDeflection)
        && payload.relativeDeflection != 1u) {
        error = ImportOptionError::InvalidValue;
        return false;
    }
    if (!(payload.optionsMask & kImportOptionUsdSubdivision)
        && payload.usdSubdivisionLevel != 0u) {
        error = ImportOptionError::InvalidValue;
        return false;
    }
    if (!(payload.optionsMask & kImportOptionLinearDeflection)
        && payload.linearDeflection != 0.0) {
        error = ImportOptionError::InvalidValue;
        return false;
    }
    if (!(payload.optionsMask & kImportOptionAngularDeflection)
        && payload.angularDeflection != 0.0) {
        error = ImportOptionError::InvalidValue;
        return false;
    }
    if (!(payload.optionsMask & kImportOptionUnitScale) && payload.unitScaleOverride != 0.0) {
        error = ImportOptionError::InvalidValue;
        return false;
    }
    if (!ValidateImportOptions(format, parsed, error)) return false;
    options = parsed;
    return true;
}

namespace import_options_detail {

inline void HashByte(uint32_t& hash, uint8_t byte) noexcept
{
    hash ^= byte;
    hash *= 16777619u;
}

inline void HashU32(uint32_t& hash, uint32_t value) noexcept
{
    for (int shift = 0; shift < 32; shift += 8) HashByte(hash, static_cast<uint8_t>(value >> shift));
}

inline void HashU64(uint32_t& hash, uint64_t value) noexcept
{
    for (int shift = 0; shift < 64; shift += 8) HashByte(hash, static_cast<uint8_t>(value >> shift));
}

inline void HashDouble(uint32_t& hash, double value) noexcept
{
    // Canonicalize negative zero so an equivalent value cannot split a cache
    // entry; every other bit pattern is significant.
    if (value == 0.0) value = 0.0;
    HashU64(hash, static_cast<uint64_t>(std::bit_cast<uint64_t>(value)));
}

} // namespace import_options_detail

// Deterministic 32-bit identity for a derived-cache key. It names the format,
// the closed option values and their explicit/absent state. It deliberately
// carries no path, file name, generation id or user text.
inline uint32_t ImportOptionsCacheIdentity(ImportOptionFormat format,
                                           const ImportOptions& options) noexcept
{
    uint32_t hash = 2166136261u;
    import_options_detail::HashU32(hash, static_cast<uint32_t>(format));
    import_options_detail::HashU32(hash, static_cast<uint32_t>(options.quality));
    import_options_detail::HashU32(hash, static_cast<uint32_t>(options.wireMode));
    import_options_detail::HashU32(hash, options.relativeDeflection ? 1u : 0u);
    import_options_detail::HashU32(hash, options.usdSubdivisionLevel);
    import_options_detail::HashU32(hash, options.linearDeflection ? 1u : 0u);
    if (options.linearDeflection)
        import_options_detail::HashDouble(hash, *options.linearDeflection);
    import_options_detail::HashU32(hash, options.angularDeflection ? 1u : 0u);
    if (options.angularDeflection)
        import_options_detail::HashDouble(hash, *options.angularDeflection);
    import_options_detail::HashU32(hash, options.unitScaleOverride ? 1u : 0u);
    if (options.unitScaleOverride)
        import_options_detail::HashDouble(hash, *options.unitScaleOverride);
    return hash;
}

} // namespace model_core
