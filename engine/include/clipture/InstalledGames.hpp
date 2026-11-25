#pragma once

// Games installed through Steam and the Epic Games Launcher, matched to a
// running process by its install folder. They name a game and back up the
// Windows game list (platform/windows/WindowsGameList.hpp), which knows games
// from any launcher once they have run.
//
// Read on first use, and again only when a process the lists do not know runs
// from inside a launcher's library (a game installed after Clipture started):
// once per such executable, so an unknown tool in a library never rescans
// twice. Nothing is read on a timer.

#include <filesystem>
#include <functional>
#include <mutex>
#include <string>
#include <unordered_set>
#include <vector>

namespace clipture {

struct GameInstall {
    std::string name;
    // Lower-case normalized install folder with a trailing separator, compared
    // as a prefix; empty when the folder is unknown.
    std::string rootPrefix;
    // Length of the folder as its manifest wrote it: the longest match wins.
    std::size_t rootLength = 0;
};

GameInstall makeGameInstall(std::string name, const std::filesystem::path& root);

// The game whose install folder holds the process, or empty.
std::string matchInstalledGame(const std::vector<GameInstall>& games, const std::string& processPath);

// Whether the process runs from where a launcher installs games: any Steam
// library (its layout is always ...\steamapps\common\...) or a folder that
// already holds an install outside Steam (Epic's, e.g. C:\Program Files\Epic Games).
bool insideLauncherLibrary(const std::vector<GameInstall>& games, const std::string& processPath);

// Where the launchers keep their manifests.
struct LauncherRoots {
    std::filesystem::path steamRoot;
    std::filesystem::path epicManifests;

    static LauncherRoots fromEnvironment();
};

std::vector<GameInstall> scanInstalledGames(const LauncherRoots& roots);

// The manifests of this PC, scanned on first use.
class InstalledGamesCatalog {
public:
    using Scanner = std::function<std::vector<GameInstall>()>;

    InstalledGamesCatalog();
    explicit InstalledGamesCatalog(Scanner scan);

    std::string match(const std::string& processPath);

private:
    Scanner scan_;
    std::mutex mutex_;
    bool scanned_ = false;
    std::vector<GameInstall> games_;
    // Executables a miss already rescanned for.
    std::unordered_set<std::string> rescannedFor_;
};

}  // namespace clipture
