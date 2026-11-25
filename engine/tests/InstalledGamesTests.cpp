// Installed games: the Windows game list, launcher manifests, and matching a
// process to its game.
#include "clipture/InstalledGames.hpp"
#include "clipture/platform/windows/WindowsGameList.hpp"

#include <Windows.h>

#include <algorithm>
#include <fstream>
#include <iostream>
#include <stdexcept>
#include <string>

using namespace clipture;

namespace {

void require(bool value, const std::string& message) {
    if (!value) throw std::runtime_error(message);
}

void writeFile(const std::filesystem::path& path, const std::string& text) {
    std::filesystem::create_directories(path.parent_path());
    std::ofstream(path, std::ios::binary) << text;
}

std::string jsonEscaped(const std::filesystem::path& path) {
    std::string escaped;
    for (const char ch : path.string()) {
        escaped.push_back(ch);
        if (ch == '\\') escaped.push_back('\\');
    }
    return escaped;
}

void matchesTheLongestInstallFolder() {
    const std::vector<GameInstall> games = {
        makeGameInstall("Launcher", "C:\\Games"),
        makeGameInstall("Grand Theft Auto V Enhanced", "C:\\Games\\GTAVEnhanced"),
    };
    require(matchInstalledGame(games, "c:\\games\\gtavenhanced\\GTA5_Enhanced.exe") == "Grand Theft Auto V Enhanced",
        "the deeper install folder wins, whatever the case");
    require(matchInstalledGame(games, "C:\\Games\\GTAVEnhanced2\\other.exe") == "Launcher",
        "a sibling folder sharing a prefix is not the game");
    require(matchInstalledGame(games, "D:\\Tools\\tool.exe").empty(), "a process outside every install is no game");
    require(matchInstalledGame(games, "").empty(), "an unreadable path is no game");
}

void readsEpicAndSteamManifests() {
    const auto base = std::filesystem::temp_directory_path() /
        ("clipture-installed-games-" + std::to_string(GetCurrentProcessId()));
    std::filesystem::remove_all(base);
    LauncherRoots roots { base / "Steam", base / "Epic" / "Manifests" };

    const auto gta = base / "Epic Games" / "GTAVEnhanced";
    writeFile(roots.epicManifests / "A.item",
        "{\n\t\"DisplayName\": \"Grand Theft Auto V Enhanced\",\n\t\"InstallLocation\": \"" + jsonEscaped(gta) + "\"\n}\n");
    writeFile(roots.epicManifests / "notes.txt", "\"DisplayName\": \"Not a manifest\"");
    const auto library = base / "SteamLibrary";
    writeFile(roots.steamRoot / "steamapps" / "libraryfolders.vdf",
        "\"libraryfolders\"\n{\n\t\"1\"\n\t{\n\t\t\"path\"\t\t\"" + jsonEscaped(library) + "\"\n\t}\n}\n");
    writeFile(library / "steamapps" / "appmanifest_730.acf",
        "\"AppState\"\n{\n\t\"name\"\t\t\"Counter-Strike 2\"\n\t\"installdir\"\t\t\"Counter-Strike Global Offensive\"\n}\n");

    const auto games = scanInstalledGames(roots);
    std::filesystem::remove_all(base);
    require(games.size() == 2, "one Epic and one Steam game");
    require(matchInstalledGame(games, (gta / "GTA5_Enhanced.exe").string()) == "Grand Theft Auto V Enhanced",
        "an Epic install folder written with escaped backslashes matches");
    const auto cs2 = library / "steamapps" / "common" / "Counter-Strike Global Offensive" / "game" / "bin" / "win64" / "cs2.exe";
    require(matchInstalledGame(games, cs2.string()) == "Counter-Strike 2", "a game in a second Steam library matches");
}

void catalogScansOnce() {
    int scans = 0;
    InstalledGamesCatalog catalog([&] {
        ++scans;
        return std::vector<GameInstall> { makeGameInstall("Game", "C:\\Games\\Game") };
    });
    require(scans == 0, "nothing is read before it is needed");
    require(catalog.match("C:\\Games\\Game\\game.exe") == "Game", "the first match scans");
    require(catalog.match("C:\\Other\\app.exe").empty() && scans == 1, "later matches reuse the scan");
}

void rescansOnlyForUnknownGamesInLibraries() {
    int scans = 0;
    std::vector<GameInstall> installed {
        makeGameInstall("Old Steam Game", "D:\\SteamLibrary\\steamapps\\common\\Old"),
        makeGameInstall("Epic Game", "C:\\Program Files\\Epic Games\\EpicGame"),
    };
    InstalledGamesCatalog catalog([&] { ++scans; return installed; });
    require(catalog.match("C:\\Windows\\notepad.exe").empty() && scans == 1, "an app outside every library never rescans");

    // Installed while Clipture ran: in a Steam library, not in the first read.
    installed.push_back(makeGameInstall("New Steam Game", "E:\\SteamLibrary\\steamapps\\common\\New"));
    require(catalog.match("E:\\SteamLibrary\\steamapps\\common\\New\\new.exe") == "New Steam Game" && scans == 2,
        "a miss inside a Steam library (even one added later) rescans and finds the new game");
    installed.push_back(makeGameInstall("New Epic Game", "C:\\Program Files\\Epic Games\\NewEpic"));
    require(catalog.match("C:\\Program Files\\Epic Games\\NewEpic\\Binaries\\game.exe") == "New Epic Game" && scans == 3,
        "a miss beside an Epic install rescans");

    const std::string tool = "D:\\SteamLibrary\\steamapps\\common\\Steamworks Shared\\tool.exe";
    require(catalog.match(tool).empty() && scans == 4, "an unknown tool in a library rescans once");
    require(catalog.match(tool).empty() && scans == 4, "and never again");
    require(catalog.match("D:\\SteamLibrary\\steamapps\\common\\Old\\old.exe") == "Old Steam Game" && scans == 4,
        "a known game never rescans");
}

// A scratch copy of the Game Bar key under HKCU, removed afterwards.
struct ScratchGameKey {
    std::wstring path = L"Software\\Clipture-tests\\GameConfigStore-" + std::to_wstring(GetCurrentProcessId());

    ScratchGameKey() {
        HKEY key = nullptr;
        RegCreateKeyExW(HKEY_CURRENT_USER, path.c_str(), 0, nullptr, 0, KEY_ALL_ACCESS, nullptr, &key, nullptr);
        RegCloseKey(key);
    }
    ~ScratchGameKey() {
        RegDeleteTreeW(HKEY_CURRENT_USER, path.c_str());
        RegDeleteKeyW(HKEY_CURRENT_USER, L"Software\\Clipture-tests");
    }

    void addEntry(const std::wstring& id, const std::wstring& exePath, const wchar_t* titleId) const {
        HKEY key = nullptr;
        RegCreateKeyExW(HKEY_CURRENT_USER, (path + L"\\" + id).c_str(), 0, nullptr, 0, KEY_ALL_ACCESS, nullptr, &key, nullptr);
        RegSetValueExW(key, L"MatchedExeFullPath", 0, REG_SZ, reinterpret_cast<const BYTE*>(exePath.c_str()),
            static_cast<DWORD>((exePath.size() + 1) * sizeof(wchar_t)));
        if (titleId) {
            const std::wstring title(titleId);
            RegSetValueExW(key, L"TitleId", 0, REG_SZ, reinterpret_cast<const BYTE*>(title.c_str()),
                static_cast<DWORD>((title.size() + 1) * sizeof(wchar_t)));
        }
        RegCloseKey(key);
    }
};

// Windows signals the change asynchronously; a short wait covers delivery.
bool refreshedWithin(platform::WindowsGameList& list, int milliseconds) {
    for (int waited = 0; waited <= milliseconds; waited += 10) {
        if (list.refreshIfChanged()) return true;
        Sleep(10);
    }
    return false;
}

void windowsListKnowsMatchedGamesOnly() {
    ScratchGameKey scratch;
    scratch.addEntry(L"a", L"C:\\Program Files\\Epic Games\\GTAVEnhanced\\GTA5_Enhanced.exe", L"1815914144");
    scratch.addEntry(L"b", L"C:\\Users\\me\\AppData\\Local\\Discord\\app-1.0\\Discord.exe", nullptr);

    platform::WindowsGameList list(scratch.path);
    require(list.size() == 1, "only the entry Windows matched to a game counts");
    require(list.contains("c:\\program files\\epic games\\gtavenhanced\\gta5_enhanced.exe"), "matched whatever the case");
    require(!list.contains("C:\\Users\\me\\AppData\\Local\\Discord\\app-1.0\\Discord.exe"),
        "an entry without a TitleId is an app marked by hand, not a game");
    require(!list.contains(""), "an unreadable path is no game");
    require(!list.refreshIfChanged(), "nothing changed, nothing reread");

    scratch.addEntry(L"c", L"D:\\Games\\New Game\\NewGame.exe", L"42");
    require(refreshedWithin(list, 2000), "Windows signals a new game");
    // Writing an entry takes several registry writes, each signalled; every
    // signal rereads, so a read that caught it half written is corrected.
    while (refreshedWithin(list, 100)) {}
    require(list.contains("D:\\Games\\New Game\\NewGame.exe"), "a game listed after start is known");
    require(!list.refreshIfChanged(), "and the list settles again");
}

void missingWindowsListIsEmpty() {
    platform::WindowsGameList list(L"Software\\Clipture-tests\\does-not-exist");
    require(list.size() == 0 && !list.contains("C:\\Games\\game.exe"), "no Game Bar list, no games from it");
    require(!list.refreshIfChanged(), "and nothing to watch");
}

}  // namespace

int main() {
    try {
        matchesTheLongestInstallFolder();
        readsEpicAndSteamManifests();
        catalogScansOnce();
        rescansOnlyForUnknownGamesInLibraries();
        windowsListKnowsMatchedGamesOnly();
        missingWindowsListIsEmpty();
    } catch (const std::exception& error) {
        std::cerr << "FAILED: " << error.what() << std::endl;
        return 1;
    }
    std::cout << "installed games tests passed" << std::endl;
    return 0;
}
