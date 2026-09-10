#include "clipture/platform/windows/CloneFileInfo.hpp"
#include <winioctl.h>
#include <algorithm>
#include <cstring>

namespace clipture::platform::windows {
CloneFileInfo queryCloneFile(HANDLE file) {
    CloneFileInfo result;
    wchar_t filesystem[64]{};
    DWORD flags = 0;
    FILE_ID_INFO id{};
    FILE_STANDARD_INFO standard{};
    FILE_ATTRIBUTE_TAG_INFO attributes{};
    if (!GetVolumeInformationByHandleW(file, nullptr, 0, nullptr, nullptr, &flags, filesystem, 64) ||
        !GetFileInformationByHandleEx(file, FileIdInfo, &id, sizeof(id)) ||
        !GetFileInformationByHandleEx(file, FileStandardInfo, &standard, sizeof(standard)) ||
        !GetFileInformationByHandleEx(file, FileAttributeTagInfo, &attributes, sizeof(attributes)) ||
        standard.EndOfFile.QuadPart < 0) return result;
    // GUID volume identity from this open handle, not a drive letter or a path supplied by UI.
    std::wstring path(32768, L'\0');
    const auto length = GetFinalPathNameByHandleW(file, path.data(), static_cast<DWORD>(path.size()), VOLUME_NAME_GUID);
    if (!length || length >= path.size()) return result;
    path.resize(length);
    const auto end = path.find(L"}\\");
    if (!path.starts_with(L"\\\\?\\Volume{") || end == std::wstring::npos) return result;
    result.volume = path.substr(0, end + 2);
    std::copy(std::begin(id.FileId.Identifier), std::end(id.FileId.Identifier), result.fileId.begin());
    result.size = static_cast<uint64_t>(standard.EndOfFile.QuadPart);
    result.refs = _wcsicmp(filesystem, L"ReFS") == 0;
    result.refcounting = (flags & FILE_SUPPORTS_BLOCK_REFCOUNTING) != 0;
    constexpr DWORD excluded = FILE_ATTRIBUTE_DIRECTORY | FILE_ATTRIBUTE_SPARSE_FILE |
        FILE_ATTRIBUTE_COMPRESSED | FILE_ATTRIBUTE_ENCRYPTED | FILE_ATTRIBUTE_REPARSE_POINT | FILE_ATTRIBUTE_OFFLINE;
    result.ordinaryFile = !standard.Directory && !(attributes.FileAttributes & excluded);
    result.queried = true;
    if (result.refs && result.refcounting) {
        FSCTL_GET_INTEGRITY_INFORMATION_BUFFER integrity{};
        DWORD returned = 0;
        if (DeviceIoControl(file, FSCTL_GET_INTEGRITY_INFORMATION, nullptr, 0, &integrity, sizeof(integrity), &returned, nullptr) &&
            returned >= sizeof(integrity)) {
            result.integrityKnown = true;
            result.clusterBytes = integrity.ClusterSizeInBytes;
            result.checksum = integrity.ChecksumAlgorithm;
            result.integrityFlags = integrity.Flags;
            result.checksumChunkBytes = integrity.ChecksumChunkSizeInBytes;
        }
    }
    return result;
}

uint64_t maximumCloneRequest(uint32_t clusterBytes) {
    if (!clusterBytes || (clusterBytes & (clusterBytes - 1)) || clusterBytes > (1u << 30)) return 0;
    return (1ULL << 30) / clusterBytes * clusterBytes; // 1 GiB, bounded below the API's 4 GiB limit.
}

CloneEligibility checkCloneRange(const CloneFileInfo& source, const CloneFileInfo& destination,
                                 uint64_t sourceOffset, uint64_t destinationOffset, uint64_t bytes) {
    if (!source.queried || !destination.queried) return CloneEligibility::Unknown;
    if (!source.refs || !destination.refs || !source.refcounting || !destination.refcounting) return CloneEligibility::UnsupportedVolume;
    if (source.volume.empty() || destination.volume.empty()) return CloneEligibility::Unknown;
    if (source.volume != destination.volume) return CloneEligibility::DifferentVolume;
    if (source.fileId == destination.fileId) return CloneEligibility::SameFile;
    if (!source.ordinaryFile || !destination.ordinaryFile) return CloneEligibility::UnsupportedAttributes;
    if (!source.integrityKnown || !destination.integrityKnown) return CloneEligibility::Unknown;
    if (source.clusterBytes != destination.clusterBytes || source.checksum != destination.checksum ||
        source.integrityFlags != destination.integrityFlags || source.checksumChunkBytes != destination.checksumChunkBytes) {
        return CloneEligibility::IntegrityMismatch;
    }
    const auto alignment = source.clusterBytes;
    if (!maximumCloneRequest(alignment) || !bytes || sourceOffset % alignment || destinationOffset % alignment || bytes % alignment ||
        sourceOffset > source.size || bytes > source.size - sourceOffset ||
        destinationOffset > destination.size || bytes > destination.size - destinationOffset ||
        source.size > INT64_MAX || destination.size > INT64_MAX) return CloneEligibility::BadRange;
    return CloneEligibility::Eligible;
}
} // namespace clipture::platform::windows
