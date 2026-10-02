#pragma once

// BasisLZ/ETC1S supercompression global-data preflight (SEC-16b).
//
// KTX-Software 4.4.2's `ktxTexture2_transcodeLzEtc1s` calls the pinned
// basisu 1.16 low-level transcoder's `decode_palettes` and `decode_tables` but
// ignores their boolean return values, and then unconditionally enters
// `transcode_slice`. A BasisLZ container that passes every structural KTX2
// check (`model_core::PreflightKtx2`'s header/level/metadata validation) but
// whose Huffman table blob is malformed makes `decode_tables` fail, leaving
// `basisu_lowlevel_etc1s_transcoder::m_selector_model` empty; `transcode_slice`
// then indexes the empty lookup table and null-derefs
// (`basisu_lowlevel_etc1s_transcoder::transcode_slice`,
// `basisu_transcoder.cpp:8013`). A two-byte mutation of a valid BasisLZ
// container's tables is enough (see `tests/fuzz/corpus/gltf/README.md`).
//
// The low-level transcoder symbols live inside `ktx.lib` and were compiled from
// KTX-Software's *vendored* basisu (`external/basisu`), while the standalone
// `basisu` vcpkg package exposes a different, newer ABI. Calling the vendored
// class from product code would therefore be an ABI mismatch, so this header
// re-implements basisu's bounded table read exactly: `bitwise_decoder`,
// `huffman_decoding_table::init`, and `read_huffman_table`, plus the
// `decode_tables` shape (four tables, then the 13-bit selector history buffer
// size), and the failure points of `decode_palettes` (the four endpoint
// Huffman tables and the selector codebook variant flags). `ValidateEtc1sGlobalData`
// returns false for any global data whose `decode_palettes` or `decode_tables`
// would fail, so `PreflightKtx2` can reject it before
// `ktxTexture2_TranscodeBasis` is ever entered.
//
// The implementation is allocation-free and bounded by the same compile-time
// caps the decoder uses (`kMaxSyms` = 16384 symbols, 31-bit code sizes), so a
// hostile global-data declaration cannot drive unbounded work. It deliberately
// mirrors the pinned decoder's behavior (including its read-past-end-as-zero
// bit reader and its `used_syms > 1` completeness relaxation) rather than
// being merely "stricter", so a valid container is never rejected. Keep it in
// sync if the KTX-Software/basisu pin changes (ADR-0046).

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <span>

namespace model_core {

namespace etc1s_detail {

// Caps mirrored from basisu 1.16 `basisu.h` (BASISD_LIB_VERSION 116).
constexpr std::uint32_t kMaxInternalCodeSize = 31;
constexpr int kFastLookupBits = 10;
constexpr std::uint32_t kFastLookupSize = 1u << kFastLookupBits;
constexpr std::uint32_t kMaxSymsLog2 = 14;
constexpr std::uint32_t kMaxSyms = 1u << kMaxSymsLog2;
constexpr std::uint32_t kTotalCodeLengthCodes = 21;

constexpr int kSmallZeroRunCode = 17;
constexpr int kBigZeroRunCode = 18;
constexpr int kSmallRepeatCode = 19;
constexpr int kBigRepeatCode = 20;

constexpr int kSmallZeroRunSizeMin = 3;
constexpr int kSmallZeroRunExtraBits = 3;
constexpr int kBigZeroRunSizeMin = 11;
constexpr int kBigZeroRunExtraBits = 7;
constexpr int kSmallRepeatSizeMin = 3;
constexpr int kSmallRepeatExtraBits = 2;
constexpr int kBigRepeatSizeMin = 7;
constexpr int kBigRepeatExtraBits = 7;

// basisu::g_huffman_sorted_codelength_codes.
constexpr std::uint8_t kSortedCodeLengthCodes[kTotalCodeLengthCodes]
    = {17, 18, 19, 20, 0, 8, 7, 9, 6, 10, 5, 11, 4, 12, 3, 13, 2, 14, 1, 15, 16};

// basisu::cHuffmanMaxSymsLog2 bits are consumed before the table body begins.
constexpr std::uint32_t kTotalSymsBits = kMaxSymsLog2;
constexpr std::uint32_t kNumCodeLengthCodesBits = 5;
constexpr std::uint32_t kCodeLengthBits = 3;
constexpr std::uint32_t kSelectorHistoryBufSizeBits = 13;

// Bounded port of basist::bitwise_decoder. Reads past the end as zero bytes,
// exactly like the pinned decoder, so a truncated table produces the same
// accept/reject decision as `decode_tables`.
class Etc1sBitReader {
public:
    Etc1sBitReader(const std::byte* data, std::size_t size) noexcept
        : next_(reinterpret_cast<const std::uint8_t*>(data))
        , end_(reinterpret_cast<const std::uint8_t*>(data) + size)
    {
    }

    std::uint32_t GetBits(std::uint32_t numBits) noexcept
    {
        const std::uint32_t bits = PeekBits(numBits);
        bitBuffer_ >>= numBits;
        bitBufferSize_ -= numBits;
        return bits;
    }

    // Mirrors decoder's decode_huffman pre-fill: at least 16 bits buffered.
    void FillTo16() noexcept
    {
        while (bitBufferSize_ < 16) {
            std::uint32_t c = 0;
            if (next_ < end_) {
                c = *next_++;
            }
            bitBuffer_ |= (c << bitBufferSize_);
            bitBufferSize_ += 8;
        }
    }

    std::uint32_t BitBuffer() const noexcept { return bitBuffer_; }
    std::uint32_t BitBufferSize() const noexcept { return bitBufferSize_; }
    void Consume(std::uint32_t numBits) noexcept
    {
        bitBuffer_ >>= numBits;
        bitBufferSize_ -= numBits;
    }

private:
    std::uint32_t PeekBits(std::uint32_t numBits) noexcept
    {
        if (numBits == 0) {
            return 0;
        }
        while (bitBufferSize_ < numBits) {
            std::uint32_t c = 0;
            if (next_ < end_) {
                c = *next_++;
            }
            bitBuffer_ |= (c << bitBufferSize_);
            bitBufferSize_ += 8;
        }
        return bitBuffer_ & ((1u << numBits) - 1u);
    }

    const std::uint8_t* next_;
    const std::uint8_t* end_;
    std::uint32_t bitBuffer_ = 0;
    std::uint32_t bitBufferSize_ = 0;
};

// Bounded port of basist::huffman_decoding_table. `MaxSyms` sizes the fixed
// storage; the pinned decoder grows its `m_tree` vector defensively, but the
// tree can never exceed one node per symbol, so `2 * MaxSyms` is exact.
template <std::uint32_t MaxSyms>
struct Etc1sHuffmanTable {
    std::uint8_t codeSizes[MaxSyms] = {};
    std::int32_t lookup[kFastLookupSize] = {};
    std::int16_t tree[2 * MaxSyms] = {};
    std::uint32_t totalSyms = 0;

    void Clear() noexcept { totalSyms = 0; }
    bool IsValid() const noexcept { return totalSyms != 0; }

    bool Init(std::uint32_t total, const std::uint8_t* codeSizesIn) noexcept
    {
        Clear();
        if (total == 0) {
            return true;
        }
        if (total > MaxSyms) {
            return false;
        }
        totalSyms = total;
        std::memcpy(codeSizes, codeSizesIn, total);
        std::memset(lookup, 0, sizeof(lookup));
        std::memset(tree, 0, sizeof(tree));

        std::uint32_t symsUsingCodeSize[kMaxInternalCodeSize + 1] = {};
        for (std::uint32_t i = 0; i < total; ++i) {
            if (codeSizesIn[i] > kMaxInternalCodeSize) {
                return false;
            }
            ++symsUsingCodeSize[codeSizesIn[i]];
        }

        std::uint32_t nextCode[kMaxInternalCodeSize + 1] = {};
        nextCode[0] = nextCode[1] = 0;
        std::uint32_t usedSyms = 0;
        std::uint32_t running = 0;
        for (std::uint32_t i = 1; i < kMaxInternalCodeSize; ++i) {
            usedSyms += symsUsingCodeSize[i];
            running = ((running + symsUsingCodeSize[i]) << 1);
            nextCode[i + 1] = running;
        }
        // basisu 1.16 relaxation: a single used symbol may form an incomplete
        // (but usable) code.
        if (((1u << kMaxInternalCodeSize) != running) && (usedSyms > 1u)) {
            return false;
        }

        int treeNext = -1;
        for (int symIndex = 0; symIndex < static_cast<int>(total); ++symIndex) {
            const std::uint32_t codeSize = codeSizesIn[symIndex];
            if (codeSize == 0) {
                continue;
            }
            std::uint32_t revCode = 0;
            std::uint32_t curCode = nextCode[codeSize]++;
            for (std::uint32_t l = codeSize; l > 0; --l, curCode >>= 1) {
                revCode = (revCode << 1) | (curCode & 1);
            }

            if (codeSize <= kFastLookupBits) {
                const std::uint32_t k = (codeSize << 16) | static_cast<std::uint32_t>(symIndex);
                while (revCode < kFastLookupSize) {
                    if (lookup[revCode] != 0) {
                        return false;
                    }
                    lookup[revCode] = static_cast<std::int32_t>(k);
                    revCode += (1u << codeSize);
                }
                continue;
            }

            int treeCur = lookup[revCode & (kFastLookupSize - 1)];
            if (treeCur == 0) {
                const std::uint32_t idx = revCode & (kFastLookupSize - 1);
                if (lookup[idx] != 0) {
                    return false;
                }
                lookup[idx] = treeNext;
                treeCur = treeNext;
                treeNext -= 2;
            }
            if (treeCur >= 0) {
                return false;
            }

            revCode >>= (kFastLookupBits - 1);
            for (int j = static_cast<int>(codeSize);
                 j > (kFastLookupBits + 1); --j) {
                treeCur -= ((revCode >>= 1) & 1);
                const int idx = -treeCur - 1;
                if (idx < 0 || idx >= static_cast<int>(2 * MaxSyms)) {
                    return false;
                }
                if (tree[idx] == 0) {
                    tree[idx] = static_cast<std::int16_t>(treeNext);
                    treeCur = treeNext;
                    treeNext -= 2;
                } else {
                    treeCur = tree[idx];
                    if (treeCur >= 0) {
                        return false;
                    }
                }
            }

            treeCur -= ((revCode >>= 1) & 1);
            const int leafIdx = -treeCur - 1;
            if (leafIdx < 0 || leafIdx >= static_cast<int>(2 * MaxSyms)) {
                return false;
            }
            if (tree[leafIdx] != 0) {
                return false;
            }
            tree[leafIdx] = static_cast<std::int16_t>(symIndex);
        }
        return true;
    }
};

using Etc1sCodeLengthTable = Etc1sHuffmanTable<kTotalCodeLengthCodes>;
using Etc1sModelTable = Etc1sHuffmanTable<kMaxSyms>;

inline std::uint32_t DecodeHuffman(Etc1sBitReader& reader,
                                   const Etc1sCodeLengthTable& table) noexcept
{
    reader.FillTo16();
    int sym = table.lookup[reader.BitBuffer() & (kFastLookupSize - 1)];
    int codeLen;
    if (sym >= 0) {
        codeLen = sym >> 16;
        sym &= 0xFFFF;
    } else {
        codeLen = kFastLookupBits;
        do {
            sym = table.tree[(~sym) + ((reader.BitBuffer() >> codeLen) & 1)];
            ++codeLen;
        } while (sym < 0);
    }
    reader.Consume(static_cast<std::uint32_t>(codeLen));
    return static_cast<std::uint32_t>(sym);
}

// Bounded port of basisu's `read_huffman_table`. Returns true exactly when the
// pinned decoder would (including the empty table it returns for
// total_used_syms == 0, which `decode_tables` then rejects as invalid).
inline bool ReadHuffmanTable(Etc1sBitReader& reader, Etc1sModelTable& table) noexcept
{
    table.Clear();
    const std::uint32_t totalUsedSyms = reader.GetBits(kTotalSymsBits);
    if (totalUsedSyms == 0) {
        return true;
    }
    if (totalUsedSyms > kMaxSyms) {
        return false;
    }

    std::uint8_t codeLengthCodeSizes[kTotalCodeLengthCodes] = {};
    const std::uint32_t numCodeLengthCodes = reader.GetBits(kNumCodeLengthCodesBits);
    if (numCodeLengthCodes < 1 || numCodeLengthCodes > kTotalCodeLengthCodes) {
        return false;
    }
    for (std::uint32_t i = 0; i < numCodeLengthCodes; ++i) {
        codeLengthCodeSizes[kSortedCodeLengthCodes[i]]
            = static_cast<std::uint8_t>(reader.GetBits(kCodeLengthBits));
    }

    Etc1sCodeLengthTable codeLengthTable;
    if (!codeLengthTable.Init(kTotalCodeLengthCodes, codeLengthCodeSizes)
        || !codeLengthTable.IsValid()) {
        return false;
    }

    std::uint8_t codeSizes[kMaxSyms] = {};
    std::uint32_t cur = 0;
    while (cur < totalUsedSyms) {
        const int c = static_cast<int>(DecodeHuffman(reader, codeLengthTable));
        if (c <= 16) {
            codeSizes[cur++] = static_cast<std::uint8_t>(c);
        } else if (c == kSmallZeroRunCode) {
            cur += reader.GetBits(kSmallZeroRunExtraBits) + kSmallZeroRunSizeMin;
        } else if (c == kBigZeroRunCode) {
            cur += reader.GetBits(kBigZeroRunExtraBits) + kBigZeroRunSizeMin;
        } else {
            if (cur == 0) {
                return false;
            }
            std::uint32_t run;
            if (c == kSmallRepeatCode) {
                run = reader.GetBits(kSmallRepeatExtraBits) + kSmallRepeatSizeMin;
            } else if (c == kBigRepeatCode) {
                run = reader.GetBits(kBigRepeatExtraBits) + kBigRepeatSizeMin;
            } else {
                return false;
            }
            const std::uint8_t prev = codeSizes[cur - 1];
            if (prev == 0) {
                return false;
            }
            do {
                if (cur >= totalUsedSyms) {
                    return false;
                }
                codeSizes[cur++] = prev;
            } while (--run > 0);
        }
    }
    if (cur != totalUsedSyms) {
        return false;
    }
    return table.Init(totalUsedSyms, codeSizes);
}

// Returns true exactly when basisu's `decode_palettes` would. The endpoint
// decode loop and the selector decode loop have no failure returns of their
// own; only the four endpoint Huffman-table reads and the selector-codebook
// header/tables can fail (and the selector reader is re-initialized, so the
// endpoint loop's bit consumption is irrelevant to the decision). We therefore
// replay just those deciding reads, reusing one scratch table.
inline bool ValidateEtc1sPalettes(std::span<const std::byte> endpointsData,
                                  std::span<const std::byte> selectorsData,
                                  std::uint32_t selectorCount) noexcept
{
    Etc1sModelTable scratch;

    Etc1sBitReader endpoints(endpointsData.data(), endpointsData.size());
    constexpr int kEndpointModels = 4; // color5 delta 0/1/2 + intensity delta.
    for (int i = 0; i < kEndpointModels; ++i) {
        if (!ReadHuffmanTable(endpoints, scratch) || !scratch.IsValid()) {
            return false;
        }
    }

    Etc1sBitReader selectors(selectorsData.data(), selectorsData.size());
    if (selectors.GetBits(1) == 1) { // global selector codebook: unsupported
        return false;
    }
    if (selectors.GetBits(1) == 1) { // hybrid global selector codebook: unsupported
        return false;
    }
    if (selectors.GetBits(1) == 1) { // raw selector codebook: cannot fail
        return true;
    }
    if (!ReadHuffmanTable(selectors, scratch)) {
        return false;
    }
    // basisu only requires the delta-selector model when there is more than one
    // selector.
    return selectorCount <= 1 || scratch.IsValid();
}

// BasisLZ global-data layout (KTX2 spec / basis_sgd.h): a 20-byte header, then
// `imageCount` 20-byte image descriptors, then the endpoint codebook, selector
// codebook, Huffman tables and (optional) extended data. `imageCount` is the
// KTX2 mip count for the 2D, single-face, single-layer containers
// `PreflightKtx2` admits.
constexpr std::size_t kBasisLzHeaderBytes = 20;
constexpr std::size_t kBasisLzImageDescBytes = 20;

} // namespace etc1s_detail

// Validates one KTX2 BasisLZ supercompression-global-data blob. `imageCount` is
// the number of image descriptors the KTX2 decoder derives from the container
// (the mip count for the admitted 2D/single-face/single-layer shape). Returns
// false when the codebook lengths do not fit, the counts are empty, or the
// global data would make basisu's `decode_palettes`/`decode_tables` fail --
// i.e. exactly the cases that currently reach the crashing `transcode_slice`.
inline bool ValidateEtc1sGlobalData(std::span<const std::byte> sgd,
                                    std::uint32_t imageCount) noexcept
{
    using namespace etc1s_detail;

    if (sgd.size() < kBasisLzHeaderBytes) {
        return false;
    }
    const auto* base = reinterpret_cast<const std::uint8_t*>(sgd.data());
    const auto u16 = [base](std::size_t offset) {
        std::uint16_t value = 0;
        std::memcpy(&value, base + offset, sizeof(value));
        return value;
    };
    const auto u32 = [base](std::size_t offset) {
        std::uint32_t value = 0;
        std::memcpy(&value, base + offset, sizeof(value));
        return value;
    };

    const std::uint32_t endpointCount = u16(0);
    const std::uint32_t selectorCount = u16(2);
    const std::uint64_t endpointsByteLength = u32(4);
    const std::uint64_t selectorsByteLength = u32(8);
    const std::uint64_t tablesByteLength = u32(12);
    // extendedByteLength is optional trailing data the decoder ignores.

    if (endpointCount == 0 || selectorCount == 0
        || endpointsByteLength == 0 || selectorsByteLength == 0
        || tablesByteLength == 0) {
        return false;
    }

    const std::uint64_t descriptors
        = kBasisLzHeaderBytes + kBasisLzImageDescBytes * std::uint64_t(imageCount);
    if (descriptors > sgd.size()) {
        return false;
    }
    const std::uint64_t tablesEnd = descriptors + endpointsByteLength + selectorsByteLength
        + tablesByteLength;
    if (tablesEnd > sgd.size()) {
        return false;
    }

    const std::size_t endpointsOffset = static_cast<std::size_t>(descriptors);
    const std::size_t selectorsOffset
        = endpointsOffset + static_cast<std::size_t>(endpointsByteLength);
    if (!ValidateEtc1sPalettes(
            sgd.subspan(endpointsOffset, static_cast<std::size_t>(endpointsByteLength)),
            sgd.subspan(selectorsOffset, static_cast<std::size_t>(selectorsByteLength)),
            selectorCount)) {
        return false;
    }

    const std::size_t tablesOffset = selectorsOffset + static_cast<std::size_t>(selectorsByteLength);
    Etc1sBitReader reader(sgd.data() + tablesOffset,
                          static_cast<std::size_t>(tablesByteLength));

    Etc1sModelTable model;
    constexpr int kTableCount = 4; // endpoint pred, delta endpoint, selector, history RLE.
    for (int i = 0; i < kTableCount; ++i) {
        if (!ReadHuffmanTable(reader, model) || !model.IsValid()) {
            return false;
        }
    }

    const std::uint32_t selectorHistoryBufSize = reader.GetBits(kSelectorHistoryBufSizeBits);
    return selectorHistoryBufSize != 0;
}

} // namespace model_core