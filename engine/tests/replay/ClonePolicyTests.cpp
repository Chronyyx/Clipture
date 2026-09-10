#include "ExtentTestSupport.hpp"
#include "clipture/platform/windows/CloneFileInfo.hpp"

namespace replay_tests {
using namespace clipture::platform::windows;
void testClonePolicy() {
    CloneFileInfo source;
    source.queried = source.refs = source.refcounting = source.ordinaryFile = source.integrityKnown = true;
    source.volume = L"fixture-volume";
    source.fileId[0] = 1;
    source.size = 8ULL << 30;
    source.clusterBytes = 4096;
    auto destination = source;
    destination.fileId[0] = 2;
    const auto check = [&](const CloneFileInfo& value) { return checkCloneRange(source, value, 0, 0, 4096); };
    require(check(destination) == CloneEligibility::Eligible, "eligible distinct aligned files");
    require(check(source) == CloneEligibility::SameFile, "hard-link aliases must not bypass source protection");
    auto changed = destination;
    changed.volume = L"another-volume";
    require(check(changed) == CloneEligibility::DifferentVolume, "same filesystem is not same volume");
    changed = destination; changed.refs = false;
    require(check(changed) == CloneEligibility::UnsupportedVolume, "NTFS is copy-only");
    changed = destination; changed.refcounting = false;
    require(check(changed) == CloneEligibility::UnsupportedVolume, "name alone does not establish capability");
    changed = destination; changed.queried = false;
    require(check(changed) == CloneEligibility::Unknown, "failed metadata query fails closed");
    changed = destination; changed.integrityKnown = false;
    require(check(changed) == CloneEligibility::Unknown, "unknown integrity fails closed");
    changed = destination; changed.checksum = 2;
    require(check(changed) == CloneEligibility::IntegrityMismatch, "integrity mismatch copies");
    changed = destination; changed.ordinaryFile = false;
    require(check(changed) == CloneEligibility::UnsupportedAttributes, "sparse/encrypted/compressed/reparse files copy");
    require(checkCloneRange(source, destination, 1, 0, 4096) == CloneEligibility::BadRange, "source alignment");
    require(checkCloneRange(source, destination, 0, 1, 4096) == CloneEligibility::BadRange, "destination alignment");
    require(checkCloneRange(source, destination, 0, 0, 4095) == CloneEligibility::BadRange, "length alignment");
    require(checkCloneRange(source, destination, 0, destination.size, 4096) == CloneEligibility::BadRange, "destination must already be extended");
    require(checkCloneRange(source, destination, UINT64_MAX - 4095, 0, 4096) == CloneEligibility::BadRange, "overflowing source range");
    require(maximumCloneRequest(0) == 0 && maximumCloneRequest(3) == 0 && maximumCloneRequest(1u << 31) == 0,
            "invalid or excessive cluster sizes are rejected");
    for (const uint32_t cluster : {4096u, 65536u}) {
        const auto limit = maximumCloneRequest(cluster);
        require(limit > 0 && limit < (1ULL << 32) && limit % cluster == 0, "bounded aligned requests strictly below 4 GiB");
        uint64_t remaining = 5ULL << 30, operations = 0;
        while (remaining) {
            const auto count = std::min(limit, remaining);
            require(count % cluster == 0 && count < (1ULL << 32), "large transfer split stays within request limit");
            remaining -= count; ++operations;
        }
        require(operations == 5, "five 1 GiB operations, without allocating a 5 GiB fixture");
    }
}
} // namespace replay_tests
