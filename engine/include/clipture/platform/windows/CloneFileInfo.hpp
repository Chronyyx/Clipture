#pragma once
#include <Windows.h>
#include <array>
#include <cstdint>
#include <string>

namespace clipture::platform::windows {
struct CloneFileInfo {
    bool queried = false;
    bool refs = false;
    bool refcounting = false;
    bool ordinaryFile = false;
    bool integrityKnown = false;
    std::wstring volume;
    std::array<unsigned char, 16> fileId{};
    uint64_t size = 0;
    uint32_t clusterBytes = 0;
    uint16_t checksum = 0;
    uint32_t integrityFlags = 0;
    uint32_t checksumChunkBytes = 0;
};
enum class CloneEligibility { Eligible, Unknown, UnsupportedVolume, DifferentVolume, SameFile, UnsupportedAttributes, IntegrityMismatch, BadRange };
CloneFileInfo queryCloneFile(HANDLE file);
CloneEligibility checkCloneRange(const CloneFileInfo& source, const CloneFileInfo& destination,
                                 uint64_t sourceOffset, uint64_t destinationOffset, uint64_t bytes);
// A bounded, aligned request strictly below 4 GiB. Zero means invalid cluster size.
uint64_t maximumCloneRequest(uint32_t clusterBytes);
} // namespace clipture::platform::windows
