#pragma once
#include "clipture/replay/CopyPayloadWriter.hpp"

namespace clipture::replay {
class PayloadOutputAttempt : public PayloadCopySink, public PayloadCloner {
public:
    // Implementations must discard unfinished output on destruction, including
    // when a source/sink throws. Exceptions propagate; they never signal success.
    virtual bool finish() = 0; // Flush/close; not a library publication operation.
    virtual bool discard() = 0; // Remove only this attempt's owned file object.
};
class PayloadOutputFactory {
public:
    virtual ~PayloadOutputFactory() = default;
    virtual std::unique_ptr<PayloadOutputAttempt> create(uint64_t outputSize) = 0;
};
enum class PayloadJobError { None, CreateFailed, TransferFailed, FinishFailed, CleanupFailed };
struct PayloadJobResult {
    PayloadJobError error = PayloadJobError::None;
    PayloadCopyResult transfer;
    bool retriedCopy = false;
    uint32_t firstCloneError = 0;
    bool ok() const noexcept { return error == PayloadJobError::None; }
};
// After any clone-operation failure: discard the complete attempt, CREATE_NEW a
// fresh output and retry once with cloning disabled. No retries for disk/read/
// flush failures. The caller owns metadata and publication after success.
PayloadJobResult writePayloadFile(const PayloadLayoutPlan& plan, PayloadOutputFactory& factory,
                                  std::span<std::byte> scratch, bool allowClones = true);
} // namespace clipture::replay
