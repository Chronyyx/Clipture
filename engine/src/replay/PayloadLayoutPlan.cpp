#include "clipture/replay/PayloadLayoutPlan.hpp"

#include <algorithm>
#include <limits>
#include <stdexcept>

namespace clipture::replay {

PayloadLayoutPlan planPayloadLayout(std::span<const PayloadExtent> samples,
                                    uint64_t outputStart, uint64_t alignment, PayloadPlacement placement) {
    if (alignment && (alignment & (alignment - 1))) {
        throw std::invalid_argument("Payload alignment must be a power of two or zero");
    }
    if (!alignment && placement == PayloadPlacement::AlignRegions) {
        throw std::invalid_argument("Aligned placement requires nonzero alignment");
    }
    PayloadLayoutPlan plan;
    plan.outputStart_ = plan.outputEnd_ = outputStart;
    plan.sampleOffsets_.reserve(samples.size());
    std::vector<PayloadTransfer> runs;
    for (const auto& sample : samples) {
        if (!sample.source || !sample.length) {
            throw std::invalid_argument("Payload sample requires a pinned nonempty range");
        }
        const auto available = sample.source->size();
        if (sample.offset > available || sample.length > available - sample.offset) {
            throw std::out_of_range("Payload sample exceeds committed source bytes");
        }
        if (sample.length > std::numeric_limits<uint64_t>::max() - plan.outputEnd_) {
            throw std::overflow_error("Payload output offsets overflow");
        }
        plan.sampleOffsets_.push_back(plan.outputEnd_);
        if (!runs.empty() && runs.back().input.source == sample.source &&
            runs.back().input.offset + runs.back().input.length == sample.offset) {
            runs.back().input.length += sample.length;
        } else {
            runs.push_back({sample, plan.outputEnd_, PayloadTransferKind::Copy});
        }
        plan.outputEnd_ += sample.length;
    }

    // Compute final placement after coalescing, never padding individual frames.
    // Source bytes and source ranges remain unchanged; only destination offsets move.
    plan.outputEnd_ = outputStart;
    std::size_t sampleIndex = 0;
    for (auto run : runs) {
        uint64_t padding = 0;
        if (placement == PayloadPlacement::AlignRegions) {
            const auto sourceResidue = run.input.offset % alignment;
            const auto destinationResidue = plan.outputEnd_ % alignment;
            const auto head = std::min(run.input.length, sourceResidue ? alignment - sourceResidue : 0);
            const auto body = (run.input.length - head) / alignment * alignment;
            const auto gap = sourceResidue >= destinationResidue
                ? sourceResidue - destinationResidue : alignment - (destinationResidue - sourceResidue);
            if (body > gap) padding = gap;
        }
        if (padding > UINT64_MAX - plan.outputEnd_ ||
            run.input.length > UINT64_MAX - plan.outputEnd_ - padding) {
            throw std::overflow_error("Aligned payload output offsets overflow");
        }
        plan.paddingBytes_ += padding;
        run.outputOffset = plan.outputEnd_ + padding;
        plan.outputEnd_ = run.outputOffset + run.input.length;
        uint64_t sampleCursor = run.outputOffset;
        while (sampleCursor < plan.outputEnd_) {
            plan.sampleOffsets_[sampleIndex] = sampleCursor;
            sampleCursor += samples[sampleIndex++].length;
        }

        // Equal residues allow a copied head, aligned body and copied tail.
        const auto emit = [&](uint64_t length, PayloadTransferKind kind) {
            if (!length) return;
            plan.transfers_.push_back({{run.input.source, run.input.offset, length},
                                       run.outputOffset, kind});
            if (kind == PayloadTransferKind::AlignmentCandidate) plan.candidateBytes_ += length;
            run.input.offset += length;
            run.outputOffset += length;
            run.input.length -= length;
        };
        if (!alignment || run.input.offset % alignment != run.outputOffset % alignment) {
            emit(run.input.length, PayloadTransferKind::Copy);
            continue;
        }
        const auto residue = run.input.offset % alignment;
        emit(std::min(run.input.length, residue ? alignment - residue : 0), PayloadTransferKind::Copy);
        emit(run.input.length - run.input.length % alignment, PayloadTransferKind::AlignmentCandidate);
        emit(run.input.length, PayloadTransferKind::Copy);
    }
    return plan;
}

} // namespace clipture::replay
