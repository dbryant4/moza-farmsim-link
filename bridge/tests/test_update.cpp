// Version comparison, reading GitHub's release document, and keeping the game's mod current.

#include "mod_sync.hpp"
#include "update_check.hpp"

#include <cstdio>
#include <filesystem>
#include <fstream>
#include <string>

namespace {

int g_failures = 0;

#define CHECK(condition)                                                              \
    do                                                                                \
    {                                                                                 \
        if (!(condition))                                                             \
        {                                                                             \
            std::printf("FAILED %s:%d: %s\n", __FILE__, __LINE__, #condition);        \
            ++g_failures;                                                             \
        }                                                                             \
    } while (false)

using bridge::ModSync;
using bridge::Version;

void testVersions()
{
    CHECK(bridge::parseVersion("0.1.2") == Version({0, 1, 2}));
    CHECK(bridge::parseVersion("v0.1.2") == Version({0, 1, 2}));
    CHECK(bridge::parseVersion("V12.0.345") == Version({12, 0, 345}));
    CHECK(!bridge::parseVersion(""));
    CHECK(!bridge::parseVersion("v"));
    CHECK(!bridge::parseVersion("dev"));
    CHECK(!bridge::parseVersion("0.1"));
    CHECK(!bridge::parseVersion("0.1.2.0"));
    CHECK(!bridge::parseVersion("0.1.2-rc1"));
    CHECK(!bridge::parseVersion("0.1.x"));
    CHECK(!bridge::parseVersion("0.-1.2"));
    CHECK(!bridge::parseVersion(" 0.1.2"));
    CHECK(!bridge::parseVersion("0.1.99999999999999999999"));

    // Compared as numbers, part by part: 0.10.0 is after 0.9.9, which comparing the text gets wrong.
    CHECK(Version({0, 10, 0}) > Version({0, 9, 9}));
    CHECK(Version({1, 0, 0}) > Version({0, 99, 99}));
    CHECK(Version({0, 1, 3}) > Version({0, 1, 2}));
    CHECK(!(Version({0, 1, 2}) > Version({0, 1, 2})));
    CHECK(!(Version({0, 1, 1}) > Version({0, 1, 2})));
    CHECK(Version({0, 1, 2}).text() == "0.1.2");
}

// Cut down from what api.github.com/repos/<owner>/<name>/releases/latest answers, keeping its shape.
const char* const kRelease = R"({
  "url": "https://api.github.com/repos/dbryant4/moza-farmsim-link/releases/1",
  "html_url": "https://github.com/dbryant4/moza-farmsim-link/releases/tag/v0.1.3",
  "id": 1,
  "author": {"login": "dbryant4", "id": 7, "site_admin": false},
  "tag_name": "v0.1.3",
  "name": "0.1.3: \"quoted\" and a \\ backslash and é",
  "draft": false,
  "prerelease": false,
  "created_at": "2026-10-11T10:00:00Z",
  "assets": [
    {
      "name": "moza-farmsim-link-0.1.3-windows.zip",
      "size": 1500000,
      "digest": "sha256:1111111111111111111111111111111111111111111111111111111111111111",
      "browser_download_url": "https://github.com/dbryant4/moza-farmsim-link/releases/download/v0.1.3/moza-farmsim-link-0.1.3-windows.zip"
    },
    {
      "name": "moza-farmsim-link-0.1.3-setup.exe",
      "size": 821255,
      "uploader": {"login": "dbryant4", "nested": [1, 2.5e3, -4, null, true, {"deep": []}]},
      "digest": "sha256:0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef",
      "browser_download_url": "https://github.com/dbryant4/moza-farmsim-link/releases/download/v0.1.3/moza-farmsim-link-0.1.3-setup.exe"
    }
  ],
  "body": "## Overview\r\nLine two with a } brace and a ] bracket.\n"
})";

std::string replaced(std::string text, const std::string& from, const std::string& to)
{
    const auto at = text.find(from);
    if (at == std::string::npos)
    {
        std::printf("FAILED: the test document has no %s\n", from.c_str());
        ++g_failures;
        return text;
    }
    return text.replace(at, from.size(), to);
}

void testRelease()
{
    const auto release = bridge::parseLatestRelease(kRelease);
    CHECK(release.has_value());
    if (release)
    {
        CHECK(release->version == Version({0, 1, 3}));
        CHECK(release->pageUrl == "https://github.com/dbryant4/moza-farmsim-link/releases/tag/v0.1.3");
        CHECK(release->installerName == "moza-farmsim-link-0.1.3-setup.exe");
        CHECK(release->installerUrl ==
              "https://github.com/dbryant4/moza-farmsim-link/releases/download/v0.1.3/moza-farmsim-link-0.1.3-setup.exe");
        CHECK(release->installerSha256 == "0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef");
        CHECK(bridge::isReleaseDownload(release->installerUrl, "dbryant4/moza-farmsim-link"));
    }

    // Not something to update to.
    CHECK(!bridge::parseLatestRelease(replaced(kRelease, "\"draft\": false", "\"draft\": true")));
    CHECK(!bridge::parseLatestRelease(replaced(kRelease, "\"prerelease\": false", "\"prerelease\": true")));
    CHECK(!bridge::parseLatestRelease(replaced(kRelease, "\"v0.1.3\"", "\"nightly\"")));
    CHECK(!bridge::parseLatestRelease(replaced(kRelease, "\"v0.1.3\"", "\"v0.1.3-beta\"")));
    // What GitHub says when there is no release, or when asked too often.
    CHECK(!bridge::parseLatestRelease(R"({"message": "Not Found", "documentation_url": "https://docs.github.com/rest", "status": "404"})"));
    // Not JSON, cut short, or with something after the end.
    CHECK(!bridge::parseLatestRelease(""));
    CHECK(!bridge::parseLatestRelease("<html><body>Sign in to the hotel's Wi-Fi</body></html>"));
    CHECK(!bridge::parseLatestRelease(std::string(kRelease).substr(0, 400)));
    CHECK(!bridge::parseLatestRelease(std::string(kRelease) + "x"));
    CHECK(!bridge::parseLatestRelease("[" + std::string(kRelease) + "]"));
    CHECK(!bridge::parseLatestRelease(std::string(5000, '[')));

    // A release without an installer is still a release: the user is sent to its page.
    const auto zipOnly = bridge::parseLatestRelease(replaced(kRelease, "0.1.3-setup.exe\",\n      \"size", "0.1.3-other.bin\",\n      \"size"));
    CHECK(zipOnly.has_value());
    if (zipOnly)
    {
        CHECK(zipOnly->version == Version({0, 1, 3}));
        CHECK(zipOnly->installerUrl.empty());
    }
    const auto noAssets = bridge::parseLatestRelease(R"({"tag_name": "v2.0.0", "html_url": "https://github.com/o/r/releases/tag/v2.0.0"})");
    CHECK(noAssets.has_value() && noAssets->version == Version({2, 0, 0}) && noAssets->installerUrl.empty());

    // An installer that is not served by GitHub over https is not taken.
    const auto elsewhere = bridge::parseLatestRelease(
        replaced(kRelease, "https://github.com/dbryant4/moza-farmsim-link/releases/download/v0.1.3/moza-farmsim-link-0.1.3-setup.exe",
                 "https://example.com/moza-farmsim-link-0.1.3-setup.exe"));
    CHECK(elsewhere.has_value() && elsewhere->installerUrl.empty());
    const auto plain = bridge::parseLatestRelease(
        replaced(kRelease, "https://github.com/dbryant4/moza-farmsim-link/releases/download/v0.1.3/moza-farmsim-link-0.1.3-setup.exe",
                 "http://github.com/dbryant4/moza-farmsim-link/releases/download/v0.1.3/moza-farmsim-link-0.1.3-setup.exe"));
    CHECK(plain.has_value() && plain->installerUrl.empty());

    // A checksum in a form this does not understand is treated as no checksum, not as a match.
    const auto oddDigest = bridge::parseLatestRelease(
        replaced(kRelease, "sha256:0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef", "md5:0123456789abcdef0123456789abcdef"));
    CHECK(oddDigest.has_value() && !oddDigest->installerUrl.empty() && oddDigest->installerSha256.empty());
}

void testDownloadRules()
{
    const char* const repository = "dbryant4/moza-farmsim-link";
    CHECK(bridge::isReleaseDownload("https://github.com/dbryant4/moza-farmsim-link/releases/download/v0.1.3/a-setup.exe", repository));
    CHECK(!bridge::isReleaseDownload("https://github.com/someone-else/moza-farmsim-link/releases/download/v0.1.3/a-setup.exe", repository));
    CHECK(!bridge::isReleaseDownload("https://github.com/dbryant4/moza-farmsim-link-evil/releases/download/v1/a-setup.exe", repository));
    CHECK(!bridge::isReleaseDownload("https://github.com.example.com/dbryant4/moza-farmsim-link/releases/download/v1/a-setup.exe", repository));
    CHECK(!bridge::isReleaseDownload("http://github.com/dbryant4/moza-farmsim-link/releases/download/v0.1.3/a-setup.exe", repository));
    CHECK(!bridge::isReleaseDownload("https://github.com/dbryant4/moza-farmsim-link/releases/download/../../../x/a-setup.exe", repository));
    CHECK(!bridge::isReleaseDownload("https://github.com/dbryant4/moza-farmsim-link/releases/download/v1/a setup.exe", repository));
    CHECK(!bridge::isReleaseDownload("https://github.com/dbryant4/moza-farmsim-link/releases/download/", repository));
    CHECK(!bridge::isReleaseDownload("", repository));
    CHECK(!bridge::isReleaseDownload("https://github.com//releases/download/v1/a-setup.exe", ""));

    CHECK(bridge::safeFileName("moza-farmsim-link-0.1.3-setup.exe") == "moza-farmsim-link-0.1.3-setup.exe");
    CHECK(bridge::safeFileName("").empty());
    CHECK(bridge::safeFileName("..\\..\\evil-setup.exe").empty());
    CHECK(bridge::safeFileName("../evil-setup.exe").empty());
    CHECK(bridge::safeFileName("C:evil-setup.exe").empty());
    CHECK(bridge::safeFileName("a b-setup.exe").empty());
    CHECK(bridge::safeFileName(".hidden-setup.exe").empty());
    CHECK(bridge::safeFileName("a\"&calc&\"-setup.exe").empty());
    CHECK(bridge::safeFileName(std::string(200, 'a') + "-setup.exe").empty());
}

void write(const std::filesystem::path& path, const std::string& content)
{
    std::ofstream(path, std::ios::binary) << content;
}

std::string read(const std::filesystem::path& path)
{
    std::ifstream file(path, std::ios::binary);
    return std::string(std::istreambuf_iterator<char>(file), std::istreambuf_iterator<char>());
}

void testModSync()
{
    namespace fs = std::filesystem;
    const fs::path root = fs::temp_directory_path() / "moza_fs_bridge_test_update";
    fs::remove_all(root);
    fs::create_directories(root / "app");
    const fs::path shipped   = root / "app" / "FS25_MozaYokeLink.zip";
    const fs::path game      = root / "FarmingSimulator2025";
    const fs::path installed = game / "mods" / "FS25_MozaYokeLink.zip";

    // Nothing to install from, then no game to install into: neither creates anything.
    CHECK(bridge::syncMod(shipped, game, false) == ModSync::NoSource);
    write(shipped, "new mod");
    CHECK(bridge::syncMod(shipped, game, false) == ModSync::NoGameFolder);
    CHECK(!fs::exists(game));
    CHECK(bridge::syncMod(shipped, {}, false) == ModSync::NoGameFolder);

    // The game is there but has never had a mod: the mods folder is made.
    fs::create_directories(game);
    CHECK(bridge::syncMod(shipped, game, false) == ModSync::Updated);
    CHECK(read(installed) == "new mod");
    CHECK(bridge::syncMod(shipped, game, false) == ModSync::UpToDate);
    CHECK(bridge::syncMod(shipped, game, true) == ModSync::UpToDate);

    // A different mod is installed (same size, to show that it is the content that is compared).
    write(installed, "old mod");
    write(game / "mods" / "FS25_SomeoneElses.zip", "not ours");
    CHECK(bridge::syncMod(shipped, game, true) == ModSync::GameRunning);
    CHECK(read(installed) == "old mod");
    CHECK(bridge::syncMod(shipped, game, false) == ModSync::Updated);
    CHECK(read(installed) == "new mod");
    CHECK(read(game / "mods" / "FS25_SomeoneElses.zip") == "not ours");
    // Nothing is left behind beside it.
    int files = 0;
    for (const auto& entry : fs::directory_iterator(game / "mods"))
    {
        (void)entry;
        ++files;
    }
    CHECK(files == 2);

    CHECK(std::string(bridge::describe(ModSync::Updated)).size() > 0);
    fs::remove_all(root);
}

} // namespace

int main()
{
    testVersions();
    testRelease();
    testDownloadRules();
    testModSync();
    if (g_failures != 0)
    {
        std::printf("%d check(s) failed\n", g_failures);
        return 1;
    }
    std::puts("update tests passed");
    return 0;
}
