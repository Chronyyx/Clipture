#include "clipture/replay/InPlacePacketArchive.hpp"
#include "clipture/replay/InPlaceExtent.hpp"
#include "clipture/replay/Mp4SamplePacker.hpp"
#include "clipture/platform/windows/InPlaceFile.hpp"
#include <atomic>
#include <map>

namespace clipture::replay {
namespace {
using platform::windows::InPlaceFile;
struct Arena;
struct Range final : InPlaceExtent {
    std::shared_ptr<Arena> arena;
    uint64_t offset, length;
    Range(std::shared_ptr<Arena> owner, uint64_t begin, uint64_t bytes)
        : arena(std::move(owner)), offset(begin), length(bytes) {}
    ~Range() override;
    uint64_t size() const noexcept override { return offset + length; }
    bool read(uint64_t begin, std::span<std::byte> bytes) const override;
    std::shared_ptr<InPlaceFile> file() const override;
};
struct Reader final : PacketPayloadReader {
    std::shared_ptr<Range> range;
    explicit Reader(std::shared_ptr<Range> value) : range(std::move(value)) {}
    std::size_t size() const noexcept override { return static_cast<std::size_t>(range->length); }
    bool read(std::size_t offset, std::span<std::byte> bytes) const override {
        if (offset > size() || bytes.size() > size() - offset) return false;
        return range->read(range->offset + offset, bytes);
    }
    std::optional<PayloadExtent> extent() const override { return PayloadExtent{range, range->offset, range->length}; }
};

struct Arena : std::enable_shared_from_this<Arena> {
    std::shared_ptr<InPlaceFile> file;
    std::mutex mutex;
    std::map<uint64_t, uint64_t> free; // Coalesced offset -> length, never leased.
    uint64_t end = InPlaceFile::mediaStart, maximum;
    explicit Arena(std::shared_ptr<InPlaceFile> value, uint64_t limit) : file(std::move(value)), maximum(limit) {}
    void release(uint64_t offset, uint64_t length) noexcept try {
        std::lock_guard lock(mutex);
        auto next = free.lower_bound(offset);
        if (next != free.begin()) {
            auto previous = std::prev(next);
            if (previous->first + previous->second == offset) {
                offset = previous->first; length += previous->second; free.erase(previous);
            }
        }
        if (next != free.end() && offset + length == next->first) {
            length += next->second; free.erase(next);
        }
        free.emplace(offset, length);
    } catch (...) {
        // Under allocation pressure, lose reusable capacity rather than throw
        // from a packet lease destructor. The hard arena cap still applies.
    }
    PacketPayloadReaderPtr write(std::span<const std::byte> bytes) {
        const std::size_t lengths[] = {bytes.size()};
        auto readers = writeRun(bytes, lengths);
        return readers.empty() ? PacketPayloadReaderPtr{} : std::move(readers.front());
    }
    // One contiguous slot and one write for several samples; each sample still
    // gets its own lease, so they expire and are reused independently.
    std::vector<PacketPayloadReaderPtr> writeRun(std::span<const std::byte> bytes, std::span<const std::size_t> lengths) {
        std::unique_lock lock(mutex);
        if (bytes.empty() || bytes.size() > 64u * 1024u * 1024u) return {};
        auto slot = std::find_if(free.begin(), free.end(), [&](const auto& item) { return item.second >= bytes.size(); });
        const uint64_t offset = slot == free.end() ? end : slot->first;
        if (offset > maximum || bytes.size() > maximum - offset) return {};
        if (!file->writeSlot(offset, bytes)) return {};
        if (slot != free.end()) {
            const auto remaining = slot->second - bytes.size();
            free.erase(slot);
            if (remaining) free.emplace(offset + bytes.size(), remaining);
        }
        end = std::max(end, offset + bytes.size());
        lock.unlock(); // A failed reader allocation can destroy/release its lease.
        std::vector<PacketPayloadReaderPtr> readers;
        readers.reserve(lengths.size());
        uint64_t next = offset;
        for (const auto length : lengths) {
            readers.push_back(std::make_shared<Reader>(std::make_shared<Range>(shared_from_this(), next, length)));
            next += length;
        }
        return readers;
    }
    bool seal() {
        std::vector<std::pair<uint64_t, uint64_t>> padding;
        uint64_t tail = 0, full = 0;
        {
            std::lock_guard lock(mutex);
            tail = full = end;
            padding.assign(free.begin(), free.end());
            // Space past the last retained sample is left over from when the
            // footage took more bytes (a busier scene, a higher bitrate); the
            // clip ends before it instead of carrying it as padding.
            if (!padding.empty() && padding.back().first + padding.back().second == end) {
                tail = padding.back().first;
                padding.pop_back();
            }
        }
        if (tail != full && !file->trimEnd(tail)) padding.emplace_back(tail, full - tail);
        // Expired slots are padding, not hidden recoverable footage in a shared
        // clip. Zero only unleased ranges; selected/live samples are untouched.
        for (const auto& [offset, length] : padding) {
            if (!file->zeroSlot(offset, length)) return false;
        }
        return file->seal();
    }
};
Range::~Range() { arena->release(offset, length); }
std::shared_ptr<InPlaceFile> Range::file() const { return arena->file; }
bool Range::read(uint64_t begin, std::span<std::byte> bytes) const {
    if (begin < offset || begin > offset + length || bytes.size() > offset + length - begin) return false;
    return arena->file->read(begin, bytes);
}
}

struct InPlacePacketArchive::Impl {
    mutable std::mutex mutex;
    std::filesystem::path folder;
    bool enabled = false;
    uint64_t maximum = 0;
    std::shared_ptr<Arena> active;
    std::vector<std::weak_ptr<Arena>> arenas;
    bool ensureActive() {
        if (active) return true;
        if (!enabled || folder.empty()) return false;
        std::error_code error;
        const auto privateFolder = folder / ".clipture-replay";
        std::filesystem::create_directories(privateFolder, error);
        if (error) return false;
        static std::atomic<uint64_t> sequence{0};
        const auto name = L"buffer-" + std::to_wstring(GetCurrentProcessId()) + L"-" +
            std::to_wstring(GetTickCount64()) + L"-" + std::to_wstring(sequence++) + L".recording";
        auto file = InPlaceFile::create(privateFolder / name);
        if (!file) return false;
        file->discardWhenUnused();
        active = std::make_shared<Arena>(std::move(file), maximum);
        std::erase_if(arenas, [](const auto& value) { return value.expired(); });
        arenas.push_back(active);
        return true;
    }
};
InPlacePacketArchive::InPlacePacketArchive() : impl_(std::make_unique<Impl>()) {}
InPlacePacketArchive::~InPlacePacketArchive() = default;
void InPlacePacketArchive::configure(const std::filesystem::path& folder, bool enabled, uint64_t maximum) {
    std::lock_guard lock(impl_->mutex);
    enabled = enabled && !folder.empty();
    if (folder != impl_->folder || enabled != impl_->enabled || maximum != impl_->maximum) impl_->active.reset();
    impl_->folder = folder;
    impl_->enabled = enabled;
    impl_->maximum = maximum;
}
std::optional<EncodedPacket> InPlacePacketArchive::persist(const EncodedPacket& source) {
    auto prepared = source.kind == PacketKind::Audio && source.codec == PacketCodec::AacLc
        ? std::optional<EncodedPacket>(source) : packMp4VideoSample(source);
    if (!prepared || !prepared->payload) return std::nullopt;
    std::lock_guard lock(impl_->mutex);
    if (!impl_->ensureActive()) return std::nullopt;
    auto reader = impl_->active->write(payloadBytes(*prepared));
    if (!reader) return std::nullopt; // Existing spill path remains the bounded fallback.
    prepared->payloadReader = std::move(reader);
    prepared->payload.reset();
    return prepared;
}
namespace {
std::shared_ptr<InPlaceFile> leasedFile(const EncodedPacket& packet) {
    if (packet.payload || !packet.payloadReader) return {};
    const auto extent = packet.payloadReader->extent();
    const auto lease = extent ? std::dynamic_pointer_cast<const InPlaceExtent>(extent->source) : nullptr;
    return lease ? lease->file() : nullptr;
}
}
bool InPlacePacketArchive::isDetached(const EncodedPacket& packet) const {
    const auto file = leasedFile(packet);
    if (!file) return false;
    std::lock_guard lock(impl_->mutex);
    return impl_->enabled && (!impl_->active || impl_->active->file != file);
}
std::vector<std::optional<EncodedPacket>> InPlacePacketArchive::rehome(std::span<const EncodedPacket> packets) {
    std::vector<std::optional<EncodedPacket>> result(packets.size());
    std::vector<std::size_t> indexes, lengths;
    PacketPayload joined;
    // The source is usually a clip published seconds ago, so these reads are
    // normally served by the file cache. No archive mutex is held during them.
    for (std::size_t i = 0; i < packets.size(); ++i) {
        const auto length = payloadSize(packets[i]);
        if (!isDetached(packets[i]) || length == 0 || length > 64u * 1024u * 1024u - joined.size()) continue;
        const auto start = joined.size();
        joined.resize(start + length);
        if (!readPayload(packets[i], 0, std::span(joined).subspan(start))) {
            joined.resize(start);
            continue;
        }
        indexes.push_back(i);
        lengths.push_back(length);
    }
    if (joined.empty()) return result;
    std::vector<PacketPayloadReaderPtr> readers;
    {
        std::lock_guard lock(impl_->mutex);
        if (!impl_->ensureActive()) return result;
        readers = impl_->active->writeRun(joined, lengths);
    }
    // Full arena: nothing is installed and the next save copies these instead.
    for (std::size_t i = 0; i < readers.size(); ++i) {
        auto moved = packets[indexes[i]];
        moved.payloadReader = std::move(readers[i]);
        result[indexes[i]] = std::move(moved);
    }
    return result;
}
std::unique_ptr<InPlaceMediaBuffer> InPlacePacketArchive::takeForSave() {
    std::shared_ptr<Arena> arena;
    {
        std::lock_guard lock(impl_->mutex);
        if (!impl_->ensureActive()) return {};
        arena = std::exchange(impl_->active, nullptr);
    }
    // No archive/retirement mutex is held during file I/O. New capture packets
    // can persist in a fresh file while the detached file is finalized.
    auto file = arena->file;
    file->retainForRecovery();
    if (!arena->seal()) return {};
    return std::make_unique<InPlaceMediaBuffer>(std::move(file));
}
uint64_t InPlacePacketArchive::diskBytes() const {
    std::lock_guard lock(impl_->mutex);
    uint64_t bytes = 0;
    for (const auto& weak : impl_->arenas) if (const auto arena = weak.lock()) bytes += arena->file->size();
    return bytes;
}
} // namespace clipture::replay
