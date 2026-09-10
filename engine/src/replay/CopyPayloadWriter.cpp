#include "clipture/replay/CopyPayloadWriter.hpp"

#include <algorithm>

namespace clipture::replay {

PayloadCopyResult copyPayload(const PayloadLayoutPlan& plan, PayloadCopySink& sink,
                              std::span<std::byte> scratch) {
    return transferPayload(plan, sink, scratch, nullptr);
}

PayloadCopyResult transferPayload(const PayloadLayoutPlan& plan, PayloadCopySink& sink,
                                  std::span<std::byte> scratch, PayloadCloner* cloner) {
    PayloadCopyResult result;
    if (plan.payloadBytes() && scratch.empty()) {
        result.error = PayloadCopyError::EmptyScratch;
        return result;
    }
    uint64_t cursor = plan.outputStart();
    for (const auto& transfer : plan.transfers()) {
        if (cursor < transfer.outputOffset) std::fill(scratch.begin(), scratch.end(), std::byte{0});
        while (cursor < transfer.outputOffset) {
            const auto count = static_cast<std::size_t>(std::min<uint64_t>(scratch.size(), transfer.outputOffset - cursor));
            if (!sink.write(cursor, scratch.first(count))) {
                result.error = PayloadCopyError::WriteFailed;
                return result;
            }
            cursor += count;
            result.paddingBytes += count;
        }
        if (cloner && transfer.kind == PayloadTransferKind::AlignmentCandidate) {
            ++result.cloneAttempts;
            const auto cloned = cloner->clone(transfer.input, transfer.outputOffset);
            if (cloned.status == CloneStatus::Failed) {
                result.error = PayloadCopyError::CloneFailed;
                result.nativeCloneError = cloned.nativeError;
                return result;
            }
            if (cloned.status == CloneStatus::Cloned) {
                result.clonedBytes += transfer.input.length;
                cursor = transfer.outputOffset + transfer.input.length;
                continue;
            }
            ++result.unsupportedRanges;
        }
        uint64_t done = 0;
        while (done < transfer.input.length) {
            const auto count = static_cast<std::size_t>(
                std::min<uint64_t>(scratch.size(), transfer.input.length - done));
            const auto chunk = scratch.first(count);
            if (!transfer.input.source->read(transfer.input.offset + done, chunk)) {
                result.error = PayloadCopyError::ReadFailed;
                return result;
            }
            if (!sink.write(transfer.outputOffset + done, chunk)) {
                result.error = PayloadCopyError::WriteFailed;
                return result;
            }
            done += count;
            result.completedBytes += count;
        }
        cursor = transfer.outputOffset + transfer.input.length;
    }
    return result;
}

} // namespace clipture::replay
