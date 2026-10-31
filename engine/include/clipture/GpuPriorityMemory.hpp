#pragma once
#include <cstddef>
#include <filesystem>
#include <optional>
#include <string>
#include <vector>

namespace clipture {

// Games that needed the realtime GPU class on this PC (ADR 0029), so the next
// session of the game starts raised instead of stuttering until starvation is
// seen again. The file holds a random per-install key and, per game, an
// HMAC-SHA256 of the executable name and the GPU's hardware IDs: no game name
// is stored, a lookup hashes the running game and compares, and the IDs cannot
// be matched against a list of known games without the key. A new GPU gets
// fresh entries.
class GpuPriorityMemory {
public:
    static constexpr std::size_t kMaximumGames = 512;

    // Loads the file at the UTF-8 `path` (empty disables the memory). A missing
    // or unreadable file starts empty; the key is created on the first remember.
    void open(const std::string& path, std::string gpuIdentity);
    bool enabled() const { return !path_.empty(); }
    bool contains(const std::string& exeName) const;
    // Adds the game and rewrites the file; false if it could not be written.
    bool remember(const std::string& exeName);
    std::size_t size() const { return ids_.size(); }

    // File format, exposed for tests.
    struct Contents {
        std::string keyHex;
        std::vector<std::string> ids;
    };
    static std::optional<Contents> parse(const std::string& text);
    static std::string format(const Contents& contents);
    // Hex HMAC-SHA256 of "v1|<lowercase exe>|<gpu identity>" under the key.
    static std::string gameId(const std::string& keyHex, const std::string& exeName, const std::string& gpuIdentity);

private:
    std::filesystem::path path_;
    std::string gpuIdentity_;
    std::string keyHex_;
    std::vector<std::string> ids_;
};

}  // namespace clipture
