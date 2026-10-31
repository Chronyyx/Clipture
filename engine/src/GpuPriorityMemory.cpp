#include "clipture/GpuPriorityMemory.hpp"

#include <Windows.h>
#include <bcrypt.h>

#include <algorithm>
#include <fstream>
#include <iostream>
#include <sstream>

namespace clipture {
namespace {

constexpr const char* kHeader = "clipture-gpu-priority v1";
constexpr std::size_t kKeyBytes = 32;
constexpr std::size_t kIdHexLength = 64;

std::string toHex(const unsigned char* bytes, std::size_t count) {
    static constexpr char digits[] = "0123456789abcdef";
    std::string hex;
    hex.reserve(count * 2);
    for (std::size_t i = 0; i < count; ++i) {
        hex.push_back(digits[bytes[i] >> 4]);
        hex.push_back(digits[bytes[i] & 0x0f]);
    }
    return hex;
}

bool isHex(const std::string& text, std::size_t length) {
    return text.size() == length && std::all_of(text.begin(), text.end(), [](char c) {
        return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f');
    });
}

std::vector<unsigned char> fromHex(const std::string& hex) {
    std::vector<unsigned char> bytes(hex.size() / 2);
    for (std::size_t i = 0; i < bytes.size(); ++i) {
        bytes[i] = static_cast<unsigned char>(std::stoi(hex.substr(i * 2, 2), nullptr, 16));
    }
    return bytes;
}

std::string lowerAscii(std::string text) {
    std::transform(text.begin(), text.end(), text.begin(), [](unsigned char c) {
        return static_cast<char>(c >= 'A' && c <= 'Z' ? c - 'A' + 'a' : c);
    });
    return text;
}

std::wstring widen(const std::string& value) {
    if (value.empty()) return {};
    const int needed = MultiByteToWideChar(CP_UTF8, 0, value.c_str(), -1, nullptr, 0);
    if (needed <= 1) return {};
    std::wstring result(static_cast<std::size_t>(needed - 1), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, value.c_str(), -1, result.data(), needed);
    return result;
}

std::string newKeyHex() {
    unsigned char key[kKeyBytes] {};
    if (!BCRYPT_SUCCESS(BCryptGenRandom(nullptr, key, sizeof(key), BCRYPT_USE_SYSTEM_PREFERRED_RNG))) return {};
    return toHex(key, sizeof(key));
}

}  // namespace

std::optional<GpuPriorityMemory::Contents> GpuPriorityMemory::parse(const std::string& text) {
    std::istringstream lines(text);
    std::string line;
    auto next = [&] {
        if (!std::getline(lines, line)) return false;
        if (!line.empty() && line.back() == '\r') line.pop_back();
        return true;
    };
    if (!next() || line != kHeader) return std::nullopt;
    if (!next() || line.rfind("key ", 0) != 0) return std::nullopt;
    Contents contents;
    contents.keyHex = line.substr(4);
    if (!isHex(contents.keyHex, kKeyBytes * 2)) return std::nullopt;
    while (next()) {
        if (isHex(line, kIdHexLength) &&
            std::find(contents.ids.begin(), contents.ids.end(), line) == contents.ids.end()) {
            contents.ids.push_back(line);
        }
    }
    return contents;
}

std::string GpuPriorityMemory::format(const Contents& contents) {
    std::string text = std::string(kHeader) + "\nkey " + contents.keyHex + "\n";
    for (const auto& id : contents.ids) text += id + "\n";
    return text;
}

std::string GpuPriorityMemory::gameId(const std::string& keyHex, const std::string& exeName, const std::string& gpuIdentity) {
    const auto key = fromHex(keyHex);
    const std::string message = "v1|" + lowerAscii(exeName) + "|" + gpuIdentity;
    BCRYPT_ALG_HANDLE algorithm = nullptr;
    if (!BCRYPT_SUCCESS(BCryptOpenAlgorithmProvider(&algorithm, BCRYPT_SHA256_ALGORITHM, nullptr, BCRYPT_ALG_HANDLE_HMAC_FLAG))) {
        return {};
    }
    unsigned char digest[32] {};
    BCRYPT_HASH_HANDLE hash = nullptr;
    bool ok = BCRYPT_SUCCESS(BCryptCreateHash(algorithm, &hash, nullptr, 0,
        const_cast<PUCHAR>(key.data()), static_cast<ULONG>(key.size()), 0));
    ok = ok && BCRYPT_SUCCESS(BCryptHashData(hash,
        reinterpret_cast<PUCHAR>(const_cast<char*>(message.data())), static_cast<ULONG>(message.size()), 0));
    ok = ok && BCRYPT_SUCCESS(BCryptFinishHash(hash, digest, sizeof(digest), 0));
    if (hash) BCryptDestroyHash(hash);
    BCryptCloseAlgorithmProvider(algorithm, 0);
    return ok ? toHex(digest, sizeof(digest)) : std::string {};
}

void GpuPriorityMemory::open(const std::string& path, std::string gpuIdentity) {
    path_ = widen(path);
    gpuIdentity_ = std::move(gpuIdentity);
    keyHex_.clear();
    ids_.clear();
    if (path_.empty()) return;
    std::ifstream file(path_, std::ios::binary);
    if (!file) return;
    std::ostringstream text;
    text << file.rdbuf();
    if (const auto contents = parse(text.str())) {
        keyHex_ = contents->keyHex;
        ids_ = contents->ids;
    } else {
        std::cerr << "[engine] GPU priority memory unreadable; starting empty.\n";
    }
}

bool GpuPriorityMemory::contains(const std::string& exeName) const {
    if (keyHex_.empty() || exeName.empty()) return false;
    const auto id = gameId(keyHex_, exeName, gpuIdentity_);
    return !id.empty() && std::find(ids_.begin(), ids_.end(), id) != ids_.end();
}

bool GpuPriorityMemory::remember(const std::string& exeName) {
    if (path_.empty() || exeName.empty() || contains(exeName)) return false;
    if (keyHex_.empty()) {
        keyHex_ = newKeyHex();
        if (keyHex_.empty()) return false;
    }
    const auto id = gameId(keyHex_, exeName, gpuIdentity_);
    if (id.empty()) return false;
    ids_.push_back(id);
    if (ids_.size() > kMaximumGames) ids_.erase(ids_.begin(), ids_.begin() + (ids_.size() - kMaximumGames));

    std::error_code ignored;
    std::filesystem::create_directories(path_.parent_path(), ignored);
    auto temporary = path_;
    temporary += L".tmp";
    {
        std::ofstream file(temporary, std::ios::binary | std::ios::trunc);
        file << format({ keyHex_, ids_ });
        if (!file.flush()) return false;
    }
    if (!MoveFileExW(temporary.c_str(), path_.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
        std::filesystem::remove(temporary, ignored);
        return false;
    }
    return true;
}

}  // namespace clipture
