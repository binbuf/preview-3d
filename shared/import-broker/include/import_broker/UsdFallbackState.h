#pragma once

#include "model_core/ImportError.h"

#include <cstdint>

namespace import_broker {

// Format-neutral normalized records are shared by both USD producers, but a
// generation's producer identity is closed. USD-006 will use this state at
// the orchestration boundary; USD-003 defines and tests the transitions now
// so fallback cannot evolve into an append/retry convention accidentally.
enum class UsdProducer : uint32_t { None = 0, TinyUsdz = 1, OpenUsd = 2 };
enum class UsdFallbackPhase : uint32_t {
    AwaitingFastClassification,
    FastCommitted,
    AwaitingCompatibility,
    CompatibilityCommitted,
    Complete,
    Failed,
};
enum class UsdFallbackObservation : uint32_t {
    Accepted,
    StartCompatibility, // caller must begin with empty catalogs/sections
    TerminalFailure,
    StaleGeneration,
    ProtocolViolation,
};

class UsdFallbackState {
public:
    explicit constexpr UsdFallbackState(uint64_t generationId) noexcept
        : generationId_(generationId) {}

    constexpr uint64_t GenerationId() const noexcept { return generationId_; }
    constexpr UsdFallbackPhase Phase() const noexcept { return phase_; }

    constexpr UsdFallbackObservation ObserveBatch(uint64_t generationId,
                                                   UsdProducer producer) noexcept
    {
        if (generationId != generationId_) return UsdFallbackObservation::StaleGeneration;
        if (producer == UsdProducer::TinyUsdz
            && phase_ == UsdFallbackPhase::AwaitingFastClassification) {
            phase_ = UsdFallbackPhase::FastCommitted;
            return UsdFallbackObservation::Accepted;
        }
        if (producer == UsdProducer::TinyUsdz && phase_ == UsdFallbackPhase::FastCommitted)
            return UsdFallbackObservation::Accepted;
        if (producer == UsdProducer::OpenUsd
            && phase_ == UsdFallbackPhase::AwaitingCompatibility) {
            phase_ = UsdFallbackPhase::CompatibilityCommitted;
            return UsdFallbackObservation::Accepted;
        }
        if (producer == UsdProducer::OpenUsd
            && phase_ == UsdFallbackPhase::CompatibilityCommitted)
            return UsdFallbackObservation::Accepted;
        return RejectProtocol();
    }

    constexpr UsdFallbackObservation ObserveError(uint64_t generationId,
                                                   UsdProducer producer,
                                                   model_core::ImportErrorCode code) noexcept
    {
        if (generationId != generationId_) return UsdFallbackObservation::StaleGeneration;
        if (producer == UsdProducer::TinyUsdz
            && phase_ == UsdFallbackPhase::AwaitingFastClassification
            && code == model_core::ImportErrorCode::UnsupportedComposition) {
            // No fast batch has committed. The caller discards any private
            // candidate section/catalog and starts OpenUSD from an empty set.
            phase_ = UsdFallbackPhase::AwaitingCompatibility;
            return UsdFallbackObservation::StartCompatibility;
        }
        if (producer == UsdProducer::TinyUsdz
            && (phase_ == UsdFallbackPhase::AwaitingFastClassification
                || phase_ == UsdFallbackPhase::FastCommitted)) {
            if (code == model_core::ImportErrorCode::UnsupportedComposition)
                return RejectProtocol(); // fallback after publication
            phase_ = UsdFallbackPhase::Failed;
            return UsdFallbackObservation::TerminalFailure;
        }
        if (producer == UsdProducer::OpenUsd
            && (phase_ == UsdFallbackPhase::AwaitingCompatibility
                || phase_ == UsdFallbackPhase::CompatibilityCommitted)) {
            if (code == model_core::ImportErrorCode::UnsupportedComposition)
                return RejectProtocol(); // no fallback loops/reverse fallback
            phase_ = UsdFallbackPhase::Failed;
            return UsdFallbackObservation::TerminalFailure;
        }
        return RejectProtocol();
    }

    constexpr UsdFallbackObservation ObserveComplete(uint64_t generationId,
                                                      UsdProducer producer) noexcept
    {
        if (generationId != generationId_) return UsdFallbackObservation::StaleGeneration;
        const bool fast = producer == UsdProducer::TinyUsdz
            && (phase_ == UsdFallbackPhase::AwaitingFastClassification
                || phase_ == UsdFallbackPhase::FastCommitted);
        const bool compatibility = producer == UsdProducer::OpenUsd
            && (phase_ == UsdFallbackPhase::AwaitingCompatibility
                || phase_ == UsdFallbackPhase::CompatibilityCommitted);
        if (!fast && !compatibility) return RejectProtocol();
        phase_ = UsdFallbackPhase::Complete;
        committedProducer_ = producer;
        return UsdFallbackObservation::Accepted;
    }

    constexpr UsdProducer CommittedProducer() const noexcept { return committedProducer_; }

private:
    constexpr UsdFallbackObservation RejectProtocol() noexcept
    {
        phase_ = UsdFallbackPhase::Failed;
        return UsdFallbackObservation::ProtocolViolation;
    }

    uint64_t generationId_ = 0;
    UsdFallbackPhase phase_ = UsdFallbackPhase::AwaitingFastClassification;
    UsdProducer committedProducer_ = UsdProducer::None;
};

} // namespace import_broker
