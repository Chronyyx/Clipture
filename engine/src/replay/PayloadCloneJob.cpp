#include "clipture/replay/PayloadCloneJob.hpp"

namespace clipture::replay {
PayloadJobResult writePayloadFile(const PayloadLayoutPlan& plan, PayloadOutputFactory& factory,
                                  std::span<std::byte> scratch, bool allowClones) {
    PayloadJobResult result;
    for (int attempt = 0; attempt < 2; ++attempt) {
        auto output = factory.create(plan.outputEnd());
        if (!output) { result.error = PayloadJobError::CreateFailed; return result; }
        result.transfer = transferPayload(plan, *output, scratch, allowClones ? output.get() : nullptr);
        if (!result.transfer.ok()) {
            if (!output->discard()) { result.error = PayloadJobError::CleanupFailed; return result; }
            if (result.transfer.error == PayloadCopyError::CloneFailed && allowClones) {
                result.firstCloneError = result.transfer.nativeCloneError;
                result.retriedCopy = true;
                allowClones = false;
                continue;
            }
            result.error = PayloadJobError::TransferFailed;
            return result;
        }
        if (!output->finish()) {
            result.error = output->discard() ? PayloadJobError::FinishFailed : PayloadJobError::CleanupFailed;
        }
        return result;
    }
    result.error = PayloadJobError::TransferFailed;
    return result;
}
} // namespace clipture::replay
