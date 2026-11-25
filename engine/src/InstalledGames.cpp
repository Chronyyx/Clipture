#include "clipture/InstalledGames.hpp"

#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <fstream>
#include <regex>
#include <sstream>
#include <string_view>
#include <system_error>
#include <utility>

namespace clipture {
namespace {

std::string lowerAscii(std::string value) {
    std::transform(value.begin(), value.end(), value.begin(), [](unsigned char ch) {
        return static_cast<char>(std::tolower(ch));
    });
    return value;
}

// Normalized once per path; matching runs for every process and game.
std::string comparablePath(const std::filesystem::path& path) {
    try {
        return lowerAscii(path.lexically_normal().string());
    } catch (const std::exception&) {
        return {};  // not representable in the process code page
    }
}

std::string readTextFile(const std::filesystem::path& path) {
    std::ifstream file(path, std::ios::binary);
    if (!file) return {};
    std::ostringstream content;
    content << file.rdbuf();
    return content.str();
}

// A quoted field of a Steam VDF ("key" "value") or JSON ("key": "value") file.
std::string extractQuotedField(const std::string& text, const std::string& field) {
    const std::regex pattern("\"" + field + "\"\\s+\"([^\"]+)\"");
    std::smatch match;
    if (std::regex_search(text, match, pattern) && match.size() > 1) return match[1].str();
    const std::regex jsonPattern("\"" + field + "\"\\s*:\\s*\"([^\"]+)\"");
    if (std::regex_search(text, match, jsonPattern) && match.size() > 1) return match[1].str();
    return {};
}

// Both formats escape a backslash as two.
std::string unescapeBackslashes(std::string value) {
    std::string result;
    result.reserve(value.size());
    for (std::size_t i = 0; i < value.size(); ++i) {
        result.push_back(value[i]);
        if (value[i] == '\\' && i + 1 < value.size() && value[i + 1] == '\\') ++i;
    }
    return result;
}

std::vector<std::filesystem::path> steamLibraries(const std::filesystem::path& steamRoot) {
    std::vector<std::filesystem::path> libraries { steamRoot };
    const auto text = readTextFile(steamRoot / "steamapps" / "libraryfolders.vdf");
    const std::regex pathPattern("\"path\"\\s+\"([^\"]+)\"");
    for (std::sregex_iterator it(text.begin(), text.end(), pathPattern), end; it != end; ++it) {
        libraries.emplace_back(unescapeBackslashes((*it)[1].str()));
    }
    return libraries;
}

}  // namespace

GameInstall makeGameInstall(std::string name, const std::filesystem::path& root) {
    GameInstall install { std::move(name), comparablePath(root), 0 };
    if (!install.rootPrefix.empty()) {
        if (install.rootPrefix.back() != '/' && install.rootPrefix.back() != '\\') install.rootPrefix.push_back('\\');
        try { install.rootLength = root.string().size(); } catch (const std::exception&) { install.rootPrefix.clear(); }
    }
    return install;
}

std::string matchInstalledGame(const std::vector<GameInstall>& games, const std::string& processPath) {
    if (processPath.empty()) return {};
    const auto path = comparablePath(std::filesystem::path(processPath));
    if (path.empty()) return {};
    std::size_t bestLength = 0;
    const std::string* bestName = nullptr;
    for (const auto& game : games) {
        if (game.rootPrefix.empty() || !path.starts_with(game.rootPrefix)) continue;
        if (game.rootLength > bestLength) {
            bestLength = game.rootLength;
            bestName = &game.name;
        }
    }
    return bestName ? *bestName : std::string();
}

bool insideLauncherLibrary(const std::vector<GameInstall>& games, const std::string& processPath) {
    if (processPath.empty()) return false;
    const auto path = comparablePath(std::filesystem::path(processPath));
    if (path.empty()) return false;
    // comparablePath normalizes separators to backslashes on Windows.
    constexpr std::string_view steamCommon = "\\steamapps\\common\\";
    if (path.find(steamCommon) != std::string::npos) return true;
    for (const auto& game : games) {
        if (game.rootPrefix.empty() || game.rootPrefix.find(steamCommon) != std::string::npos) continue;
        // The folder holding this install, with its trailing separator.
        const auto withoutSlash = game.rootPrefix.substr(0, game.rootPrefix.size() - 1);
        const auto parentEnd = withoutSlash.find_last_of("\\/");
        if (parentEnd == std::string::npos) continue;
        if (path.starts_with(game.rootPrefix.substr(0, parentEnd + 1))) return true;
    }
    return false;
}

LauncherRoots LauncherRoots::fromEnvironment() {
    const char* programFilesX86 = std::getenv("ProgramFiles(x86)");
    const char* programData = std::getenv("ProgramData");
    LauncherRoots roots;
    roots.steamRoot = programFilesX86 ? std::filesystem::path(programFilesX86) / "Steam"
                                      : std::filesystem::path("C:\\Program Files (x86)\\Steam");
    roots.epicManifests = (programData ? std::filesystem::path(programData) : std::filesystem::path("C:\\ProgramData")) /
        "Epic" / "EpicGamesLauncher" / "Data" / "Manifests";
    return roots;
}

std::vector<GameInstall> scanInstalledGames(const LauncherRoots& roots) {
    std::vector<GameInstall> games;
    std::error_code ignored;
    for (const auto& library : steamLibraries(roots.steamRoot)) {
        const auto steamApps = library / "steamapps";
        if (!std::filesystem::exists(steamApps, ignored)) continue;
        for (const auto& entry : std::filesystem::directory_iterator(steamApps, ignored)) {
            if (!entry.is_regular_file(ignored) || entry.path().filename().string().rfind("appmanifest_", 0) != 0) continue;
            const auto text = readTextFile(entry.path());
            const auto name = extractQuotedField(text, "name");
            const auto installDir = extractQuotedField(text, "installdir");
            if (!name.empty() && !installDir.empty()) {
                games.push_back(makeGameInstall(name, steamApps / "common" / unescapeBackslashes(installDir)));
            }
        }
    }

    if (std::filesystem::exists(roots.epicManifests, ignored)) {
        for (const auto& entry : std::filesystem::directory_iterator(roots.epicManifests, ignored)) {
            if (!entry.is_regular_file(ignored) || entry.path().extension() != ".item") continue;
            const auto text = readTextFile(entry.path());
            const auto name = extractQuotedField(text, "DisplayName");
            const auto installLocation = extractQuotedField(text, "InstallLocation");
            if (!name.empty() && !installLocation.empty()) {
                games.push_back(makeGameInstall(name, unescapeBackslashes(installLocation)));
            }
        }
    }
    return games;
}

InstalledGamesCatalog::InstalledGamesCatalog()
    : InstalledGamesCatalog([] { return scanInstalledGames(LauncherRoots::fromEnvironment()); }) {}

InstalledGamesCatalog::InstalledGamesCatalog(Scanner scan) : scan_(std::move(scan)) {}

std::string InstalledGamesCatalog::match(const std::string& processPath) {
    std::lock_guard lock(mutex_);
    if (!scanned_) {
        games_ = scan_();
        scanned_ = true;
    }
    auto name = matchInstalledGame(games_, processPath);
    if (!name.empty() || !insideLauncherLibrary(games_, processPath)) return name;
    // Installed after the lists were read: read them again, once per executable.
    if (!rescannedFor_.insert(comparablePath(std::filesystem::path(processPath))).second) return name;
    games_ = scan_();
    return matchInstalledGame(games_, processPath);
}

}  // namespace clipture