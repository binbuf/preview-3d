#pragma once

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <span>
#include <vector>
#include <ppl.h>

namespace model_core {

inline uint64_t Fnv1a64Append(uint64_t hash, std::span<const std::byte> data) noexcept
{
    constexpr uint64_t kPrime = 1099511628211ULL;
    for (std::byte b : data) {
        hash ^= static_cast<uint64_t>(b);
        hash *= kPrime;
    }
    return hash;
}

// FNV-1a 64-bit. Hand-written because vcpkg.json pins only catch2 and no
// checksum library is otherwise available. Deliberately non-cryptographic:
// a worker that computes its own checksum can forge any checksum, so no
// checksum algorithm stops a lying worker -- the real defense is the
// copy-then-validate bounds/arithmetic checks in
// shared/import-broker/SharedSectionValidator.h, proven adversarially by the
// (deferred) synthetic hostile-worker suite. This only catches incidental
// corruption on the honest-worker path.
inline uint64_t Fnv1a64(std::span<const std::byte> data) noexcept
{
    constexpr uint64_t kOffsetBasis = 14695981039346656037ULL;
    return Fnv1a64Append(kOffsetBasis, data);
}

// Protocol-v10 uses XXH64 directly for small payloads and a fixed 256-KiB XXH64
// tree for large payloads. The fixed leaves make the result independent of span
// boundaries while allowing large normalized scans to validate in parallel.
// This remains an integrity checksum, not an authentication primitive: the
// AppContainer boundary and strict host validation contain a dishonest worker.
class WireChecksum64State {
public:
    void Update(std::span<const std::byte> data) noexcept
    {
        totalLength_ += data.size();
        if (bufferSize_ + data.size() < sizeof(buffer_)) {
            std::memcpy(buffer_ + bufferSize_, data.data(), data.size());
            bufferSize_ += data.size();
            return;
        }
        if (bufferSize_) {
            const size_t fill=sizeof(buffer_)-bufferSize_;
            std::memcpy(buffer_+bufferSize_,data.data(),fill);
            Consume(buffer_);
            data=data.subspan(fill);
            bufferSize_=0;
        }
        while (data.size() >= sizeof(buffer_)) {
            Consume(data.data());
            data=data.subspan(sizeof(buffer_));
        }
        if (!data.empty()) {
            std::memcpy(buffer_,data.data(),data.size());
            bufferSize_=data.size();
        }
    }

    uint64_t Digest() const noexcept
    {
        uint64_t hash;
        if (totalLength_ >= sizeof(buffer_)) {
            hash=RotateLeft(v1_,1)+RotateLeft(v2_,7)+RotateLeft(v3_,12)+RotateLeft(v4_,18);
            hash=Merge(hash,v1_); hash=Merge(hash,v2_); hash=Merge(hash,v3_); hash=Merge(hash,v4_);
        } else {
            hash=kPrime5;
        }
        hash+=totalLength_;
        const std::byte* at=buffer_;
        size_t remaining=bufferSize_;
        while (remaining>=8) {
            hash^=Round(0,Read64(at));
            hash=RotateLeft(hash,27)*kPrime1+kPrime4;
            at+=8; remaining-=8;
        }
        if (remaining>=4) {
            hash^=uint64_t(Read32(at))*kPrime1;
            hash=RotateLeft(hash,23)*kPrime2+kPrime3;
            at+=4; remaining-=4;
        }
        while (remaining--) {
            hash^=uint64_t(static_cast<unsigned char>(*at++))*kPrime5;
            hash=RotateLeft(hash,11)*kPrime1;
        }
        hash^=hash>>33; hash*=kPrime2;
        hash^=hash>>29; hash*=kPrime3;
        hash^=hash>>32;
        return hash;
    }

private:
    static constexpr uint64_t kPrime1=11400714785074694791ull;
    static constexpr uint64_t kPrime2=14029467366897019727ull;
    static constexpr uint64_t kPrime3=1609587929392839161ull;
    static constexpr uint64_t kPrime4=9650029242287828579ull;
    static constexpr uint64_t kPrime5=2870177450012600261ull;
    static uint64_t RotateLeft(uint64_t value,unsigned count) noexcept
    {
        return (value<<count)|(value>>(64-count));
    }
    static uint64_t Read64(const std::byte* data) noexcept
    {
        uint64_t value; std::memcpy(&value,data,sizeof(value)); return value;
    }
    static uint32_t Read32(const std::byte* data) noexcept
    {
        uint32_t value; std::memcpy(&value,data,sizeof(value)); return value;
    }
    static uint64_t Round(uint64_t value,uint64_t input) noexcept
    {
        value+=input*kPrime2; value=RotateLeft(value,31); return value*kPrime1;
    }
    static uint64_t Merge(uint64_t hash,uint64_t value) noexcept
    {
        hash^=Round(0,value); return hash*kPrime1+kPrime4;
    }
    void Consume(const std::byte* data) noexcept
    {
        v1_=Round(v1_,Read64(data)); v2_=Round(v2_,Read64(data+8));
        v3_=Round(v3_,Read64(data+16)); v4_=Round(v4_,Read64(data+24));
    }

    uint64_t totalLength_=0;
    uint64_t v1_=kPrime1+kPrime2;
    uint64_t v2_=kPrime2;
    uint64_t v3_=0;
    uint64_t v4_=0-kPrime1;
    std::byte buffer_[32]{};
    size_t bufferSize_=0;
};

inline uint64_t WireChecksum64Serial(std::span<const std::byte> data) noexcept
{
    WireChecksum64State state; state.Update(data); return state.Digest();
}

inline uint64_t WireChecksum64(std::span<const std::byte> first,
                               std::span<const std::byte> second)
{
    constexpr size_t kParallelThreshold=10*1024*1024,kBlockBytes=256*1024;
    const size_t total=first.size()+second.size();
    if (total<kParallelThreshold) {
        WireChecksum64State state; state.Update(first);state.Update(second);return state.Digest();
    }
    const size_t blocks=(total+kBlockBytes-1)/kBlockBytes;
    std::vector<uint64_t> hashes(blocks);
    concurrency::parallel_for(size_t(0),blocks,[&](size_t block) {
        const size_t offset=block*kBlockBytes,length=(std::min)(kBlockBytes,total-offset);
        if (offset+length<=first.size())
            hashes[block]=WireChecksum64Serial(first.subspan(offset,length));
        else if (offset>=first.size())
            hashes[block]=WireChecksum64Serial(second.subspan(offset-first.size(),length));
        else {
            const size_t fromFirst=first.size()-offset;
            WireChecksum64State state;state.Update(first.subspan(offset,fromFirst));
            state.Update(second.first(length-fromFirst));hashes[block]=state.Digest();
        }
    });
    WireChecksum64State root;
    const uint64_t totalBytes=total;
    root.Update(std::as_bytes(std::span(hashes)));
    root.Update(std::as_bytes(std::span(&totalBytes,1)));
    return root.Digest();
}

inline uint64_t WireChecksum64(std::span<const std::byte> data)
{
    return WireChecksum64(data,{});
}

} // namespace model_core
